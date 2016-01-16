/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright 2012, 2013 Red Hat, Inc.
 */

/**
 * SECTION:nm-editor-utils
 * @short_description: Miscellaneous connection editor utilities
 *
 * nm-editor-utils contains helper functions for connection editors.
 * The goal is that this should eventually be shared between nmtui,
 * nm-connection-editor, and gnome-control-center.
 */

#include "config.h"

#include <glib/gi18n.h>
#include <nm-utils.h>

#include <nm-device-bond.h>
#include <nm-device-bridge.h>
#include <nm-device-team.h>
#include <nm-device-ethernet.h>
#include <nm-device-infiniband.h>
#include <nm-device-team.h>
#include <nm-device-vlan.h>
#include <nm-device-wifi.h>

#include "nm-editor-utils.h"
#if 0
#include "vpn-helpers.h"

static GSList *vpn_plugins;

static gint
sort_vpn_plugins (gconstpointer a, gconstpointer b)
{
	NMVpnPluginUiInterface *aa = NM_VPN_PLUGIN_UI_INTERFACE (a);
	NMVpnPluginUiInterface *bb = NM_VPN_PLUGIN_UI_INTERFACE (b);
	char *aa_desc = NULL, *bb_desc = NULL;
	int ret;

	g_object_get (aa, NM_VPN_PLUGIN_UI_INTERFACE_NAME, &aa_desc, NULL);
	g_object_get (bb, NM_VPN_PLUGIN_UI_INTERFACE_NAME, &bb_desc, NULL);

	ret = g_strcmp0 (aa_desc, bb_desc);

	g_free (aa_desc);
	g_free (bb_desc);

	return ret;
}
#endif

static void
wifi_connection_setup_func (NMConnection        *connection,
                            NMSettingConnection *s_con,
                            NMSetting           *s_hw)
{
	g_object_set (G_OBJECT (s_hw),
	              NM_SETTING_WIRELESS_MODE, NM_SETTING_WIRELESS_MODE_INFRA,
	              NULL);
}

static void
bond_connection_setup_func (NMConnection        *connection,
                            NMSettingConnection *s_con,
                            NMSetting           *s_hw)
{
	NMSettingBond *s_bond = NM_SETTING_BOND (s_hw);
	const char **options, *def, *cur;
	int i;

	options = nm_setting_bond_get_valid_options (s_bond);
	for (i = 0; options[i]; i++) {
		def = nm_setting_bond_get_option_default (s_bond, options[i]);
		cur = nm_setting_bond_get_option_by_name (s_bond, options[i]);
		if (g_strcmp0 (def, cur) != 0)
			nm_setting_bond_add_option (s_bond, options[i], def);
	}
}

typedef void (*NMEditorNewConnectionSetupFunc) (NMConnection        *connection,
                                                NMSettingConnection *s_con,
                                                NMSetting           *s_hw);

typedef struct {
	NMEditorConnectionTypeData data;

	const char *id_format;
	NMEditorNewConnectionSetupFunc connection_setup_func;
	gboolean no_autoconnect;
} NMEditorConnectionTypeDataReal;

static gint
sort_types (gconstpointer a, gconstpointer b)
{
	NMEditorConnectionTypeData *typea = *(NMEditorConnectionTypeData **)a;
	NMEditorConnectionTypeData *typeb = *(NMEditorConnectionTypeData **)b;

	if (typea->virtual && !typeb->virtual)
		return 1;
	else if (typeb->virtual && !typea->virtual)
		return -1;

	if (typea->setting_type == NM_TYPE_SETTING_VPN &&
	    typeb->setting_type != NM_TYPE_SETTING_VPN)
		return 1;
	else if (typeb->setting_type == NM_TYPE_SETTING_VPN &&
	         typea->setting_type != NM_TYPE_SETTING_VPN)
		return -1;

	return g_utf8_collate (typea->name, typeb->name);
}

/**
 * nm_editor_utils_get_connection_type_list:
 *
 * Gets an array of information about supported connection types. The
 * array is sorted in a standard presentation order (hardware types
 * first, alphabetized, then virtual types, alphabetized, then VPN
 * types, alphabetized).
 *
 * Returns: the array of connection type information
 */
NMEditorConnectionTypeData **
nm_editor_utils_get_connection_type_list (void)
{
	GPtrArray *array;
	NMEditorConnectionTypeDataReal *item;
	static NMEditorConnectionTypeData **list;
#if 0
	GHashTable *vpn_plugins_hash;
	gboolean have_vpn_plugins;
#endif

	if (list)
		return list;

	array = g_ptr_array_new ();

	item = g_new0 (NMEditorConnectionTypeDataReal, 1);
	item->data.name = _("Ethernet");
	item->data.setting_type = NM_TYPE_SETTING_WIRED;
	item->data.device_type = NM_TYPE_DEVICE_ETHERNET;
	item->data.virtual = FALSE;
	item->id_format = _("Ethernet connection %d");
	g_ptr_array_add (array, item);

	item = g_new0 (NMEditorConnectionTypeDataReal, 1);
	item->data.name = _("Wi-Fi");
	item->data.setting_type = NM_TYPE_SETTING_WIRELESS;
	item->data.device_type = NM_TYPE_DEVICE_WIFI;
	item->data.virtual = FALSE;
	item->id_format = _("Wi-Fi connection %d");
	item->connection_setup_func = wifi_connection_setup_func;
	g_ptr_array_add (array, item);

	item = g_new0 (NMEditorConnectionTypeDataReal, 1);
	item->data.name = _("InfiniBand");
	item->data.setting_type = NM_TYPE_SETTING_INFINIBAND;
	item->data.device_type = NM_TYPE_DEVICE_INFINIBAND;
	item->data.virtual = FALSE;
	item->id_format = _("InfiniBand connection %d");
	g_ptr_array_add (array, item);

#if 0
	item = g_new0 (NMEditorConnectionTypeDataReal, 1);
	item->data.name = _("Mobile Broadband");
	item->data.setting_type = NM_TYPE_SETTING_GSM;
	item->data.virtual = FALSE;
	item->id_format = _("Mobile broadband connection %d");
	item->no_autoconnect = TRUE;
	g_ptr_array_add (array, item);

	item = g_new0 (NMEditorConnectionTypeDataReal, 1);
	item->data.name = _("DSL");
	item->data.setting_type = NM_TYPE_SETTING_PPPOE;
	item->data.device_type = NM_TYPE_DEVICE_ETHERNET;
	item->data.virtual = FALSE;
	item->id_format = _("DSL connection %d");
	item->no_autoconnect = TRUE;
	g_ptr_array_add (array, item);
#endif

	item = g_new0 (NMEditorConnectionTypeDataReal, 1);
	item->data.name = _("Bond");
	item->data.setting_type = NM_TYPE_SETTING_BOND;
	item->data.device_type = NM_TYPE_DEVICE_BOND;
	item->data.virtual = TRUE;
	item->id_format = _("Bond connection %d");
	item->connection_setup_func = bond_connection_setup_func;
	g_ptr_array_add (array, item);

	item = g_new0 (NMEditorConnectionTypeDataReal, 1);
	item->data.name = _("Bridge");
	item->data.setting_type = NM_TYPE_SETTING_BRIDGE;
	item->data.slave_setting_type = NM_TYPE_SETTING_BRIDGE_PORT;
	item->data.device_type = NM_TYPE_DEVICE_BRIDGE;
	item->data.virtual = TRUE;
	item->id_format = _("Bridge connection %d");
	g_ptr_array_add (array, item);

	item = g_new0 (NMEditorConnectionTypeDataReal, 1);
	item->data.name = _("Team");
	item->data.setting_type = NM_TYPE_SETTING_TEAM;
	item->data.slave_setting_type = NM_TYPE_SETTING_TEAM_PORT;
	item->data.device_type = NM_TYPE_DEVICE_TEAM;
	item->data.virtual = TRUE;
	item->id_format = _("Team connection %d");
	g_ptr_array_add (array, item);

	item = g_new0 (NMEditorConnectionTypeDataReal, 1);
	item->data.name = _("VLAN");
	item->data.setting_type = NM_TYPE_SETTING_VLAN;
	item->data.device_type = NM_TYPE_DEVICE_VLAN;
	item->data.virtual = TRUE;
	item->id_format = _("VLAN connection %d");
	g_ptr_array_add (array, item);

#if 0
	/* Add "VPN" only if there are plugins */
	vpn_plugins_hash = vpn_get_plugins (NULL);
	have_vpn_plugins  = vpn_plugins_hash && g_hash_table_size (vpn_plugins_hash);
	if (have_vpn_plugins) {
		GHashTableIter iter;
		gpointer name, plugin;

		item = g_new0 (NMEditorConnectionTypeDataReal, 1);
		item->data.name = _("VPN");
		item->data.setting_type = NM_TYPE_SETTING_VPN;
		item->data.virtual = TRUE;
		item->id_format = _("VPN connection %d");
		item->no_autoconnect = TRUE;
		g_ptr_array_add (array, item);

		vpn_plugins = NULL;
		g_hash_table_iter_init (&iter, vpn_plugins_hash);
		while (g_hash_table_iter_next (&iter, &name, &plugin))
			vpn_plugins = g_slist_prepend (vpn_plugins, plugin);
		vpn_plugins = g_slist_sort (vpn_plugins, sort_vpn_plugins);
	}
#endif

	g_ptr_array_sort (array, sort_types);
	g_ptr_array_add (array, NULL);

	list = (NMEditorConnectionTypeData **)g_ptr_array_free (array, FALSE);
	return list;
}

static char *
get_available_connection_name (const char       *format,
                               NMRemoteSettings *settings)
{
	GSList *connections, *iter, *names = NULL;
	char *cname = NULL;
	int i = 0;

	connections = nm_remote_settings_list_connections (settings);
	for (iter = connections; iter; iter = iter->next) {
		const char *id;

		id = nm_connection_get_id (NM_CONNECTION (iter->data));
		g_assert (id);
		names = g_slist_append (names, (gpointer) id);
	}
	g_slist_free (connections);

	/* Find the next available unique connection name */
	while (!cname && (i++ < 10000)) {
		char *temp;
		gboolean found = FALSE;

		temp = g_strdup_printf (format, i);
		for (iter = names; iter; iter = g_slist_next (iter)) {
			if (!strcmp (iter->data, temp)) {
				found = TRUE;
				break;
			}
		}
		if (!found)
			cname = temp;
		else
			g_free (temp);
	}

	g_slist_free (names);
	return cname;
}

/**
 * nm_editor_utils_create_connection:
 * @type: the type of the connection's primary #NMSetting
 * @master: (allow-none): the connection's master, if any
 * @settings: an #NMRemoteSettings
 *
 * Creates a new #NMConnection of the given type, automatically
 * creating a UUID and an appropriate not-currently-in-use connection
 * name, setting #NMSettingConnection:autoconnect appropriately for
 * the connection type, filling in slave-related information if
 * @master is not %NULL, and initializing any other mandatory-to-set
 * properties to reasonable initial values.
 *
 * Returns: a new #NMConnection
 */
NMConnection *
nm_editor_utils_create_connection (GType             type,
                                   NMConnection     *master,
                                   NMRemoteSettings *settings)
{
	NMEditorConnectionTypeData **types;
	NMEditorConnectionTypeDataReal *type_data = NULL;
	const char *master_setting_type = NULL, *master_uuid = NULL;
	GType master_type = G_TYPE_INVALID, slave_setting_type = G_TYPE_INVALID;
	NMConnection *connection;
	NMSettingConnection *s_con;
	NMSetting *s_hw, *s_slave;
	char *uuid, *id;
	int i;

	if (master) {
		NMSettingConnection *master_s_con;

		master_s_con = nm_connection_get_setting_connection (master);
		master_setting_type = nm_setting_connection_get_connection_type (master_s_con);
		master_uuid = nm_setting_connection_get_uuid (master_s_con);
		master_type = nm_connection_lookup_setting_type (master_setting_type);
	}

	types = nm_editor_utils_get_connection_type_list ();
	for (i = 0; types[i]; i++) {
		if (types[i]->setting_type == type)
			type_data = (NMEditorConnectionTypeDataReal *)types[i];
		if (types[i]->setting_type == master_type)
			slave_setting_type = types[i]->slave_setting_type;

	}
	if (!type_data) {
		g_return_val_if_reached (NULL);
		return NULL;
	}

	connection = nm_connection_new ();

	s_con = NM_SETTING_CONNECTION (nm_setting_connection_new ());
	nm_connection_add_setting (connection, NM_SETTING (s_con));

	s_hw = g_object_new (type, NULL);
	nm_connection_add_setting (connection, s_hw);

	if (slave_setting_type != G_TYPE_INVALID) {
		s_slave = g_object_new (slave_setting_type, NULL);
		nm_connection_add_setting (connection, s_slave);
	}

	uuid = nm_utils_uuid_generate ();
	id = get_available_connection_name (type_data->id_format, settings);

	g_object_set (s_con,
	              NM_SETTING_CONNECTION_UUID, uuid,
	              NM_SETTING_CONNECTION_ID, id,
	              NM_SETTING_CONNECTION_TYPE, nm_setting_get_name (s_hw),
	              NM_SETTING_CONNECTION_AUTOCONNECT, !type_data->no_autoconnect,
	              NM_SETTING_CONNECTION_MASTER, master_uuid,
	              NM_SETTING_CONNECTION_SLAVE_TYPE, master_setting_type,
	              NULL);

	g_free (uuid);
	g_free (id);

	if (type_data->connection_setup_func)
		type_data->connection_setup_func (connection, s_con, s_hw);

	return connection;
}

/**
 * nm_editor_utils_get_connection_type_data:
 * @conn: an #NMConnection
 *
 * Gets the #NMEditorConnectionTypeData corresponding to
 * @conn's connection type.
 *
 * Returns: the #NMEditorConnectionTypeData
 */
NMEditorConnectionTypeData *
nm_editor_utils_get_connection_type_data (NMConnection *conn)
{
	NMSettingConnection *s_con;
	const char *conn_type;
	GType conn_gtype;
	NMEditorConnectionTypeData **types;
	int i;

	s_con = nm_connection_get_setting_connection (conn);
	g_return_val_if_fail (s_con != NULL, NULL);

	conn_type = nm_setting_connection_get_connection_type (s_con);
	conn_gtype = nm_connection_lookup_setting_type (conn_type);
	g_return_val_if_fail (conn_gtype != G_TYPE_INVALID, NULL);

	types = nm_editor_utils_get_connection_type_list ();
	for (i = 0; types[i]; i++) {
		if (types[i]->setting_type == conn_gtype)
			return types[i];
	}

	return NULL;
}
