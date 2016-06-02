/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/*
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 * Copyright 2008 Novell, Inc.
 * Copyright 2008 - 2010 Red Hat, Inc.
 * Copyright 2015 Red Hat, Inc.
 */

#include "nm-default.h"

#include "nm-vpn-editor-plugin.h"

#include "nm-core-internal.h"

static void nm_vpn_editor_plugin_default_init (NMVpnEditorPluginInterface *iface);

G_DEFINE_INTERFACE (NMVpnEditorPlugin, nm_vpn_editor_plugin, G_TYPE_OBJECT)

static void
nm_vpn_editor_plugin_default_init (NMVpnEditorPluginInterface *iface)
{
	/* Properties */

	/**
	 * NMVpnEditorPlugin:name:
	 *
	 * Short display name of the VPN plugin.
	 */
	g_object_interface_install_property (iface,
		 g_param_spec_string (NM_VPN_EDITOR_PLUGIN_NAME, "", "",
		                      NULL,
		                      G_PARAM_READABLE |
		                      G_PARAM_STATIC_STRINGS));

	/**
	 * NMVpnEditorPlugin:description:
	 *
	 * Longer description of the VPN plugin.
	 */
	g_object_interface_install_property (iface,
		 g_param_spec_string (NM_VPN_EDITOR_PLUGIN_DESCRIPTION, "", "",
		                      NULL,
		                      G_PARAM_READABLE |
		                      G_PARAM_STATIC_STRINGS));

	/**
	 * NMVpnEditorPlugin:service:
	 *
	 * D-Bus service name of the plugin's VPN service.
	 */
	g_object_interface_install_property (iface,
		 g_param_spec_string (NM_VPN_EDITOR_PLUGIN_SERVICE, "", "",
		                      NULL,
		                      G_PARAM_READABLE |
		                      G_PARAM_STATIC_STRINGS));
}

/*********************************************************************/

static NMVpnEditorPlugin *
_nm_vpn_editor_plugin_load (const char *plugin_name,
                            gboolean do_file_checks,
                            const char *check_service,
                            int check_owner,
                            NMUtilsCheckFilePredicate check_file,
                            gpointer user_data,
                            GError **error)
{
	GModule *module = NULL;
	NMVpnEditorPluginFactory factory = NULL;
	NMVpnEditorPlugin *editor_plugin = NULL;
	gs_free char *plugin_filename_free = NULL;
	const char *plugin_filename;

	g_return_val_if_fail (plugin_name && *plugin_name, NULL);

	/* if @do_file_checks is FALSE, we pass plugin_name directly to
	 * g_module_open().
	 *
	 * Otherwise, we allow for library names without path component.
	 * In which case, we prepend the plugin directory and form an
	 * absolute path. In that case, we perform checks on the file.
	 *
	 * One exception is that we don't allow for the "la" suffix. The
	 * reason is that g_module_open() interprets files with this extension
	 * special and we don't want that. */
	plugin_filename = plugin_name;
	if (do_file_checks) {
		if (   !strchr (plugin_name, '/')
		    && !g_str_has_suffix (plugin_name, ".la")) {
			plugin_filename_free = g_module_build_path (NMPLUGINDIR, plugin_name);
			plugin_filename = plugin_filename_free;
		}

		/* _nm_utils_check_module_file() fails with ENOENT if the plugin file
		 * does not exist. That is relevant, because nm-applet checks for that. */
		if (!_nm_utils_check_module_file (plugin_filename,
		                                  check_owner,
		                                  check_file,
		                                  user_data,
		                                  error))
			return NULL;
	}

	module = g_module_open (plugin_filename, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
	if (!module) {
		g_set_error (error,
		             NM_VPN_PLUGIN_ERROR,
		             NM_VPN_PLUGIN_ERROR_FAILED,
		             _("cannot load plugin %s"), plugin_name);
		return NULL;
	}

	if (g_module_symbol (module, "nm_vpn_editor_plugin_factory", (gpointer) &factory)) {
		gs_free_error GError *factory_error = NULL;
		gboolean success = FALSE;

		editor_plugin = factory (&factory_error);

		g_assert (!editor_plugin || G_IS_OBJECT (editor_plugin));

		if (editor_plugin) {
			gs_free char *plug_name = NULL, *plug_service = NULL;

			/* Validate plugin properties */

			g_object_get (G_OBJECT (editor_plugin),
			              NM_VPN_EDITOR_PLUGIN_NAME, &plug_name,
			              NM_VPN_EDITOR_PLUGIN_SERVICE, &plug_service,
			              NULL);

			if (!plug_name || !*plug_name) {
				g_set_error (error,
				             NM_VPN_PLUGIN_ERROR,
				             NM_VPN_PLUGIN_ERROR_FAILED,
				             _("cannot load VPN plugin in '%s': missing plugin name"),
				             g_module_name (module));
			} else if (   check_service
			           && g_strcmp0 (plug_service, check_service) != 0) {
				g_set_error (error,
				             NM_VPN_PLUGIN_ERROR,
				             NM_VPN_PLUGIN_ERROR_FAILED,
				             _("cannot load VPN plugin in '%s': invalid service name"),
				             g_module_name (module));
			} else {
				/* Success! */
				g_object_set_data_full (G_OBJECT (editor_plugin), "gmodule", module,
				                        (GDestroyNotify) g_module_close);
				success = TRUE;
			}
		} else {
			if (factory_error) {
				g_propagate_error (error, factory_error);
				factory_error = NULL;
			} else {
				g_set_error (error,
				             NM_VPN_PLUGIN_ERROR,
				             NM_VPN_PLUGIN_ERROR_FAILED,
				             _("unknown error initializing plugin %s"), plugin_name);
			}
		}

		if (!success) {
			g_module_close (module);
			editor_plugin = NULL;
		}
	} else {
		g_set_error (error,
		             NM_VPN_PLUGIN_ERROR,
		             NM_VPN_PLUGIN_ERROR_FAILED,
		             _("failed to load nm_vpn_editor_plugin_factory() from %s (%s)"),
		             g_module_name (module), g_module_error ());
		g_module_close (module);
		editor_plugin = NULL;
	}

	return editor_plugin;
}

/**
 * nm_vpn_editor_plugin_load_from_file:
 * @plugin_name: The path or name of the shared library to load.
 *  The path must either be an absolute filename to an existing file.
 *  Alternatively, it can be the name (without path) of a library in the
 *  plugin directory of NetworkManager.
 * @check_service: if not-null, check that the loaded plugin advertises
 *  the given service.
 * @check_owner: if non-negative, check whether the file is owned
 *  by UID @check_owner or by root. In this case also check that
 *  the file is not writable by anybody else.
 * @check_file: (scope call): optional callback to validate the file prior to
 *   loading the shared library.
 * @user_data: user data for @check_file
 * @error: on failure the error reason.
 *
 * Load the shared libary @plugin_name and create a new
 * #NMVpnEditorPlugin instace via the #NMVpnEditorPluginFactory
 * function.
 *
 * If @plugin_name is not an absolute path name, it assumes the file
 * is in the plugin directory of NetworkManager. In any case, the call
 * will do certain checks on the file before passing it to dlopen.
 * A consequence for that is, that you cannot omit the ".so" suffix.
 *
 * Returns: (transfer full): a new plugin instance or %NULL on error.
 *
 * Since: 1.2
 */
NMVpnEditorPlugin *
nm_vpn_editor_plugin_load_from_file  (const char *plugin_name,
                                      const char *check_service,
                                      int check_owner,
                                      NMUtilsCheckFilePredicate check_file,
                                      gpointer user_data,
                                      GError **error)
{
	return _nm_vpn_editor_plugin_load (plugin_name,
	                                   TRUE,
	                                   check_service,
	                                   check_owner,
	                                   check_file,
	                                   user_data,
	                                   error);
}

/*********************************************************************/

/**
 * nm_vpn_editor_plugin_get_editor:
 * @plugin: the #NMVpnEditorPlugin
 * @connection: the #NMConnection to be edited
 * @error: on return, an error or %NULL
 *
 * Returns: (transfer full): a new #NMVpnEditor or %NULL on error
 */
NMVpnEditor *
nm_vpn_editor_plugin_get_editor (NMVpnEditorPlugin *plugin,
                                 NMConnection *connection,
                                 GError **error)
{
	g_return_val_if_fail (NM_IS_VPN_EDITOR_PLUGIN (plugin), NULL);

	return NM_VPN_EDITOR_PLUGIN_GET_INTERFACE (plugin)->get_editor (plugin, connection, error);
}

NMVpnEditorPluginCapability
nm_vpn_editor_plugin_get_capabilities (NMVpnEditorPlugin *plugin)
{
	g_return_val_if_fail (NM_IS_VPN_EDITOR_PLUGIN (plugin), 0);

	return NM_VPN_EDITOR_PLUGIN_GET_INTERFACE (plugin)->get_capabilities (plugin);
}

/**
 * nm_vpn_editor_plugin_import:
 * @plugin: the #NMVpnEditorPlugin
 * @path: full path to the file to attempt to read into a new #NMConnection
 * @error: on return, an error or %NULL
 *
 * Returns: (transfer full): a new #NMConnection imported from @path, or %NULL
 * on error or if the file at @path was not recognized by this plugin
 */
NMConnection *
nm_vpn_editor_plugin_import (NMVpnEditorPlugin *plugin,
                             const char *path,
                             GError **error)
{
	g_return_val_if_fail (NM_IS_VPN_EDITOR_PLUGIN (plugin), NULL);

	if (nm_vpn_editor_plugin_get_capabilities (plugin) & NM_VPN_EDITOR_PLUGIN_CAPABILITY_IMPORT) {
		g_return_val_if_fail (NM_VPN_EDITOR_PLUGIN_GET_INTERFACE (plugin)->import_from_file != NULL, NULL);
		return NM_VPN_EDITOR_PLUGIN_GET_INTERFACE (plugin)->import_from_file (plugin, path, error);
	}

	g_set_error (error,
	             NM_VPN_PLUGIN_ERROR,
	             NM_VPN_PLUGIN_ERROR_FAILED,
	             _("the plugin does not support import capability"));
	return NULL;
}

gboolean
nm_vpn_editor_plugin_export (NMVpnEditorPlugin *plugin,
                             const char *path,
                             NMConnection *connection,
                             GError **error)
{
	g_return_val_if_fail (NM_IS_VPN_EDITOR_PLUGIN (plugin), FALSE);

	if (nm_vpn_editor_plugin_get_capabilities (plugin) & NM_VPN_EDITOR_PLUGIN_CAPABILITY_EXPORT) {
		g_return_val_if_fail (NM_VPN_EDITOR_PLUGIN_GET_INTERFACE (plugin)->export_to_file != NULL, FALSE);
		return NM_VPN_EDITOR_PLUGIN_GET_INTERFACE (plugin)->export_to_file (plugin, path, connection, error);
	}

	g_set_error (error,
	             NM_VPN_PLUGIN_ERROR,
	             NM_VPN_PLUGIN_ERROR_FAILED,
	             _("the plugin does not support export capability"));
	return FALSE;
}

char *
nm_vpn_editor_plugin_get_suggested_filename (NMVpnEditorPlugin *plugin,
                                             NMConnection *connection)
{
	g_return_val_if_fail (NM_IS_VPN_EDITOR_PLUGIN (plugin), NULL);

	if (NM_VPN_EDITOR_PLUGIN_GET_INTERFACE (plugin)->get_suggested_filename)
		return NM_VPN_EDITOR_PLUGIN_GET_INTERFACE (plugin)->get_suggested_filename (plugin, connection);
	return NULL;
}

