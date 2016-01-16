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

#include <string.h>

#include "nm-device-macvlan.h"
#include "nm-device-private.h"
#include "nm-dbus-manager.h"
#include "nm-logging.h"
#include "nm-manager.h"
#include "nm-platform.h"

#include "nm-device-macvlan-glue.h"

G_DEFINE_TYPE (NMDeviceMacvlan, nm_device_macvlan, NM_TYPE_DEVICE_GENERIC)

#define NM_DEVICE_MACVLAN_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), NM_TYPE_DEVICE_MACVLAN, NMDeviceMacvlanPrivate))

typedef struct {
	NMPlatformMacvlanProperties props;
} NMDeviceMacvlanPrivate;

enum {
	PROP_0,
	PROP_PARENT,
	PROP_MODE,
	PROP_NO_PROMISC,

	LAST_PROP
};

/**************************************************************/

/**************************************************************/

static void
update_properties (NMDevice *device)
{
	NMDeviceMacvlanPrivate *priv = NM_DEVICE_MACVLAN_GET_PRIVATE (device);
	GObject *object = G_OBJECT (device);
	NMPlatformMacvlanProperties props;

	if (!nm_platform_macvlan_get_properties (nm_device_get_ifindex (device), &props)) {
		nm_log_warn (LOGD_HW, "(%s): could not read macvlan properties",
		             nm_device_get_iface (device));
		return;
	}

	g_object_freeze_notify (object);

	if (priv->props.parent_ifindex != props.parent_ifindex)
		g_object_notify (object, NM_DEVICE_MACVLAN_PARENT);
	if (g_strcmp0 (priv->props.mode, props.mode) != 0)
		g_object_notify (object, NM_DEVICE_MACVLAN_MODE);
	if (priv->props.no_promisc != props.no_promisc)
		g_object_notify (object, NM_DEVICE_MACVLAN_NO_PROMISC);

	memcpy (&priv->props, &props, sizeof (NMPlatformMacvlanProperties));

	g_object_thaw_notify (object);
}

static void
link_changed (NMDevice *device, NMPlatformLink *info)
{
	NM_DEVICE_CLASS (nm_device_macvlan_parent_class)->link_changed (device, info);
	update_properties (device);
}

/**************************************************************/

NMDevice *
nm_device_macvlan_new (NMPlatformLink *platform_device)
{
	g_return_val_if_fail (platform_device != NULL, NULL);

	return (NMDevice *) g_object_new (NM_TYPE_DEVICE_MACVLAN,
	                                  NM_DEVICE_PLATFORM_DEVICE, platform_device,
	                                  NM_DEVICE_TYPE_DESC, "Macvlan",
	                                  NM_DEVICE_DEVICE_TYPE, NM_DEVICE_TYPE_GENERIC,
	                                  NULL);
}

static void
nm_device_macvlan_init (NMDeviceMacvlan *self)
{
}

static void
constructed (GObject *object)
{
	update_properties (NM_DEVICE (object));

	G_OBJECT_CLASS (nm_device_macvlan_parent_class)->constructed (object);
}

static void
get_property (GObject *object, guint prop_id,
              GValue *value, GParamSpec *pspec)
{
	NMDeviceMacvlanPrivate *priv = NM_DEVICE_MACVLAN_GET_PRIVATE (object);
	NMDevice *parent;

	switch (prop_id) {
	case PROP_PARENT:
		parent = nm_manager_get_device_by_ifindex (nm_manager_get (), priv->props.parent_ifindex);
		g_value_set_boxed (value, parent ? nm_device_get_path (parent) : "/");
		break;
	case PROP_MODE:
		g_value_set_string (value, priv->props.mode);
		break;
	case PROP_NO_PROMISC:
		g_value_set_boolean (value, priv->props.no_promisc);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
nm_device_macvlan_class_init (NMDeviceMacvlanClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	NMDeviceClass *device_class = NM_DEVICE_CLASS (klass);

	g_type_class_add_private (klass, sizeof (NMDeviceMacvlanPrivate));

	object_class->constructed = constructed;
	object_class->get_property = get_property;

	device_class->link_changed = link_changed;

	/* properties */
	g_object_class_install_property
		(object_class, PROP_PARENT,
		 g_param_spec_boxed (NM_DEVICE_MACVLAN_PARENT,
		                     "Parent",
		                     "Parent device",
		                     DBUS_TYPE_G_OBJECT_PATH,
		                     G_PARAM_READABLE));

	g_object_class_install_property
		(object_class, PROP_MODE,
		 g_param_spec_string (NM_DEVICE_MACVLAN_MODE,
		                      "Mode",
		                      "Mode: 'private', 'vepa', 'bridge', or 'passthru'",
		                      NULL,
		                      G_PARAM_READABLE));

	g_object_class_install_property
		(object_class, PROP_NO_PROMISC,
		 g_param_spec_boolean (NM_DEVICE_MACVLAN_NO_PROMISC,
		                       "No-promisc",
		                       "No promiscuous mode",
		                       FALSE,
		                       G_PARAM_READABLE));

	nm_dbus_manager_register_exported_type (nm_dbus_manager_get (),
	                                        G_TYPE_FROM_CLASS (klass),
	                                        &dbus_glib_nm_device_macvlan_object_info);
}
