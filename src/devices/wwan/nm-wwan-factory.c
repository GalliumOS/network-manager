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
 * Copyright (C) 2014 Red Hat, Inc.
 */

#include <string.h>
#include <gmodule.h>

#include "config.h"
#include "nm-device-factory.h"
#include "nm-wwan-factory.h"
#include "nm-modem-manager.h"
#include "nm-device-modem.h"
#include "nm-logging.h"

static GType nm_wwan_factory_get_type (void);

static void device_factory_interface_init (NMDeviceFactory *factory_iface);

G_DEFINE_TYPE_EXTENDED (NMWwanFactory, nm_wwan_factory, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (NM_TYPE_DEVICE_FACTORY, device_factory_interface_init))

#define NM_WWAN_FACTORY_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), NM_TYPE_WWAN_FACTORY, NMWwanFactoryPrivate))

typedef struct {
	NMModemManager *mm;
} NMWwanFactoryPrivate;

/************************************************************************/

#define PLUGIN_TYPE NM_DEVICE_TYPE_MODEM

G_MODULE_EXPORT NMDeviceFactory *
nm_device_factory_create (GError **error)
{
	return (NMDeviceFactory *) g_object_new (NM_TYPE_WWAN_FACTORY, NULL);
}

G_MODULE_EXPORT NMDeviceType
nm_device_factory_get_device_type (void)
{
	return PLUGIN_TYPE;
}

/************************************************************************/

static void
modem_added_cb (NMModemManager *manager,
                NMModem *modem,
                gpointer user_data)
{
	NMWwanFactory *self = NM_WWAN_FACTORY (user_data);
	NMDevice *device;
	const char *driver, *port;

	/* Do nothing if the modem was consumed by some other plugin */
	if (nm_device_factory_emit_component_added (NM_DEVICE_FACTORY (self), G_OBJECT (modem)))
		return;

	driver = nm_modem_get_driver (modem);

	/* If it was a Bluetooth modem and no bluetooth device claimed it, ignore
	 * it.  The rfcomm port (and thus the modem) gets created automatically
	 * by the Bluetooth code during the connection process.
	 */
	if (driver && strstr (driver, "bluetooth")) {
		port = nm_modem_get_data_port (modem);
		if (!port)
			port = nm_modem_get_control_port (modem);
		nm_log_info (LOGD_MB, "ignoring modem '%s' (no associated Bluetooth device)", port);
		return;
	}

	/* Make the new modem device */
	device = nm_device_modem_new (modem);
	g_assert (device);
	g_signal_emit_by_name (self, NM_DEVICE_FACTORY_DEVICE_ADDED, device);
	g_object_unref (device);
}

static void
nm_wwan_factory_init (NMWwanFactory *self)
{
	NMWwanFactoryPrivate *priv = NM_WWAN_FACTORY_GET_PRIVATE (self);

	priv->mm = g_object_new (NM_TYPE_MODEM_MANAGER, NULL);
	g_assert (priv->mm);
	g_signal_connect (priv->mm,
	                  NM_MODEM_MANAGER_MODEM_ADDED,
	                  G_CALLBACK (modem_added_cb),
	                  self);
}

static void
device_factory_interface_init (NMDeviceFactory *factory_iface)
{
}

static void
dispose (GObject *object)
{
	NMWwanFactory *self = NM_WWAN_FACTORY (object);
	NMWwanFactoryPrivate *priv = NM_WWAN_FACTORY_GET_PRIVATE (self);

	if (priv->mm)
		g_signal_handlers_disconnect_by_func (priv->mm, modem_added_cb, self);
	g_clear_object (&priv->mm);

	/* Chain up to the parent class */
	G_OBJECT_CLASS (nm_wwan_factory_parent_class)->dispose (object);
}

static void
nm_wwan_factory_class_init (NMWwanFactoryClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	g_type_class_add_private (object_class, sizeof (NMWwanFactoryPrivate));

	object_class->dispose = dispose;
}
