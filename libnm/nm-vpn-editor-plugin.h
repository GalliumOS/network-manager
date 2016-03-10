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
 * Copyright 2008 - 2014 Red Hat, Inc.
 * Copyright 2008 Novell, Inc.
 */

#ifndef __NM_VPN_EDITOR_PLUGIN_H__
#define __NM_VPN_EDITOR_PLUGIN_H__

#if !defined (__NETWORKMANAGER_H_INSIDE__) && !defined (NETWORKMANAGER_COMPILATION)
#error "Only <NetworkManager.h> can be included directly."
#endif

#include <glib.h>
#include <glib-object.h>
#include <nm-types.h>

G_BEGIN_DECLS

typedef struct _NMVpnEditorPlugin NMVpnEditorPlugin;
typedef struct _NMVpnEditor NMVpnEditor;

/* Plugin's factory function that returns a GObject that implements
 * NMVpnEditorPlugin.
 */
#ifndef __GI_SCANNER__
typedef NMVpnEditorPlugin * (*NMVpnEditorPluginFactory) (GError **error);
NMVpnEditorPlugin *nm_vpn_editor_plugin_factory (GError **error);
#endif


/**************************************************/
/* Editor plugin interface                        */
/**************************************************/

#define NM_TYPE_VPN_EDITOR_PLUGIN               (nm_vpn_editor_plugin_get_type ())
#define NM_VPN_EDITOR_PLUGIN(obj)               (G_TYPE_CHECK_INSTANCE_CAST ((obj), NM_TYPE_VPN_EDITOR_PLUGIN, NMVpnEditorPlugin))
#define NM_IS_VPN_EDITOR_PLUGIN(obj)            (G_TYPE_CHECK_INSTANCE_TYPE ((obj), NM_TYPE_VPN_EDITOR_PLUGIN))
#define NM_VPN_EDITOR_PLUGIN_GET_INTERFACE(obj) (G_TYPE_INSTANCE_GET_INTERFACE ((obj), NM_TYPE_VPN_EDITOR_PLUGIN, NMVpnEditorPluginInterface))

/**
 * NMVpnEditorPluginCapability:
 * @NM_VPN_EDITOR_PLUGIN_CAPABILITY_NONE: unknown or no capability
 * @NM_VPN_EDITOR_PLUGIN_CAPABILITY_IMPORT: the plugin can import new connections
 * @NM_VPN_EDITOR_PLUGIN_CAPABILITY_EXPORT: the plugin can export connections
 * @NM_VPN_EDITOR_PLUGIN_CAPABILITY_IPV6: the plugin supports IPv6 addressing
 *
 * Flags that indicate certain capabilities of the plugin to editor programs.
 **/
typedef enum /*< flags >*/ {
	NM_VPN_EDITOR_PLUGIN_CAPABILITY_NONE   = 0x00,
	NM_VPN_EDITOR_PLUGIN_CAPABILITY_IMPORT = 0x01,
	NM_VPN_EDITOR_PLUGIN_CAPABILITY_EXPORT = 0x02,
	NM_VPN_EDITOR_PLUGIN_CAPABILITY_IPV6   = 0x04
} NMVpnEditorPluginCapability;

/* Short display name of the VPN plugin */
#define NM_VPN_EDITOR_PLUGIN_NAME "name"

/* Longer description of the VPN plugin */
#define NM_VPN_EDITOR_PLUGIN_DESCRIPTION "description"

/* D-Bus service name of the plugin's VPN service */
#define NM_VPN_EDITOR_PLUGIN_SERVICE "service"

/**
 * NMVpnEditorPluginInterface:
 * @g_iface: the parent interface
 * @get_editor: returns an #NMVpnEditor, pre-filled with values from @connection
 *   if non-%NULL.
 * @get_capabilities: returns a bitmask of capabilities.
 * @import_from_file: Try to import a connection from the specified path.  On
 *   success, return a partial #NMConnection object.  On error, return %NULL and
 *   set @error with additional information.  Note that @error can be %NULL, in
 *   which case no additional error information should be provided.
 * @export_to_file: Export the given connection to the specified path.  Return
 *   %TRUE on success.  On error, return %FALSE and set @error with additional
 *   error information.  Note that @error can be %NULL, in which case no
 *   additional error information should be provided.
 * @get_suggested_filename: For a given connection, return a suggested file
 *   name.  Returned value will be %NULL or a suggested file name to be freed by
 *   the caller.
 *
 * Interface for VPN editor plugins.
 */
typedef struct {
	GTypeInterface g_iface;

	NMVpnEditor * (*get_editor) (NMVpnEditorPlugin *plugin,
	                             NMConnection *connection,
	                             GError **error);

	NMVpnEditorPluginCapability (*get_capabilities) (NMVpnEditorPlugin *plugin);

	NMConnection * (*import_from_file) (NMVpnEditorPlugin *plugin,
	                                    const char *path,
	                                    GError **error);

	gboolean (*export_to_file) (NMVpnEditorPlugin *plugin,
	                            const char *path,
	                            NMConnection *connection,
	                            GError **error);

	char * (*get_suggested_filename) (NMVpnEditorPlugin *plugin, NMConnection *connection);
} NMVpnEditorPluginInterface;

GType nm_vpn_editor_plugin_get_type (void);

NMVpnEditor *nm_vpn_editor_plugin_get_editor (NMVpnEditorPlugin *plugin,
                                              NMConnection *connection,
                                              GError **error);

NMVpnEditorPluginCapability nm_vpn_editor_plugin_get_capabilities (NMVpnEditorPlugin *plugin);

NMConnection *nm_vpn_editor_plugin_import                 (NMVpnEditorPlugin *plugin,
                                                           const char *path,
                                                           GError **error);
gboolean      nm_vpn_editor_plugin_export                 (NMVpnEditorPlugin *plugin,
                                                           const char *path,
                                                           NMConnection *connection,
                                                           GError **error);
char         *nm_vpn_editor_plugin_get_suggested_filename (NMVpnEditorPlugin *plugin,
                                                           NMConnection *connection);

/**************************************************/
/* Editor interface                               */
/**************************************************/

#define NM_TYPE_VPN_EDITOR               (nm_vpn_editor_get_type ())
#define NM_VPN_EDITOR(obj)               (G_TYPE_CHECK_INSTANCE_CAST ((obj), NM_TYPE_VPN_EDITOR, NMVpnEditor))
#define NM_IS_VPN_EDITOR(obj)            (G_TYPE_CHECK_INSTANCE_TYPE ((obj), NM_TYPE_VPN_EDITOR))
#define NM_VPN_EDITOR_GET_INTERFACE(obj) (G_TYPE_INSTANCE_GET_INTERFACE ((obj), NM_TYPE_VPN_EDITOR, NMVpnEditorInterface))

/**
 * NMVpnEditorInterface:
 * @g_iface: the parent interface
 * @get_widget: return the #GtkWidget for the VPN editor's UI
 * @placeholder: not currently used
 * @update_connection: called to save the user-entered options to the connection
 *   object.  Should return %FALSE and set @error if the current options are
 *   invalid.  @error should contain enough information for the plugin to
 *   determine which UI widget is invalid at a later point in time.  For
 *   example, creating unique error codes for what error occurred and populating
 *   the message field of @error with the name of the invalid property.
 * @changed: emitted when the value of a UI widget changes.  May trigger a
 *   validity check via @update_connection to write values to the connection.
 *
 * Interface for editing a specific #NMConnection
 */
typedef struct {
	GTypeInterface g_iface;

	GObject * (*get_widget) (NMVpnEditor *editor);

	void (*placeholder) (void);

	gboolean (*update_connection) (NMVpnEditor *editor,
	                               NMConnection *connection,
	                               GError **error);

	void (*changed) (NMVpnEditor *editor);
} NMVpnEditorInterface;

GType nm_vpn_editor_get_type (void);

GObject * nm_vpn_editor_get_widget (NMVpnEditor *editor);

gboolean nm_vpn_editor_update_connection (NMVpnEditor *editor,
                                          NMConnection *connection,
                                          GError **error);

G_END_DECLS

#endif	/* NM_VPN_EDITOR_PLUGIN_H */
