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
 * Copyright 2013 Red Hat, Inc.
 */

/**
 * SECTION:nmt-page-ip4
 * @short_description: The editor page for IP4 configuration
 */

#include "config.h"

#include <stdlib.h>

#include <glib.h>
#include <glib/gi18n-lib.h>

#include "nmt-page-ip4.h"
#include "nmt-ip-entry.h"
#include "nmt-address-list.h"
#include "nmt-route-editor.h"

#include "nm-editor-bindings.h"

G_DEFINE_TYPE (NmtPageIP4, nmt_page_ip4, NMT_TYPE_EDITOR_PAGE)

static NmtNewtPopupEntry ip4methods[] = {
	{ N_("Disabled"), NM_SETTING_IP4_CONFIG_METHOD_DISABLED },
	{ N_("Automatic"), NM_SETTING_IP4_CONFIG_METHOD_AUTO },
	{ N_("Link-Local"), NM_SETTING_IP4_CONFIG_METHOD_LINK_LOCAL },
	{ N_("Manual"), NM_SETTING_IP4_CONFIG_METHOD_MANUAL },
	{ N_("Shared"), NM_SETTING_IP4_CONFIG_METHOD_SHARED },
	{ NULL, NULL }
};

NmtNewtWidget *
nmt_page_ip4_new (NMConnection *conn)
{
	return g_object_new (NMT_TYPE_PAGE_IP4,
	                     "connection", conn,
	                     "title", _("IPv4 CONFIGURATION"),
	                     NULL);
}

gboolean
nmt_page_ip4_is_non_empty (NmtPageIP4 *ip4)
{
	NMConnection *conn;
	NMSettingIP4Config *s_ip4;

	conn = nmt_editor_page_get_connection (NMT_EDITOR_PAGE (ip4));
	s_ip4 = nm_connection_get_setting_ip4_config (conn);
	if (   !g_strcmp0 (nm_setting_ip4_config_get_method (s_ip4), NM_SETTING_IP4_CONFIG_METHOD_MANUAL)
	    || nm_setting_ip4_config_get_num_addresses (s_ip4))
		return TRUE;
	return FALSE;
}

static void
nmt_page_ip4_init (NmtPageIP4 *ip4)
{
}

static void
edit_routes (NmtNewtButton *button,
             gpointer       user_data)
{
	NMSetting *s_ip4 = user_data;
	NmtNewtForm *form;

	form = nmt_route_editor_new (s_ip4);
	nmt_newt_form_run_sync (form);
	g_object_unref (form);
}

static gboolean
ip4_routes_transform_to_description (GBinding     *binding,
                                     const GValue *source_value,
                                     GValue       *target_value,
                                     gpointer      user_data)
{
	GPtrArray *routes;
	char *text;

	routes = g_value_get_boxed (source_value);
	if (!routes || !routes->len)
		text = g_strdup (_("(No custom routes)"));
	else {
		text = g_strdup_printf (g_dngettext (GETTEXT_PACKAGE,
		                                     "One custom route",
		                                     "%d custom routes",
		                                     routes->len),
		                        routes->len);
	}

	g_value_take_string (target_value, text);
	return TRUE;
}

static void
nmt_page_ip4_constructed (GObject *object)
{
	NmtPageIP4 *ip4 = NMT_PAGE_IP4 (object);
	NmtPageGrid *grid;
	NMSettingIP4Config *s_ip4;
	NmtNewtWidget *widget, *button;
	NMConnection *conn;

	conn = nmt_editor_page_get_connection (NMT_EDITOR_PAGE (ip4));
	s_ip4 = nm_connection_get_setting_ip4_config (conn);
	if (!s_ip4) {
		s_ip4 = (NMSettingIP4Config *) nm_setting_ip4_config_new ();
		g_object_set (G_OBJECT (s_ip4),
		              NM_SETTING_IP4_CONFIG_METHOD, NM_SETTING_IP4_CONFIG_METHOD_AUTO,
		              NULL);
		nm_connection_add_setting (conn, (NMSetting *) s_ip4);
	}

	widget = nmt_newt_popup_new (ip4methods);
	g_object_bind_property (s_ip4, NM_SETTING_IP4_CONFIG_METHOD,
	                        widget, "active-id",
	                        G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
	nmt_editor_page_set_header_widget (NMT_EDITOR_PAGE (ip4), widget);

	grid = NMT_PAGE_GRID (ip4);

	widget = nmt_address_list_new (NMT_ADDRESS_LIST_IP4_WITH_PREFIX);
	nm_editor_bind_ip4_addresses_with_prefix_to_strv (s_ip4, NM_SETTING_IP4_CONFIG_ADDRESSES,
	                                                  widget, "strings",
	                                                  G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
	nmt_page_grid_append (grid, _("Addresses"), widget, NULL);

	widget = nmt_ip_entry_new (25, AF_INET, FALSE, TRUE);
	nm_editor_bind_ip4_gateway_to_string (s_ip4, NM_SETTING_IP4_CONFIG_ADDRESSES,
	                                      widget, "text",
	                                      G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
	nmt_page_grid_append (grid, _("Gateway"), widget, NULL);

	widget = nmt_address_list_new (NMT_ADDRESS_LIST_IP4);
	nm_editor_bind_ip4_addresses_to_strv (s_ip4, NM_SETTING_IP4_CONFIG_DNS,
	                                      widget, "strings",
	                                      G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
	nmt_page_grid_append (grid, _("DNS servers"), widget, NULL);

	widget = nmt_address_list_new (NMT_ADDRESS_LIST_HOSTNAME);
	g_object_bind_property (s_ip4, NM_SETTING_IP4_CONFIG_DNS_SEARCH,
	                        widget, "strings",
	                        G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
	nmt_page_grid_append (grid, _("Search domains"), widget, NULL);

	nmt_page_grid_append (grid, NULL, nmt_newt_separator_new (), NULL);

	widget = g_object_new (NMT_TYPE_NEWT_LABEL,
	                       "text", "",
	                       "style", NMT_NEWT_LABEL_PLAIN,
	                       NULL);
	g_object_bind_property_full (s_ip4, NM_SETTING_IP4_CONFIG_ROUTES,
	                             widget, "text",
	                             G_BINDING_SYNC_CREATE,
	                             ip4_routes_transform_to_description,
	                             NULL, NULL, NULL);
	button = nmt_newt_button_new (_("Edit..."));
	g_signal_connect (button, "clicked", G_CALLBACK (edit_routes), s_ip4);
	nmt_page_grid_append (grid, _("Routing"), widget, button);

	widget = nmt_newt_checkbox_new (_("Never use this network for default route"));
	g_object_bind_property (s_ip4, NM_SETTING_IP4_CONFIG_NEVER_DEFAULT,
	                        widget, "active",
	                        G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL);
	nmt_page_grid_append (grid, NULL, widget, NULL);

	nmt_page_grid_append (grid, NULL, nmt_newt_separator_new (), NULL);

	widget = nmt_newt_checkbox_new (_("Require IPv4 addressing for this connection"));
	g_object_bind_property (s_ip4, NM_SETTING_IP4_CONFIG_MAY_FAIL,
	                        widget, "active",
	                        G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL |
	                        G_BINDING_INVERT_BOOLEAN);
	nmt_page_grid_append (grid, NULL, widget, NULL);

	G_OBJECT_CLASS (nmt_page_ip4_parent_class)->constructed (object);
}

static void
nmt_page_ip4_class_init (NmtPageIP4Class *ip4_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (ip4_class);

	object_class->constructed = nmt_page_ip4_constructed;
}
