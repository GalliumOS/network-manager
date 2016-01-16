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
 * Copyright 2013, 2014 Red Hat, Inc.
 */

#include "config.h"

#include <string.h>

#include "nm-device-vxlan.h"
#include "nm-device-private.h"
#include "nm-dbus-manager.h"
#include "nm-logging.h"
#include "nm-manager.h"
#include "nm-platform.h"
#include "nm-utils.h"

#include "nm-device-vxlan-glue.h"

G_DEFINE_TYPE (NMDeviceVxlan, nm_device_vxlan, NM_TYPE_DEVICE_GENERIC)

#define NM_DEVICE_VXLAN_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), NM_TYPE_DEVICE_VXLAN, NMDeviceVxlanPrivate))

typedef struct {
	NMPlatformVxlanProperties props;
} NMDeviceVxlanPrivate;

enum {
	PROP_0,
	PROP_PARENT,
	PROP_ID,
	PROP_GROUP,
	PROP_LOCAL,
	PROP_TOS,
	PROP_TTL,
	PROP_LEARNING,
	PROP_AGEING,
	PROP_LIMIT,
	PROP_DST_PORT,
	PROP_SRC_PORT_MIN,
	PROP_SRC_PORT_MAX,
	PROP_PROXY,
	PROP_RSC,
	PROP_L2MISS,
	PROP_L3MISS,

	LAST_PROP
};

/**************************************************************/

static void
update_properties (NMDevice *device)
{
	NMDeviceVxlanPrivate *priv = NM_DEVICE_VXLAN_GET_PRIVATE (device);
	GObject *object = G_OBJECT (device);
	NMPlatformVxlanProperties props;

	if (!nm_platform_vxlan_get_properties (nm_device_get_ifindex (device), &props)) {
		nm_log_warn (LOGD_HW, "(%s): could not read vxlan properties",
		             nm_device_get_iface (device));
		return;
	}

	g_object_freeze_notify (object);

	if (priv->props.parent_ifindex != props.parent_ifindex)
		g_object_notify (object, NM_DEVICE_VXLAN_PARENT);
	if (priv->props.id != props.id)
		g_object_notify (object, NM_DEVICE_VXLAN_ID);
	if (priv->props.group != props.group)
		g_object_notify (object, NM_DEVICE_VXLAN_GROUP);
	if (priv->props.local != props.local)
		g_object_notify (object, NM_DEVICE_VXLAN_LOCAL);
	if (memcmp (&priv->props.group6, &props.group6, sizeof (props.group6)) != 0)
		g_object_notify (object, NM_DEVICE_VXLAN_GROUP);
	if (memcmp (&priv->props.local6, &props.local6, sizeof (props.local6)) != 0)
		g_object_notify (object, NM_DEVICE_VXLAN_LOCAL);
	if (priv->props.tos != props.tos)
		g_object_notify (object, NM_DEVICE_VXLAN_TOS);
	if (priv->props.ttl != props.ttl)
		g_object_notify (object, NM_DEVICE_VXLAN_TTL);
	if (priv->props.learning != props.learning)
		g_object_notify (object, NM_DEVICE_VXLAN_LEARNING);
	if (priv->props.ageing != props.ageing)
		g_object_notify (object, NM_DEVICE_VXLAN_AGEING);
	if (priv->props.limit != props.limit)
		g_object_notify (object, NM_DEVICE_VXLAN_LIMIT);
	if (priv->props.dst_port != props.dst_port)
		g_object_notify (object, NM_DEVICE_VXLAN_DST_PORT);
	if (priv->props.src_port_min != props.src_port_min)
		g_object_notify (object, NM_DEVICE_VXLAN_SRC_PORT_MIN);
	if (priv->props.src_port_max != props.src_port_max)
		g_object_notify (object, NM_DEVICE_VXLAN_SRC_PORT_MAX);
	if (priv->props.proxy != props.proxy)
		g_object_notify (object, NM_DEVICE_VXLAN_PROXY);
	if (priv->props.rsc != props.rsc)
		g_object_notify (object, NM_DEVICE_VXLAN_RSC);
	if (priv->props.l2miss != props.l2miss)
		g_object_notify (object, NM_DEVICE_VXLAN_L2MISS);
	if (priv->props.l3miss != props.l3miss)
		g_object_notify (object, NM_DEVICE_VXLAN_L3MISS);

	memcpy (&priv->props, &props, sizeof (NMPlatformVxlanProperties));

	g_object_thaw_notify (object);
}

static void
link_changed (NMDevice *device, NMPlatformLink *info)
{
	NM_DEVICE_CLASS (nm_device_vxlan_parent_class)->link_changed (device, info);
	update_properties (device);
}

/**************************************************************/

NMDevice *
nm_device_vxlan_new (NMPlatformLink *platform_device)
{
	g_return_val_if_fail (platform_device != NULL, NULL);

	return (NMDevice *) g_object_new (NM_TYPE_DEVICE_VXLAN,
	                                  NM_DEVICE_PLATFORM_DEVICE, platform_device,
	                                  NM_DEVICE_TYPE_DESC, "Vxlan",
	                                  NM_DEVICE_DEVICE_TYPE, NM_DEVICE_TYPE_GENERIC,
	                                  NULL);
}

static void
nm_device_vxlan_init (NMDeviceVxlan *self)
{
}

static void
constructed (GObject *object)
{
	update_properties (NM_DEVICE (object));

	G_OBJECT_CLASS (nm_device_vxlan_parent_class)->constructed (object);
}

static void
get_property (GObject *object, guint prop_id,
              GValue *value, GParamSpec *pspec)
{
	NMDeviceVxlanPrivate *priv = NM_DEVICE_VXLAN_GET_PRIVATE (object);
	NMDevice *parent;

	switch (prop_id) {
	case PROP_PARENT:
		parent = nm_manager_get_device_by_ifindex (nm_manager_get (), priv->props.parent_ifindex);
		g_value_set_boxed (value, parent ? nm_device_get_path (parent) : "/");
		break;
	case PROP_ID:
		g_value_set_uint (value, priv->props.id);
		break;
	case PROP_GROUP:
		if (priv->props.group)
			g_value_set_string (value, nm_utils_inet4_ntop (priv->props.group, NULL));
		else if (!IN6_IS_ADDR_UNSPECIFIED (&priv->props.group6))
			g_value_set_string (value, nm_utils_inet6_ntop (&priv->props.group6, NULL));
		break;
	case PROP_LOCAL:
		if (priv->props.local)
			g_value_set_string (value, nm_utils_inet4_ntop (priv->props.local, NULL));
		else if (!IN6_IS_ADDR_UNSPECIFIED (&priv->props.local6))
			g_value_set_string (value, nm_utils_inet6_ntop (&priv->props.local6, NULL));
		break;
	case PROP_TOS:
		g_value_set_uchar (value, priv->props.tos);
		break;
	case PROP_TTL:
		g_value_set_uchar (value, priv->props.ttl);
		break;
	case PROP_LEARNING:
		g_value_set_boolean (value, priv->props.learning);
		break;
	case PROP_AGEING:
		g_value_set_uint (value, priv->props.ageing);
		break;
	case PROP_LIMIT:
		g_value_set_uint (value, priv->props.limit);
		break;
	case PROP_DST_PORT:
		g_value_set_uint (value, priv->props.dst_port);
		break;
	case PROP_SRC_PORT_MIN:
		g_value_set_uint (value, priv->props.src_port_min);
		break;
	case PROP_SRC_PORT_MAX:
		g_value_set_uint (value, priv->props.src_port_max);
		break;
	case PROP_PROXY:
		g_value_set_uint (value, priv->props.proxy);
		break;
	case PROP_RSC:
		g_value_set_boolean (value, priv->props.rsc);
		break;
	case PROP_L2MISS:
		g_value_set_boolean (value, priv->props.l2miss);
		break;
	case PROP_L3MISS:
		g_value_set_boolean (value, priv->props.l3miss);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
nm_device_vxlan_class_init (NMDeviceVxlanClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	NMDeviceClass *device_class = NM_DEVICE_CLASS (klass);

	g_type_class_add_private (klass, sizeof (NMDeviceVxlanPrivate));

	object_class->constructed = constructed;
	object_class->get_property = get_property;

	device_class->link_changed = link_changed;

	/* properties */
	g_object_class_install_property
		(object_class, PROP_PARENT,
		 g_param_spec_boxed (NM_DEVICE_VXLAN_PARENT,
		                     "Parent",
		                     "Parent device",
		                     DBUS_TYPE_G_OBJECT_PATH,
		                     G_PARAM_READABLE));

	g_object_class_install_property
		(object_class, PROP_ID,
		 g_param_spec_uint (NM_DEVICE_VXLAN_ID,
		                    "Id",
		                    "Id",
		                    0, G_MAXUINT32, 0,
		                    G_PARAM_READABLE));

	g_object_class_install_property
		(object_class, PROP_GROUP,
		 g_param_spec_string (NM_DEVICE_VXLAN_GROUP,
		                      "Group",
		                      "Group",
		                      NULL,
		                      G_PARAM_READABLE));

	g_object_class_install_property
		(object_class, PROP_LOCAL,
		 g_param_spec_string (NM_DEVICE_VXLAN_LOCAL,
		                      "Local",
		                      "Local",
		                      NULL,
		                      G_PARAM_READABLE));

	g_object_class_install_property
		(object_class, PROP_TOS,
		 g_param_spec_uchar (NM_DEVICE_VXLAN_TOS,
		                     "ToS",
		                     "ToS",
		                     0, 255, 0,
		                     G_PARAM_READABLE));

	g_object_class_install_property
		(object_class, PROP_TTL,
		 g_param_spec_uchar (NM_DEVICE_VXLAN_TTL,
		                     "TTL",
		                     "TTL",
		                     0, 255, 0,
		                     G_PARAM_READABLE));

	g_object_class_install_property
		(object_class, PROP_LEARNING,
		 g_param_spec_boolean (NM_DEVICE_VXLAN_LEARNING,
		                       "Learning",
		                       "Learning",
		                       FALSE,
		                       G_PARAM_READABLE));

	g_object_class_install_property
		(object_class, PROP_AGEING,
		 g_param_spec_uint (NM_DEVICE_VXLAN_AGEING,
		                    "Ageing",
		                    "Ageing",
		                    0, G_MAXUINT32, 0,
		                    G_PARAM_READABLE));

	g_object_class_install_property
		(object_class, PROP_LIMIT,
		 g_param_spec_uint (NM_DEVICE_VXLAN_LIMIT,
		                    "Limit",
		                    "Limit",
		                    0, G_MAXUINT32, 0,
		                    G_PARAM_READABLE));

	g_object_class_install_property
		(object_class, PROP_DST_PORT,
		 g_param_spec_uint (NM_DEVICE_VXLAN_DST_PORT,
		                    "Destination port",
		                    "Destination port",
		                    0, 65535, 0,
		                    G_PARAM_READABLE));

	g_object_class_install_property
		(object_class, PROP_SRC_PORT_MIN,
		 g_param_spec_uint (NM_DEVICE_VXLAN_SRC_PORT_MIN,
		                    "Source port min",
		                    "Minimum source port",
		                    0, 65535, 0,
		                    G_PARAM_READABLE));

	g_object_class_install_property
		(object_class, PROP_SRC_PORT_MAX,
		 g_param_spec_uint (NM_DEVICE_VXLAN_SRC_PORT_MAX,
		                    "Source port max",
		                    "Maximum source port",
		                    0, 65535, 0,
		                    G_PARAM_READABLE));

	g_object_class_install_property
		(object_class, PROP_PROXY,
		 g_param_spec_boolean (NM_DEVICE_VXLAN_PROXY,
		                       "Proxy",
		                       "Proxy",
		                       FALSE,
		                       G_PARAM_READABLE));

	g_object_class_install_property
		(object_class, PROP_RSC,
		 g_param_spec_boolean (NM_DEVICE_VXLAN_RSC,
		                       "RSC",
		                       "RSC",
		                       FALSE,
		                       G_PARAM_READABLE));

	g_object_class_install_property
		(object_class, PROP_L2MISS,
		 g_param_spec_boolean (NM_DEVICE_VXLAN_L2MISS,
		                       "L2miss",
		                       "L2miss",
		                       FALSE,
		                       G_PARAM_READABLE));

	g_object_class_install_property
		(object_class, PROP_L3MISS,
		 g_param_spec_boolean (NM_DEVICE_VXLAN_L3MISS,
		                       "L3miss",
		                       "L3miss",
		                       FALSE,
		                       G_PARAM_READABLE));

	nm_dbus_manager_register_exported_type (nm_dbus_manager_get (),
	                                        G_TYPE_FROM_CLASS (klass),
	                                        &dbus_glib_nm_device_vxlan_object_info);
}
