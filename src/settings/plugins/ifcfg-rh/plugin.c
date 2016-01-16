/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager system settings service
 *
 * Dan Williams <dcbw@redhat.com>
 * Søren Sandmann <sandmann@daimi.au.dk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright (C) 2007 - 2011 Red Hat, Inc.
 */

#include <config.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <net/ethernet.h>
#include <netinet/ether.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <gmodule.h>
#include <glib-object.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>

#if HAVE_SELINUX
#include <selinux/selinux.h>
#endif

#include <nm-setting-connection.h>

#include "common.h"
#include "nm-dbus-glib-types.h"
#include "plugin.h"
#include "nm-system-config-interface.h"
#include "nm-settings-error.h"
#include "nm-config.h"
#include "nm-logging.h"

#include "nm-ifcfg-connection.h"
#include "nm-inotify-helper.h"
#include "shvar.h"
#include "reader.h"
#include "writer.h"
#include "utils.h"

#define DBUS_SERVICE_NAME "com.redhat.ifcfgrh1"
#define DBUS_OBJECT_PATH "/com/redhat/ifcfgrh1"

static gboolean impl_ifcfgrh_get_ifcfg_details (SCPluginIfcfg *plugin,
                                                const char *in_ifcfg,
                                                const char **out_uuid,
                                                const char **out_path,
                                                GError **error);

#include "nm-ifcfg-rh-glue.h"

static void connection_new_or_changed (SCPluginIfcfg *plugin,
                                       const char *path,
                                       NMIfcfgConnection *existing,
                                       char **out_old_path);

static void system_config_interface_init (NMSystemConfigInterface *system_config_interface_class);

G_DEFINE_TYPE_EXTENDED (SCPluginIfcfg, sc_plugin_ifcfg, G_TYPE_OBJECT, 0,
						G_IMPLEMENT_INTERFACE (NM_TYPE_SYSTEM_CONFIG_INTERFACE,
											   system_config_interface_init))

#define SC_PLUGIN_IFCFG_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), SC_TYPE_PLUGIN_IFCFG, SCPluginIfcfgPrivate))


typedef struct {
	GHashTable *connections;  /* uuid::connection */

	gboolean initialized;
	gulong ih_event_id;
	int sc_network_wd;
	GFileMonitor *hostname_monitor;
	guint hostname_monitor_id;
	char *hostname;

	GFileMonitor *ifcfg_monitor;
	guint ifcfg_monitor_id;

	DBusGConnection *bus;
} SCPluginIfcfgPrivate;


static void
connection_ifcfg_changed (NMIfcfgConnection *connection, gpointer user_data)
{
	SCPluginIfcfg *plugin = SC_PLUGIN_IFCFG (user_data);
	const char *path;

	path = nm_ifcfg_connection_get_path (connection);
	g_return_if_fail (path != NULL);

	connection_new_or_changed (plugin, path, connection, NULL);
}

static void
connection_removed_cb (NMSettingsConnection *obj, gpointer user_data)
{
	g_hash_table_remove (SC_PLUGIN_IFCFG_GET_PRIVATE (user_data)->connections,
	                     nm_connection_get_uuid (NM_CONNECTION (obj)));
}

static NMIfcfgConnection *
_internal_new_connection (SCPluginIfcfg *self,
                          const char *path,
                          NMConnection *source,
                          GError **error)
{
	SCPluginIfcfgPrivate *priv = SC_PLUGIN_IFCFG_GET_PRIVATE (self);
	NMIfcfgConnection *connection;
	const char *cid;
	GError *local = NULL;
	gboolean ignore_error = FALSE;

	if (!source)
		nm_log_info (LOGD_SETTINGS, "parsing %s ... ", path);

	connection = nm_ifcfg_connection_new (source, path, &local, &ignore_error);
	if (!connection) {
		if (!ignore_error)
			nm_log_warn (LOGD_SETTINGS, "    %s", (local && local->message) ? local->message : "(unknown)");
		if (local)
			g_propagate_error (error, local);
		else
			g_set_error (error, IFCFG_PLUGIN_ERROR, 0, "(unknown)");
		return NULL;
	}

	cid = nm_connection_get_id (NM_CONNECTION (connection));
	g_assert (cid);

	g_hash_table_insert (priv->connections,
	                     g_strdup (nm_connection_get_uuid (NM_CONNECTION (connection))),
	                     connection);
	nm_log_info (LOGD_SETTINGS, "    read connection '%s'", cid);
	g_signal_connect (connection, NM_SETTINGS_CONNECTION_REMOVED,
	                  G_CALLBACK (connection_removed_cb),
	                  self);

	if (nm_ifcfg_connection_get_unmanaged_spec (connection)) {
		const char *spec;
		const char *device_id;

		spec = nm_ifcfg_connection_get_unmanaged_spec (connection);
		device_id = strchr (spec, ':');
		if (device_id)
			device_id++;
		else
			device_id = spec;
		nm_log_warn (LOGD_SETTINGS, "    Ignoring connection '%s' / device '%s' due to NM_CONTROLLED=no.",
		             cid, device_id);
	} else if (nm_ifcfg_connection_get_unrecognized_spec (connection)) {
		nm_log_warn (LOGD_SETTINGS, "    Ignoring connection '%s' of unrecognized type.", cid);
	}

	/* watch changes of ifcfg hardlinks */
	g_signal_connect (G_OBJECT (connection), "ifcfg-changed",
	                  G_CALLBACK (connection_ifcfg_changed), self);

	return connection;
}

/* Monitoring */

static void
remove_connection (SCPluginIfcfg *self, NMIfcfgConnection *connection)
{
	SCPluginIfcfgPrivate *priv = SC_PLUGIN_IFCFG_GET_PRIVATE (self);
	gboolean unmanaged, unrecognized;

	g_return_if_fail (self != NULL);
	g_return_if_fail (connection != NULL);

	unmanaged = !!nm_ifcfg_connection_get_unmanaged_spec (connection);
	unrecognized = !!nm_ifcfg_connection_get_unrecognized_spec (connection);

	g_object_ref (connection);
	g_hash_table_remove (priv->connections, nm_connection_get_uuid (NM_CONNECTION (connection)));
	nm_settings_connection_signal_remove (NM_SETTINGS_CONNECTION (connection));
	g_object_unref (connection);

	/* Emit changes _after_ removing the connection */
	if (unmanaged)
		g_signal_emit_by_name (self, NM_SYSTEM_CONFIG_INTERFACE_UNMANAGED_SPECS_CHANGED);
	if (unrecognized)
		g_signal_emit_by_name (self, NM_SYSTEM_CONFIG_INTERFACE_UNRECOGNIZED_SPECS_CHANGED);
}

static NMIfcfgConnection *
find_by_path (SCPluginIfcfg *self, const char *path)
{
	SCPluginIfcfgPrivate *priv = SC_PLUGIN_IFCFG_GET_PRIVATE (self);
	GHashTableIter iter;
	NMIfcfgConnection *candidate = NULL;

	g_return_val_if_fail (path != NULL, NULL);

	g_hash_table_iter_init (&iter, priv->connections);
	while (g_hash_table_iter_next (&iter, NULL, (gpointer) &candidate)) {
		if (g_strcmp0 (path, nm_ifcfg_connection_get_path (candidate)) == 0)
			return candidate;
	}
	return NULL;
}

static NMIfcfgConnection *
find_by_uuid_from_path (SCPluginIfcfg *self, const char *path)
{
	SCPluginIfcfgPrivate *priv = SC_PLUGIN_IFCFG_GET_PRIVATE (self);
	char *uuid;

	g_return_val_if_fail (path != NULL, NULL);

	uuid = uuid_from_file (path);
	if (uuid)
		return g_hash_table_lookup (priv->connections, uuid);
	else
		return NULL;
}

static void
connection_new_or_changed (SCPluginIfcfg *self,
                           const char *path,
                           NMIfcfgConnection *existing,
                           char **out_old_path)
{
	SCPluginIfcfgPrivate *priv = SC_PLUGIN_IFCFG_GET_PRIVATE (self);
	NMIfcfgConnection *new;
	GError *error = NULL;
	gboolean ignore_error = FALSE;
	const char *new_unmanaged = NULL, *old_unmanaged = NULL;
	const char *new_unrecognized = NULL, *old_unrecognized = NULL;
	gboolean unmanaged_changed, unrecognized_changed;

	g_return_if_fail (self != NULL);
	g_return_if_fail (path != NULL);

	if (out_old_path)
		*out_old_path = NULL;

	if (!existing) {
		/* See if it's a rename */
		existing = find_by_uuid_from_path (self, path);
		if (existing) {
			const char *old_path = nm_ifcfg_connection_get_path (existing);
			nm_log_info (LOGD_SETTINGS, "renaming %s -> %s", old_path, path);
			if (out_old_path)
				*out_old_path = g_strdup (old_path);
			nm_ifcfg_connection_set_path (existing, path);
		}
	}

	if (!existing) {
		/* New connection */
		new = _internal_new_connection (self, path, NULL, NULL);
		if (new) {
			if (nm_ifcfg_connection_get_unmanaged_spec (new))
				g_signal_emit_by_name (self, NM_SYSTEM_CONFIG_INTERFACE_UNMANAGED_SPECS_CHANGED);
			else if (nm_ifcfg_connection_get_unrecognized_spec (new))
				g_signal_emit_by_name (self, NM_SYSTEM_CONFIG_INTERFACE_UNRECOGNIZED_SPECS_CHANGED);
			else
				g_signal_emit_by_name (self, NM_SYSTEM_CONFIG_INTERFACE_CONNECTION_ADDED, new);
		}
		return;
	}

	new = (NMIfcfgConnection *) nm_ifcfg_connection_new (NULL, path, &error, &ignore_error);
	if (!new) {
		/* errors reading connection; remove it */
		if (!ignore_error)
			nm_log_warn (LOGD_SETTINGS, "    %s", (error && error->message) ? error->message : "(unknown)");
		g_clear_error (&error);

		nm_log_info (LOGD_SETTINGS, "removed %s.", path);
		remove_connection (self, existing);
		return;
	}

	/* Successfully read connection changes */

	old_unmanaged = nm_ifcfg_connection_get_unmanaged_spec (NM_IFCFG_CONNECTION (existing));
	new_unmanaged = nm_ifcfg_connection_get_unmanaged_spec (NM_IFCFG_CONNECTION (new));
	unmanaged_changed = g_strcmp0 (old_unmanaged, new_unmanaged);

	old_unrecognized = nm_ifcfg_connection_get_unrecognized_spec (NM_IFCFG_CONNECTION (existing));
	new_unrecognized = nm_ifcfg_connection_get_unrecognized_spec (NM_IFCFG_CONNECTION (new));
	unrecognized_changed = g_strcmp0 (old_unrecognized, new_unrecognized);

	if (   !unmanaged_changed
	    && !unrecognized_changed
	    && nm_connection_compare (NM_CONNECTION (existing),
	                              NM_CONNECTION (new),
	                              NM_SETTING_COMPARE_FLAG_IGNORE_AGENT_OWNED_SECRETS |
	                                  NM_SETTING_COMPARE_FLAG_IGNORE_NOT_SAVED_SECRETS)) {
		g_object_unref (new);
		return;
	}

	nm_log_info (LOGD_SETTINGS, "updating %s", path);
	g_object_set (existing,
	              NM_IFCFG_CONNECTION_UNMANAGED_SPEC, new_unmanaged,
	              NM_IFCFG_CONNECTION_UNRECOGNIZED_SPEC, new_unrecognized,
	              NULL);

	if (new_unmanaged || new_unrecognized) {
		if (!old_unmanaged && !old_unrecognized) {
			g_object_ref (existing);
			/* Unexport the connection by telling the settings service it's
			 * been removed.
			 */
			nm_settings_connection_signal_remove (NM_SETTINGS_CONNECTION (existing));
			/* Remove the path so that claim_connection() doesn't complain later when
			 * interface gets managed and connection is re-added. */
			nm_connection_set_path (NM_CONNECTION (existing), NULL);

			/* signal_remove() will end up removing the connection from our hash,
			 * so add it back now.
			 */
			g_hash_table_insert (priv->connections,
			                     g_strdup (nm_connection_get_uuid (NM_CONNECTION (existing))),
			                     existing);
		}
	} else {
		const char *cid = nm_connection_get_id (NM_CONNECTION (new));

		if (old_unmanaged /* && !new_unmanaged */) {
			nm_log_info (LOGD_SETTINGS, "Managing connection '%s' and its device because NM_CONTROLLED was true.", cid);
			g_signal_emit_by_name (self, NM_SYSTEM_CONFIG_INTERFACE_CONNECTION_ADDED, existing);
		} else if (old_unrecognized /* && !new_unrecognized */) {
			nm_log_info (LOGD_SETTINGS, "Managing connection '%s' because it is now a recognized type.", cid);
			g_signal_emit_by_name (self, NM_SYSTEM_CONFIG_INTERFACE_CONNECTION_ADDED, existing);
		}

		if (!nm_settings_connection_replace_settings (NM_SETTINGS_CONNECTION (existing),
		                                              NM_CONNECTION (new),
		                                              FALSE,  /* don't set Unsaved */
		                                              &error)) {
			/* Shouldn't ever get here as 'new' was verified by the reader already */
			g_assert_no_error (error);
		}
	}
	g_object_unref (new);

	if (unmanaged_changed)
		g_signal_emit_by_name (self, NM_SYSTEM_CONFIG_INTERFACE_UNMANAGED_SPECS_CHANGED);
	if (unrecognized_changed)
		g_signal_emit_by_name (self, NM_SYSTEM_CONFIG_INTERFACE_UNRECOGNIZED_SPECS_CHANGED);
}

static void
ifcfg_dir_changed (GFileMonitor *monitor,
                   GFile *file,
                   GFile *other_file,
                   GFileMonitorEvent event_type,
                   gpointer user_data)
{
	SCPluginIfcfg *plugin = SC_PLUGIN_IFCFG (user_data);
	char *path, *base, *ifcfg_path;
	NMIfcfgConnection *connection;

	path = g_file_get_path (file);
	if (utils_should_ignore_file (path, FALSE)) {
		g_free (path);
		return;
	}

	base = g_file_get_basename (file);
	if (utils_is_ifcfg_alias_file (base, NULL)) {
		/* Alias file changed. Get the base ifcfg file from it */
		ifcfg_path = utils_get_ifcfg_from_alias (path);
	} else {
		/* Given any ifcfg, keys, or routes file, get the ifcfg file path */
		ifcfg_path = utils_get_ifcfg_path (path);
	}
	if (ifcfg_path) {
		connection = find_by_path (plugin, ifcfg_path);
		switch (event_type) {
		case G_FILE_MONITOR_EVENT_DELETED:
			nm_log_info (LOGD_SETTINGS, "removed %s.", ifcfg_path);
			if (connection)
				remove_connection (plugin, connection);
			break;
		case G_FILE_MONITOR_EVENT_CREATED:
		case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
			/* Update or new */
			connection_new_or_changed (plugin, ifcfg_path, connection, NULL);
			break;
		default:
			break;
		}
		g_free (ifcfg_path);
	}
	g_free (path);
	g_free (base);
}

static void
setup_ifcfg_monitoring (SCPluginIfcfg *plugin)
{
	SCPluginIfcfgPrivate *priv = SC_PLUGIN_IFCFG_GET_PRIVATE (plugin);
	GFile *file;
	GFileMonitor *monitor;

	file = g_file_new_for_path (IFCFG_DIR "/");
	monitor = g_file_monitor_directory (file, G_FILE_MONITOR_NONE, NULL, NULL);
	g_object_unref (file);

	if (monitor) {
		priv->ifcfg_monitor_id = g_signal_connect (monitor, "changed",
		                                           G_CALLBACK (ifcfg_dir_changed), plugin);
		priv->ifcfg_monitor = monitor;
	}
}

static void
read_connections (SCPluginIfcfg *plugin)
{
	SCPluginIfcfgPrivate *priv = SC_PLUGIN_IFCFG_GET_PRIVATE (plugin);
	GDir *dir;
	GError *err = NULL;
	const char *item;
	GHashTable *oldconns;
	GHashTableIter iter;
	gpointer key, value;
	NMIfcfgConnection *connection;

	dir = g_dir_open (IFCFG_DIR, 0, &err);
	if (!dir) {
		nm_log_warn (LOGD_SETTINGS, "Could not read directory '%s': %s", IFCFG_DIR, err->message);
		g_error_free (err);
		return;
	}

	oldconns = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
	g_hash_table_iter_init (&iter, priv->connections);
	while (g_hash_table_iter_next (&iter, NULL, &value)) {
		const char *ifcfg_path = nm_ifcfg_connection_get_path (value);
		if (ifcfg_path)
			g_hash_table_insert (oldconns, g_strdup (ifcfg_path), value);
	}

	while ((item = g_dir_read_name (dir))) {
		char *full_path, *old_path;

		if (utils_should_ignore_file (item, TRUE))
			continue;
		if (utils_is_ifcfg_alias_file (item, NULL))
			continue;

		full_path = g_build_filename (IFCFG_DIR, item, NULL);
		if (!utils_get_ifcfg_name (full_path, TRUE))
			goto next;

		connection = g_hash_table_lookup (oldconns, full_path);
		g_hash_table_remove (oldconns, full_path);
		connection_new_or_changed (plugin, full_path, connection, &old_path);

		if (old_path) {
			g_hash_table_remove (oldconns, old_path);
			g_free (old_path);
		}

	next:
		g_free (full_path);
	}

	g_dir_close (dir);

	g_hash_table_iter_init (&iter, oldconns);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		nm_log_info (LOGD_SETTINGS, "removed %s.", (char *)key);
		g_hash_table_iter_remove (&iter);
		remove_connection (plugin, value);
	}

	g_hash_table_destroy (oldconns);
}

static GSList *
get_connections (NMSystemConfigInterface *config)
{
	SCPluginIfcfg *plugin = SC_PLUGIN_IFCFG (config);
	SCPluginIfcfgPrivate *priv = SC_PLUGIN_IFCFG_GET_PRIVATE (plugin);
	GSList *list = NULL;
	GHashTableIter iter;
	NMIfcfgConnection *connection;

	if (!priv->initialized) {
		if (nm_config_get_monitor_connection_files (nm_config_get ()))
			setup_ifcfg_monitoring (plugin);
		read_connections (plugin);
		priv->initialized = TRUE;
	}

	g_hash_table_iter_init (&iter, priv->connections);
	while (g_hash_table_iter_next (&iter, NULL, (gpointer) &connection)) {
		if (   !nm_ifcfg_connection_get_unmanaged_spec (connection)
		    && !nm_ifcfg_connection_get_unrecognized_spec (connection))
			list = g_slist_prepend (list, connection);
	}

	return list;
}

static gboolean
load_connection (NMSystemConfigInterface *config,
                 const char *filename)
{
	SCPluginIfcfg *plugin = SC_PLUGIN_IFCFG (config);
	NMIfcfgConnection *connection;
	int dir_len = strlen (IFCFG_DIR);

	if (   strncmp (filename, IFCFG_DIR, dir_len) != 0
	    || filename[dir_len] != '/'
	    || strchr (filename + dir_len + 1, '/') != NULL)
		return FALSE;

	if (utils_should_ignore_file (filename + dir_len + 1, TRUE))
		return FALSE;

	connection = find_by_path (plugin, filename);
	connection_new_or_changed (plugin, filename, connection, NULL);
	if (!connection)
		connection = find_by_path (plugin, filename);

	return (connection != NULL);
}

static void
reload_connections (NMSystemConfigInterface *config)
{
	SCPluginIfcfg *plugin = SC_PLUGIN_IFCFG (config);

	read_connections (plugin);
}

static GSList *
get_unhandled_specs (NMSystemConfigInterface *config,
                     const char *property)
{
	SCPluginIfcfgPrivate *priv = SC_PLUGIN_IFCFG_GET_PRIVATE (config);
	GSList *list = NULL, *list_iter;
	GHashTableIter iter;
	gpointer connection;
	char *spec;
	gboolean found;

	g_hash_table_iter_init (&iter, priv->connections);
	while (g_hash_table_iter_next (&iter, NULL, &connection)) {
		g_object_get (connection, property, &spec, NULL);
		if (spec) {
			/* Ignore duplicates */
			for (list_iter = list, found = FALSE; list_iter; list_iter = g_slist_next (list_iter)) {
				if (g_str_equal (list_iter->data, spec)) {
					found = TRUE;
					break;
				}
			}
			if (found)
				g_free (spec);
			else
				list = g_slist_prepend (list, spec);
		}
	}
	return list;
}

static GSList *
get_unmanaged_specs (NMSystemConfigInterface *config)
{
	return get_unhandled_specs (config, NM_IFCFG_CONNECTION_UNMANAGED_SPEC);
}

static GSList *
get_unrecognized_specs (NMSystemConfigInterface *config)
{
	return get_unhandled_specs (config, NM_IFCFG_CONNECTION_UNRECOGNIZED_SPEC);
}

static NMSettingsConnection *
add_connection (NMSystemConfigInterface *config,
                NMConnection *connection,
                gboolean save_to_disk,
                GError **error)
{
	SCPluginIfcfg *self = SC_PLUGIN_IFCFG (config);
	NMIfcfgConnection *added = NULL;
	char *path = NULL;

	/* Ensure we reject attempts to add the connection long before we're
	 * asked to write it to disk.
	 */
	if (!writer_can_write_connection (connection, error))
		return NULL;

	if (save_to_disk) {
		if (!writer_new_connection (connection, IFCFG_DIR, &path, error))
			return NULL;
	}

	added = _internal_new_connection (self, path, connection, error);
	g_free (path);
	return (NMSettingsConnection *) added;
}

#define SC_NETWORK_FILE "/etc/sysconfig/network"
#define HOSTNAME_FILE   "/etc/hostname"

static char *
plugin_get_hostname (SCPluginIfcfg *plugin)
{
	shvarFile *network;
	char *hostname;
	gboolean ignore_localhost;

	if (g_file_get_contents (HOSTNAME_FILE, &hostname, NULL, NULL)) {
		g_strchomp (hostname);
		return hostname;
	}

	network = svOpenFile (SC_NETWORK_FILE, NULL);
	if (!network) {
		nm_log_warn (LOGD_SETTINGS, "Could not get hostname: failed to read " SC_NETWORK_FILE);
		return NULL;
	}

	hostname = svGetValue (network, "HOSTNAME", FALSE);
	ignore_localhost = svTrueValue (network, "NM_IGNORE_HOSTNAME_LOCALHOST", FALSE);
	if (ignore_localhost) {
		/* Ignore a hostname of 'localhost' or 'localhost.localdomain' to preserve
		 * 'network' service behavior.
		 */
		if (hostname && (!strcmp (hostname, "localhost") || !strcmp (hostname, "localhost.localdomain"))) {
			g_free (hostname);
			hostname = NULL;
		}
	}

	svCloseFile (network);
	return hostname;
}

static gboolean
plugin_set_hostname (SCPluginIfcfg *plugin, const char *hostname)
{
	SCPluginIfcfgPrivate *priv = SC_PLUGIN_IFCFG_GET_PRIVATE (plugin);
	shvarFile *network;
	char *hostname_eol;
	gboolean ret;
#if HAVE_SELINUX
	security_context_t se_ctx_prev, se_ctx = NULL;
	struct stat file_stat = { .st_mode = 0 };

	/* Get default context for HOSTNAME_FILE and set it for fscreate */
	stat (HOSTNAME_FILE, &file_stat);
	matchpathcon (HOSTNAME_FILE, file_stat.st_mode, &se_ctx);
	matchpathcon_fini ();
	getfscreatecon (&se_ctx_prev);
	setfscreatecon (se_ctx);
#endif

	hostname_eol = g_strdup_printf ("%s\n", hostname);
	ret = g_file_set_contents (HOSTNAME_FILE, hostname_eol, -1, NULL);

#if HAVE_SELINUX
	/* Restore previous context and cleanup */
	setfscreatecon (se_ctx_prev);
	freecon (se_ctx);
	freecon (se_ctx_prev);
#endif

	if (!ret) {
		nm_log_warn (LOGD_SETTINGS, "Could not save hostname: failed to create/open " HOSTNAME_FILE);
		g_free (hostname_eol);
		return FALSE;
	}

	g_free (priv->hostname);
	priv->hostname = g_strdup (hostname);
	g_free (hostname_eol);

	/* Remove "HOSTNAME" from SC_NETWORK_FILE, if present */
	network = svOpenFile (SC_NETWORK_FILE, NULL);
	if (network) {
		svSetValue (network, "HOSTNAME", NULL, FALSE);
		svWriteFile (network, 0644, NULL);
		svCloseFile (network);
	}

	return TRUE;
}

static void
hostname_maybe_changed (SCPluginIfcfg *plugin)
{
	SCPluginIfcfgPrivate *priv = SC_PLUGIN_IFCFG_GET_PRIVATE (plugin);
	char *new_hostname;

	new_hostname = plugin_get_hostname (plugin);
	if (   (new_hostname && !priv->hostname)
	    || (!new_hostname && priv->hostname)
	    || (priv->hostname && new_hostname && strcmp (priv->hostname, new_hostname))) {
		g_free (priv->hostname);
		priv->hostname = new_hostname;
		g_object_notify (G_OBJECT (plugin), NM_SYSTEM_CONFIG_INTERFACE_HOSTNAME);
	} else
		g_free (new_hostname);
}

static void
sc_network_changed_cb (NMInotifyHelper *ih,
                       struct inotify_event *evt,
                       const char *path,
                       gpointer user_data)
{
	SCPluginIfcfg *plugin = SC_PLUGIN_IFCFG (user_data);
	SCPluginIfcfgPrivate *priv = SC_PLUGIN_IFCFG_GET_PRIVATE (plugin);

	if (evt->wd != priv->sc_network_wd)
		return;

	hostname_maybe_changed (plugin);
}

static void
hostname_changed_cb (GFileMonitor *monitor,
                     GFile *file,
                     GFile *other_file,
                     GFileMonitorEvent event_type,
                     gpointer user_data)
{
	SCPluginIfcfg *plugin = SC_PLUGIN_IFCFG (user_data);

	hostname_maybe_changed (plugin);
}

static gboolean
impl_ifcfgrh_get_ifcfg_details (SCPluginIfcfg *plugin,
                                const char *in_ifcfg,
                                const char **out_uuid,
                                const char **out_path,
                                GError **error)
{
	NMIfcfgConnection *connection;
	NMSettingConnection *s_con;
	const char *uuid;
	const char *path;

	if (!g_path_is_absolute (in_ifcfg)) {
		g_set_error (error,
		             NM_SETTINGS_ERROR,
		             NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "ifcfg path '%s' is not absolute", in_ifcfg);
		return FALSE;
	}

	connection = find_by_path (plugin, in_ifcfg);
	if (   !connection
	    || nm_ifcfg_connection_get_unmanaged_spec (connection)
	    || nm_ifcfg_connection_get_unrecognized_spec (connection)) {
		g_set_error (error,
		             NM_SETTINGS_ERROR,
		             NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "ifcfg file '%s' unknown", in_ifcfg);
		return FALSE;
	}

	s_con = nm_connection_get_setting_connection (NM_CONNECTION (connection));
	if (!s_con) {
		g_set_error (error,
		             NM_SETTINGS_ERROR,
		             NM_SETTINGS_ERROR_INTERNAL_ERROR,
		             "unable to retrieve the connection setting");
		return FALSE;
	}

	uuid = nm_setting_connection_get_uuid (s_con);
	if (!uuid) {
		g_set_error (error,
		             NM_SETTINGS_ERROR,
		             NM_SETTINGS_ERROR_INTERNAL_ERROR,
		             "unable to get the UUID");
		return FALSE;
	}
	
	path = nm_connection_get_path (NM_CONNECTION (connection));
	if (!path) {
		g_set_error (error,
		             NM_SETTINGS_ERROR,
		             NM_SETTINGS_ERROR_INTERNAL_ERROR,
		             "unable to get the connection D-Bus path");
		return FALSE;
	}

	*out_uuid = g_strdup (uuid);
	*out_path = g_strdup (path);

	return TRUE;
}

static void
init (NMSystemConfigInterface *config)
{
}

static void
sc_plugin_ifcfg_init (SCPluginIfcfg *plugin)
{
	SCPluginIfcfgPrivate *priv = SC_PLUGIN_IFCFG_GET_PRIVATE (plugin);
	NMInotifyHelper *ih;
	GError *error = NULL;
	gboolean success = FALSE;
	GFile *file;
	GFileMonitor *monitor;

	priv->connections = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);

	/* We watch SC_NETWORK_FILE via NMInotifyHelper (which doesn't track file creation but
	 * *does* track modifications made via other hard links), since we expect it to always
	 * exist. But we watch HOSTNAME_FILE via GFileMonitor (which has the opposite
	 * semantics), since /etc/hostname might not exist, but is unlikely to have hard
	 * links. bgo 532815 is the bug for being able to just use GFileMonitor for both.
	 */

	ih = nm_inotify_helper_get ();
	priv->ih_event_id = g_signal_connect (ih, "event", G_CALLBACK (sc_network_changed_cb), plugin);
	priv->sc_network_wd = nm_inotify_helper_add_watch (ih, SC_NETWORK_FILE);

	file = g_file_new_for_path (HOSTNAME_FILE);
	monitor = g_file_monitor_file (file, G_FILE_MONITOR_NONE, NULL, NULL);
	g_object_unref (file);
	if (monitor) {
		priv->hostname_monitor_id =
			g_signal_connect (monitor, "changed", G_CALLBACK (hostname_changed_cb), plugin);
		priv->hostname_monitor = monitor;
	}

	priv->hostname = plugin_get_hostname (plugin);

	priv->bus = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);
	if (!priv->bus) {
		nm_log_warn (LOGD_SETTINGS, "Couldn't connect to D-Bus: %s", error->message);
		g_clear_error (&error);
	} else {
		DBusConnection *tmp;
		DBusGProxy *proxy;
		int result;

		tmp = dbus_g_connection_get_connection (priv->bus);
		dbus_connection_set_exit_on_disconnect (tmp, FALSE);

		proxy = dbus_g_proxy_new_for_name (priv->bus,
		                                   "org.freedesktop.DBus",
		                                   "/org/freedesktop/DBus",
		                                   "org.freedesktop.DBus");

		if (!dbus_g_proxy_call (proxy, "RequestName", &error,
		                        G_TYPE_STRING, DBUS_SERVICE_NAME,
		                        G_TYPE_UINT, DBUS_NAME_FLAG_DO_NOT_QUEUE,
		                        G_TYPE_INVALID,
		                        G_TYPE_UINT, &result,
		                        G_TYPE_INVALID)) {
			nm_log_warn (LOGD_SETTINGS, "Couldn't acquire D-Bus service: %s", error->message);
			g_clear_error (&error);
		} else if (result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
			nm_log_warn (LOGD_SETTINGS, "Couldn't acquire ifcfgrh1 D-Bus service (already taken)");
		} else
			success = TRUE;
	}

	if (!success) {
		if (priv->bus) {
			dbus_g_connection_unref (priv->bus);
			priv->bus = NULL;
		}
	}
}

static void
dispose (GObject *object)
{
	SCPluginIfcfg *plugin = SC_PLUGIN_IFCFG (object);
	SCPluginIfcfgPrivate *priv = SC_PLUGIN_IFCFG_GET_PRIVATE (plugin);
	NMInotifyHelper *ih;

	if (priv->bus) {
		dbus_g_connection_unref (priv->bus);
		priv->bus = NULL;
	}

	if (priv->ih_event_id) {
		ih = nm_inotify_helper_get ();

		g_signal_handler_disconnect (ih, priv->ih_event_id);
		priv->ih_event_id = 0;

		if (priv->sc_network_wd >= 0)
			nm_inotify_helper_remove_watch (ih, priv->sc_network_wd);
	}

	if (priv->hostname_monitor) {
		if (priv->hostname_monitor_id)
			g_signal_handler_disconnect (priv->hostname_monitor, priv->hostname_monitor_id);

		g_file_monitor_cancel (priv->hostname_monitor);
		g_object_unref (priv->hostname_monitor);
	}

	g_free (priv->hostname);

	if (priv->connections) {
		g_hash_table_destroy (priv->connections);
		priv->connections = NULL;
	}

	if (priv->ifcfg_monitor) {
		if (priv->ifcfg_monitor_id)
			g_signal_handler_disconnect (priv->ifcfg_monitor, priv->ifcfg_monitor_id);

		g_file_monitor_cancel (priv->ifcfg_monitor);
		g_object_unref (priv->ifcfg_monitor);
	}

	G_OBJECT_CLASS (sc_plugin_ifcfg_parent_class)->dispose (object);
}

static void
get_property (GObject *object, guint prop_id,
			  GValue *value, GParamSpec *pspec)
{
	SCPluginIfcfgPrivate *priv = SC_PLUGIN_IFCFG_GET_PRIVATE (object);

	switch (prop_id) {
	case NM_SYSTEM_CONFIG_INTERFACE_PROP_NAME:
		g_value_set_string (value, IFCFG_PLUGIN_NAME);
		break;
	case NM_SYSTEM_CONFIG_INTERFACE_PROP_INFO:
		g_value_set_string (value, IFCFG_PLUGIN_INFO);
		break;
	case NM_SYSTEM_CONFIG_INTERFACE_PROP_CAPABILITIES:
		g_value_set_uint (value, NM_SYSTEM_CONFIG_INTERFACE_CAP_MODIFY_CONNECTIONS | NM_SYSTEM_CONFIG_INTERFACE_CAP_MODIFY_HOSTNAME);
		break;
	case NM_SYSTEM_CONFIG_INTERFACE_PROP_HOSTNAME:
		g_value_set_string (value, priv->hostname);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
set_property (GObject *object, guint prop_id,
			  const GValue *value, GParamSpec *pspec)
{
	const char *hostname;

	switch (prop_id) {
	case NM_SYSTEM_CONFIG_INTERFACE_PROP_HOSTNAME:
		hostname = g_value_get_string (value);
		if (hostname && strlen (hostname) < 1)
			hostname = NULL;
		plugin_set_hostname (SC_PLUGIN_IFCFG (object), hostname);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
sc_plugin_ifcfg_class_init (SCPluginIfcfgClass *req_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (req_class);

	g_type_class_add_private (req_class, sizeof (SCPluginIfcfgPrivate));

	object_class->dispose = dispose;
	object_class->get_property = get_property;
	object_class->set_property = set_property;

	g_object_class_override_property (object_class,
	                                  NM_SYSTEM_CONFIG_INTERFACE_PROP_NAME,
	                                  NM_SYSTEM_CONFIG_INTERFACE_NAME);

	g_object_class_override_property (object_class,
	                                  NM_SYSTEM_CONFIG_INTERFACE_PROP_INFO,
	                                  NM_SYSTEM_CONFIG_INTERFACE_INFO);

	g_object_class_override_property (object_class,
	                                  NM_SYSTEM_CONFIG_INTERFACE_PROP_CAPABILITIES,
	                                  NM_SYSTEM_CONFIG_INTERFACE_CAPABILITIES);

	g_object_class_override_property (object_class,
	                                  NM_SYSTEM_CONFIG_INTERFACE_PROP_HOSTNAME,
	                                  NM_SYSTEM_CONFIG_INTERFACE_HOSTNAME);

	dbus_g_object_type_install_info (G_TYPE_FROM_CLASS (req_class),
									 &dbus_glib_nm_ifcfg_rh_object_info);
}

static void
system_config_interface_init (NMSystemConfigInterface *system_config_interface_class)
{
	/* interface implementation */
	system_config_interface_class->get_connections = get_connections;
	system_config_interface_class->add_connection = add_connection;
	system_config_interface_class->load_connection = load_connection;
	system_config_interface_class->reload_connections = reload_connections;
	system_config_interface_class->get_unmanaged_specs = get_unmanaged_specs;
	system_config_interface_class->get_unrecognized_specs = get_unrecognized_specs;
	system_config_interface_class->init = init;
}

G_MODULE_EXPORT GObject *
nm_system_config_factory (void)
{
	static SCPluginIfcfg *singleton = NULL;
	SCPluginIfcfgPrivate *priv;

	if (!singleton) {
		singleton = SC_PLUGIN_IFCFG (g_object_new (SC_TYPE_PLUGIN_IFCFG, NULL));
		priv = SC_PLUGIN_IFCFG_GET_PRIVATE (singleton);
		if (priv->bus)
			dbus_g_connection_register_g_object (priv->bus,
			                                     DBUS_OBJECT_PATH,
			                                     G_OBJECT (singleton));
		nm_log_info (LOGD_SETTINGS, "Acquired D-Bus service %s", DBUS_SERVICE_NAME);
	} else
		g_object_ref (singleton);

	return G_OBJECT (singleton);
}
