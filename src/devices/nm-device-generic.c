/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager -- Network link manager
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
 * Copyright 2013 Red Hat, Inc.
 */

#include "config.h"

#include "nm-device-generic.h"
#include "nm-device-private.h"
#include "nm-enum-types.h"
#include "nm-platform.h"
#include "nm-utils.h"
#include "nm-glib-compat.h"
#include "nm-dbus-manager.h"

#include "nm-device-generic-glue.h"

G_DEFINE_TYPE (NMDeviceGeneric, nm_device_generic, NM_TYPE_DEVICE)

#define NM_DEVICE_GENERIC_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), NM_TYPE_DEVICE_GENERIC, NMDeviceGenericPrivate))

typedef struct {
	char *type_description;
} NMDeviceGenericPrivate;

enum {
	PROP_0,
	PROP_TYPE_DESCRIPTION,

	LAST_PROP
};

#define NM_DEVICE_GENERIC_ERROR (nm_device_generic_error_quark ())

static GQuark
nm_device_generic_error_quark (void)
{
	static GQuark quark = 0;
	if (!quark)
		quark = g_quark_from_static_string ("nm-device-generic-error");
	return quark;
}

/**************************************************************/

static guint32
get_generic_capabilities (NMDevice *dev)
{
	if (nm_platform_link_supports_carrier_detect (nm_device_get_ifindex (dev)))
		return NM_DEVICE_CAP_CARRIER_DETECT;
	else
		return NM_DEVICE_CAP_NONE;
}

static gboolean
check_connection_compatible (NMDevice *device, NMConnection *connection)
{
	NMSettingConnection *s_con;

	if (!NM_DEVICE_CLASS (nm_device_generic_parent_class)->check_connection_compatible (device, connection))
		return FALSE;

	if (!nm_connection_is_type (connection, NM_SETTING_GENERIC_SETTING_NAME))
		return FALSE;

	s_con = nm_connection_get_setting_connection (connection);
	if (!nm_setting_connection_get_interface_name (s_con))
		return FALSE;

	return TRUE;
}

static void
update_connection (NMDevice *device, NMConnection *connection)
{
	NMSettingConnection *s_con;

	if (!nm_connection_get_setting_generic (connection))
		nm_connection_add_setting (connection, nm_setting_generic_new ());

	s_con = nm_connection_get_setting_connection (connection);
	g_assert (s_con);
	g_object_set (G_OBJECT (s_con),
	              NM_SETTING_CONNECTION_INTERFACE_NAME, nm_device_get_iface (device),
	              NULL);
}

/**************************************************************/

NMDevice *
nm_device_generic_new (NMPlatformLink *platform_device)
{
	g_return_val_if_fail (platform_device != NULL, NULL);

	return (NMDevice *) g_object_new (NM_TYPE_DEVICE_GENERIC,
	                                  NM_DEVICE_PLATFORM_DEVICE, platform_device,
	                                  NM_DEVICE_TYPE_DESC, "Generic",
	                                  NM_DEVICE_DEVICE_TYPE, NM_DEVICE_TYPE_GENERIC,
	                                  NULL);
}

static void
nm_device_generic_init (NMDeviceGeneric *self)
{
	nm_device_set_initial_unmanaged_flag (NM_DEVICE (self), NM_UNMANAGED_DEFAULT, TRUE);
}

static void
constructed (GObject *object)
{
	NMDeviceGeneric *self = NM_DEVICE_GENERIC (object);
	NMDeviceGenericPrivate *priv = NM_DEVICE_GENERIC_GET_PRIVATE (self);

	if (!priv->type_description) {
		int ifindex = nm_device_get_ip_ifindex (NM_DEVICE (self));

		if (ifindex != 0)
			priv->type_description = g_strdup (nm_platform_link_get_type_name (ifindex));
	}

	G_OBJECT_CLASS (nm_device_generic_parent_class)->constructed (object);
}

static void
dispose (GObject *object)
{
	NMDeviceGeneric *self = NM_DEVICE_GENERIC (object);
	NMDeviceGenericPrivate *priv = NM_DEVICE_GENERIC_GET_PRIVATE (self);

	g_clear_pointer (&priv->type_description, g_free);

	G_OBJECT_CLASS (nm_device_generic_parent_class)->dispose (object);
}

static void
get_property (GObject *object, guint prop_id,
              GValue *value, GParamSpec *pspec)
{
	NMDeviceGeneric *self = NM_DEVICE_GENERIC (object);
	NMDeviceGenericPrivate *priv = NM_DEVICE_GENERIC_GET_PRIVATE (self);

	switch (prop_id) {
	case PROP_TYPE_DESCRIPTION:
		g_value_set_string (value, priv->type_description);
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
	NMDeviceGeneric *self = NM_DEVICE_GENERIC (object);
	NMDeviceGenericPrivate *priv = NM_DEVICE_GENERIC_GET_PRIVATE (self);

	switch (prop_id) {
	case PROP_TYPE_DESCRIPTION:
		priv->type_description = g_value_dup_string (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
nm_device_generic_class_init (NMDeviceGenericClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	NMDeviceClass *parent_class = NM_DEVICE_CLASS (klass);

	g_type_class_add_private (klass, sizeof (NMDeviceGenericPrivate));

	parent_class->connection_type = NM_SETTING_GENERIC_SETTING_NAME;

	object_class->constructed = constructed;
	object_class->dispose = dispose;
	object_class->get_property = get_property;
	object_class->set_property = set_property;

	parent_class->get_generic_capabilities = get_generic_capabilities;
	parent_class->check_connection_compatible = check_connection_compatible;
	parent_class->update_connection = update_connection;

	/* properties */
	g_object_class_install_property
		(object_class, PROP_TYPE_DESCRIPTION,
		 g_param_spec_string (NM_DEVICE_GENERIC_TYPE_DESCRIPTION,
		                      "Type Description",
		                      "Type description",
		                      NULL,
		                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	nm_dbus_manager_register_exported_type (nm_dbus_manager_get (),
	                                        G_TYPE_FROM_CLASS (klass),
	                                        &dbus_glib_nm_device_generic_object_info);

	dbus_g_error_domain_register (NM_DEVICE_GENERIC_ERROR, NULL, NM_TYPE_DEVICE_GENERIC_ERROR);
}
