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
 * Copyright (C) 2005 - 2014 Red Hat, Inc.
 * Copyright (C) 2006 - 2008 Novell, Inc.
 */

#include "config.h"
#include <glib.h>
#include <glib/gi18n.h>
#include <netinet/in.h>
#include <string.h>
#include <stdlib.h>
#include <linux/sockios.h>
#include <linux/ethtool.h>
#include <linux/version.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/if.h>
#include <errno.h>
#include <netinet/ether.h>

#include <gudev/gudev.h>

#include "nm-glib-compat.h"
#include "nm-device-ethernet.h"
#include "nm-device-private.h"
#include "nm-activation-request.h"
#include "NetworkManagerUtils.h"
#include "nm-supplicant-manager.h"
#include "nm-supplicant-interface.h"
#include "nm-supplicant-config.h"
#include "nm-setting-connection.h"
#include "nm-setting-wired.h"
#include "nm-setting-8021x.h"
#include "nm-setting-pppoe.h"
#include "nm-setting-bond.h"
#include "ppp-manager/nm-ppp-manager.h"
#include "nm-logging.h"
#include "nm-utils.h"
#include "nm-enum-types.h"
#include "nm-dbus-manager.h"
#include "nm-platform.h"
#include "nm-dcb.h"
#include "nm-settings-connection.h"

#include "nm-device-ethernet-glue.h"


G_DEFINE_TYPE (NMDeviceEthernet, nm_device_ethernet, NM_TYPE_DEVICE)

#define NM_DEVICE_ETHERNET_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), NM_TYPE_DEVICE_ETHERNET, NMDeviceEthernetPrivate))

#define WIRED_SECRETS_TRIES "wired-secrets-tries"

#define PPPOE_RECONNECT_DELAY 7

#define NM_ETHERNET_ERROR (nm_ethernet_error_quark ())

static NMSetting *device_get_setting (NMDevice *device, GType setting_type);

typedef struct Supplicant {
	NMSupplicantManager *mgr;
	NMSupplicantInterface *iface;

	/* signal handler ids */
	guint iface_error_id;
	guint iface_state_id;

	/* Timeouts and idles */
	guint iface_con_error_cb_id;
	guint con_timeout_id;
} Supplicant;

typedef enum {
	DCB_WAIT_UNKNOWN = 0,
	/* Ensure carrier is up before enabling DCB */
	DCB_WAIT_CARRIER_PREENABLE_UP,
	/* Wait for carrier down when device starts enabling */
	DCB_WAIT_CARRIER_PRECONFIG_DOWN,
	/* Wait for carrier up when device has finished enabling */
	DCB_WAIT_CARRIER_PRECONFIG_UP,
	/* Wait carrier down when device starts configuring */
	DCB_WAIT_CARRIER_POSTCONFIG_DOWN,
	/* Wait carrier up when device has finished configuring */
	DCB_WAIT_CARRIER_POSTCONFIG_UP,
} DcbWait;

typedef struct {
	guint8              perm_hw_addr[ETH_ALEN];    /* Permanent MAC address */
	guint8              initial_hw_addr[ETH_ALEN]; /* Initial MAC address (as seen when NM starts) */

	guint32             speed;

	Supplicant          supplicant;
	guint               supplicant_timeout_id;

	/* s390 */
	char *              subchan1;
	char *              subchan2;
	char *              subchan3;
	char *              subchannels; /* Composite used for checking unmanaged specs */
	char *              s390_nettype;
	GHashTable *        s390_options;

	/* PPPoE */
	NMPPPManager *ppp_manager;
	NMIP4Config  *pending_ip4_config;
	gint32        last_pppoe_time;
	guint         pppoe_wait_id;

	/* DCB */
	DcbWait       dcb_wait;
	guint         dcb_timeout_id;
	guint         dcb_carrier_id;
} NMDeviceEthernetPrivate;

enum {
	PROP_0,
	PROP_PERM_HW_ADDRESS,
	PROP_SPEED,

	LAST_PROP
};


static GQuark
nm_ethernet_error_quark (void)
{
	static GQuark quark = 0;
	if (!quark)
		quark = g_quark_from_static_string ("nm-ethernet-error");
	return quark;
}

static char *
get_link_basename (const char *parent_path, const char *name, GError **error)
{
	char *link_dest, *path;
	char *result = NULL;

	path = g_strdup_printf ("%s/%s", parent_path, name);
	link_dest = g_file_read_link (path, error);
	if (link_dest) {
		result = g_path_get_basename (link_dest);
		g_free (link_dest);
	}
	g_free (path);
	return result;
}

static void
_update_s390_subchannels (NMDeviceEthernet *self)
{
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (self);
	const char *iface;
	GUdevClient *client;
	GUdevDevice *dev;
	GUdevDevice *parent = NULL;
	const char *parent_path, *item, *driver;
	const char *subsystems[] = { "net", NULL };
	GDir *dir;
	GError *error = NULL;

	iface = nm_device_get_iface (NM_DEVICE (self));

	client = g_udev_client_new (subsystems);
	if (!client) {
		nm_log_warn (LOGD_DEVICE | LOGD_HW, "(%s): failed to initialize GUdev client", iface);
		return;
	}

	dev = g_udev_client_query_by_subsystem_and_name (client, "net", iface);
	if (!dev) {
		nm_log_warn (LOGD_DEVICE | LOGD_HW, "(%s): failed to find device with udev", iface);
		goto out;
	}

	/* Try for the "ccwgroup" parent */
	parent = g_udev_device_get_parent_with_subsystem (dev, "ccwgroup", NULL);
	if (!parent) {
		/* FIXME: whatever 'lcs' devices' subsystem is here... */
		if (!parent) {
			/* Not an s390 device */
			goto out;
		}
	}

	parent_path = g_udev_device_get_sysfs_path (parent);
	dir = g_dir_open (parent_path, 0, &error);
	if (!dir) {
		nm_log_warn (LOGD_DEVICE | LOGD_HW, "(%s): failed to open directory '%s': %s",
		             iface, parent_path,
		             error && error->message ? error->message : "(unknown)");
		g_clear_error (&error);
		goto out;
	}

	while ((item = g_dir_read_name (dir))) {
		if (!strcmp (item, "cdev0")) {
			priv->subchan1 = get_link_basename (parent_path, "cdev0", &error);
		} else if (!strcmp (item, "cdev1")) {
			priv->subchan2 = get_link_basename (parent_path, "cdev1", &error);
		} else if (!strcmp (item, "cdev2")) {
			priv->subchan3 = get_link_basename (parent_path, "cdev2", &error);
		} else if (!strcmp (item, "driver")) {
			priv->s390_nettype = get_link_basename (parent_path, "driver", &error);
		} else if (   !strcmp (item, "layer2")
		           || !strcmp (item, "portname")
		           || !strcmp (item, "portno")) {
			char *path, *value;
			path = g_strdup_printf ("%s/%s", parent_path, item);
			value = nm_platform_sysctl_get (path);
			if (value && *value)
				g_hash_table_insert (priv->s390_options, g_strdup (item), g_strdup (value));
			else
				nm_log_warn (LOGD_DEVICE | LOGD_HW, "(%s): error reading %s", iface, path);
			g_free (path);
			g_free (value);
		}
		if (error) {
			nm_log_warn (LOGD_DEVICE | LOGD_HW, "(%s): %s", iface, error->message);
			g_clear_error (&error);
		}
	}

	g_dir_close (dir);

	if (priv->subchan3) {
		priv->subchannels = g_strdup_printf ("%s,%s,%s",
		                                     priv->subchan1,
		                                     priv->subchan2,
		                                     priv->subchan3);
	} else if (priv->subchan2) {
		priv->subchannels = g_strdup_printf ("%s,%s",
		                                     priv->subchan1,
		                                     priv->subchan2);
	} else
		priv->subchannels = g_strdup (priv->subchan1);

	driver = nm_device_get_driver (NM_DEVICE (self));
	nm_log_info (LOGD_DEVICE | LOGD_HW,
	             "(%s): found s390 '%s' subchannels [%s]",
	             iface, driver ? driver : "(unknown driver)", priv->subchannels);

out:
	if (parent)
		g_object_unref (parent);
	if (dev)
		g_object_unref (dev);
	g_object_unref (client);
}

static GObject*
constructor (GType type,
             guint n_construct_params,
             GObjectConstructParam *construct_params)
{
	GObject *object;
	NMDevice *self;
	int ifindex;

	object = G_OBJECT_CLASS (nm_device_ethernet_parent_class)->constructor (type,
	                                                                        n_construct_params,
	                                                                        construct_params);
	if (object) {
		self = NM_DEVICE (object);
		ifindex = nm_device_get_ifindex (self);

		g_assert (   nm_platform_link_get_type (ifindex) == NM_LINK_TYPE_ETHERNET
		          || nm_platform_link_get_type (ifindex) == NM_LINK_TYPE_VETH);

		nm_log_dbg (LOGD_HW | LOGD_ETHER, "(%s): kernel ifindex %d",
		            nm_device_get_iface (NM_DEVICE (self)),
		            nm_device_get_ifindex (NM_DEVICE (self)));

		/* s390 stuff */
		_update_s390_subchannels (NM_DEVICE_ETHERNET (self));
	}

	return object;
}

static void
clear_secrets_tries (NMDevice *device)
{
	NMActRequest *req;
	NMConnection *connection;

	req = nm_device_get_act_request (device);
	if (req) {
		connection = nm_act_request_get_connection (req);
		/* Clear wired secrets tries on success, failure, or when deactivating */
		g_object_set_data (G_OBJECT (connection), WIRED_SECRETS_TRIES, NULL);
	}
}

static void
device_state_changed (NMDevice *device,
                      NMDeviceState new_state,
                      NMDeviceState old_state,
                      NMDeviceStateReason reason)
{
	if (   new_state == NM_DEVICE_STATE_ACTIVATED
	    || new_state == NM_DEVICE_STATE_FAILED
	    || new_state == NM_DEVICE_STATE_DISCONNECTED)
		clear_secrets_tries (device);
}

static void
nm_device_ethernet_init (NMDeviceEthernet *self)
{
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (self);
	priv->s390_options = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
}

NMDevice *
nm_device_ethernet_new (NMPlatformLink *platform_device)
{
	g_return_val_if_fail (platform_device != NULL, NULL);

	return (NMDevice *) g_object_new (NM_TYPE_DEVICE_ETHERNET,
	                                  NM_DEVICE_PLATFORM_DEVICE, platform_device,
	                                  NM_DEVICE_TYPE_DESC, "Ethernet",
	                                  NM_DEVICE_DEVICE_TYPE, NM_DEVICE_TYPE_ETHERNET,
	                                  NULL);
}

static void
update_permanent_hw_address (NMDevice *dev)
{
	NMDeviceEthernet *self = NM_DEVICE_ETHERNET (dev);
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (self);
	struct ifreq req;
	struct ethtool_perm_addr *epaddr = NULL;
	int fd, ret;
	const guint8 *mac;

	fd = socket (PF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		nm_log_warn (LOGD_HW, "couldn't open control socket.");
		return;
	}

	/* Get permanent MAC address */
	memset (&req, 0, sizeof (struct ifreq));
	strncpy (req.ifr_name, nm_device_get_iface (dev), IFNAMSIZ);

	epaddr = g_malloc0 (sizeof (struct ethtool_perm_addr) + ETH_ALEN);
	epaddr->cmd = ETHTOOL_GPERMADDR;
	epaddr->size = ETH_ALEN;
	req.ifr_data = (void *) epaddr;

	errno = 0;
	ret = ioctl (fd, SIOCETHTOOL, &req);
	if ((ret < 0) || !nm_ethernet_address_is_valid ((struct ether_addr *) epaddr->data)) {
		nm_log_dbg (LOGD_HW | LOGD_ETHER, "(%s): unable to read permanent MAC address (error %d)",
		            nm_device_get_iface (dev), errno);
		/* Fall back to current address */
		mac = nm_device_get_hw_address (dev, NULL);
		if (mac)
			memcpy (epaddr->data, mac, ETH_ALEN);
		else
			memset (epaddr->data, 0, ETH_ALEN);
	}

	if (memcmp (&priv->perm_hw_addr, epaddr->data, ETH_ALEN)) {
		memcpy (&priv->perm_hw_addr, epaddr->data, ETH_ALEN);
		g_object_notify (G_OBJECT (dev), NM_DEVICE_ETHERNET_PERMANENT_HW_ADDRESS);
	}

	g_free (epaddr);
	close (fd);
}

static void
update_initial_hw_address (NMDevice *dev)
{
	NMDeviceEthernet *self = NM_DEVICE_ETHERNET (dev);
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (self);
	char *mac_str;
	const guint8 *mac;

	/* This sets initial MAC address from current MAC address. It should only
	 * be called from NMDevice constructor() to really get the initial address.
	 */
	mac = nm_device_get_hw_address (dev, NULL);
	if (mac)
		memcpy (priv->initial_hw_addr, mac, ETH_ALEN);

	mac_str = nm_utils_hwaddr_ntoa (priv->initial_hw_addr, ARPHRD_ETHER);
	nm_log_dbg (LOGD_DEVICE | LOGD_ETHER, "(%s): read initial MAC address %s",
	            nm_device_get_iface (dev), mac_str);
	g_free (mac_str);
}

static guint32
get_generic_capabilities (NMDevice *dev)
{
	if (nm_platform_link_supports_carrier_detect (nm_device_get_ifindex (dev)))
	    return NM_DEVICE_CAP_CARRIER_DETECT;
	else {
		nm_log_info (LOGD_HW,
		             "(%s): driver '%s' does not support carrier detection.",
		             nm_device_get_iface (dev),
		             nm_device_get_driver (dev));
		return NM_DEVICE_CAP_NONE;
	}
}

static gboolean
match_subchans (NMDeviceEthernet *self, NMSettingWired *s_wired, gboolean *try_mac)
{
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (self);
	const GPtrArray *subchans;
	int i;

	*try_mac = TRUE;

	subchans = nm_setting_wired_get_s390_subchannels (s_wired);
	if (!subchans)
		return TRUE;

	/* connection requires subchannels but the device has none */
	if (!priv->subchannels)
		return FALSE;

	/* Make sure each subchannel in the connection is a subchannel of this device */
	for (i = 0; i < subchans->len; i++) {
		const char *candidate = g_ptr_array_index (subchans, i);

		if (   (priv->subchan1 && !strcmp (priv->subchan1, candidate))
		    || (priv->subchan2 && !strcmp (priv->subchan2, candidate))
		    || (priv->subchan3 && !strcmp (priv->subchan3, candidate)))
			continue;

		return FALSE;  /* a subchannel was not found */
	}

	*try_mac = FALSE;
	return TRUE;
}

static gboolean
check_connection_compatible (NMDevice *device, NMConnection *connection)
{
	NMDeviceEthernet *self = NM_DEVICE_ETHERNET (device);
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (self);
	NMSettingWired *s_wired;

	if (!NM_DEVICE_CLASS (nm_device_ethernet_parent_class)->check_connection_compatible (device, connection))
		return FALSE;

	s_wired = nm_connection_get_setting_wired (connection);

	if (nm_connection_is_type (connection, NM_SETTING_PPPOE_SETTING_NAME)) {
		/* NOP */
	} else if (nm_connection_is_type (connection, NM_SETTING_WIRED_SETTING_NAME)) {
		if (!s_wired)
			return FALSE;
	} else
		return FALSE;

	if (s_wired) {
		const GByteArray *mac;
		gboolean try_mac = TRUE;
		const GSList *mac_blacklist, *mac_blacklist_iter;

		if (!match_subchans (self, s_wired, &try_mac))
			return FALSE;

		mac = nm_setting_wired_get_mac_address (s_wired);
		if (try_mac && mac && memcmp (mac->data, &priv->perm_hw_addr, ETH_ALEN))
			return FALSE;

		/* Check for MAC address blacklist */
		mac_blacklist = nm_setting_wired_get_mac_address_blacklist (s_wired);
		for (mac_blacklist_iter = mac_blacklist; mac_blacklist_iter;
			 mac_blacklist_iter = g_slist_next (mac_blacklist_iter)) {
			struct ether_addr addr;

			if (!ether_aton_r (mac_blacklist_iter->data, &addr)) {
				g_warn_if_reached ();
				return FALSE;
			}

			if (memcmp (&addr, &priv->perm_hw_addr, ETH_ALEN) == 0)
				return FALSE;
		}
	}

	return TRUE;
}

/* FIXME: Move it to nm-device.c and then get rid of all foo_device_get_setting() all around.
   It's here now to keep the patch short. */
static NMSetting *
device_get_setting (NMDevice *device, GType setting_type)
{
	NMActRequest *req;
	NMSetting *setting = NULL;

	req = nm_device_get_act_request (device);
	if (req) {
		NMConnection *connection;

		connection = nm_act_request_get_connection (req);
		if (connection)
			setting = nm_connection_get_setting (connection, setting_type);
	}

	return setting;
}

/*****************************************************************************/
/* 802.1X */

static void
remove_supplicant_timeouts (NMDeviceEthernet *self)
{
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (self);

	if (priv->supplicant.con_timeout_id) {
		g_source_remove (priv->supplicant.con_timeout_id);
		priv->supplicant.con_timeout_id = 0;
	}

	if (priv->supplicant_timeout_id) {
		g_source_remove (priv->supplicant_timeout_id);
		priv->supplicant_timeout_id = 0;
	}
}

static void
remove_supplicant_interface_error_handler (NMDeviceEthernet *self)
{
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (self);

	if (priv->supplicant.iface_error_id != 0) {
		g_signal_handler_disconnect (priv->supplicant.iface, priv->supplicant.iface_error_id);
		priv->supplicant.iface_error_id = 0;
	}

	if (priv->supplicant.iface_con_error_cb_id > 0) {
		g_source_remove (priv->supplicant.iface_con_error_cb_id);
		priv->supplicant.iface_con_error_cb_id = 0;
	}
}

static void
supplicant_interface_release (NMDeviceEthernet *self)
{
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (self);

	remove_supplicant_timeouts (self);
	remove_supplicant_interface_error_handler (self);

	if (priv->supplicant.iface_state_id > 0) {
		g_signal_handler_disconnect (priv->supplicant.iface, priv->supplicant.iface_state_id);
		priv->supplicant.iface_state_id = 0;
	}

	if (priv->supplicant.iface) {
		nm_supplicant_interface_disconnect (priv->supplicant.iface);
		nm_supplicant_manager_iface_release (priv->supplicant.mgr, priv->supplicant.iface);
		priv->supplicant.iface = NULL;
	}
}

static void
wired_secrets_cb (NMActRequest *req,
                  guint32 call_id,
                  NMConnection *connection,
                  GError *error,
                  gpointer user_data)
{
	NMDevice *dev = NM_DEVICE (user_data);

	g_return_if_fail (req == nm_device_get_act_request (dev));
	g_return_if_fail (nm_device_get_state (dev) == NM_DEVICE_STATE_NEED_AUTH);
	g_return_if_fail (nm_act_request_get_connection (req) == connection);

	if (error) {
		nm_log_warn (LOGD_ETHER, "%s", error->message);
		nm_device_state_changed (dev,
		                         NM_DEVICE_STATE_FAILED,
		                         NM_DEVICE_STATE_REASON_NO_SECRETS);
	} else
		nm_device_activate_schedule_stage1_device_prepare (dev);
}

static gboolean
link_timeout_cb (gpointer user_data)
{
	NMDeviceEthernet *self = NM_DEVICE_ETHERNET (user_data);
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (self);
	NMDevice *dev = NM_DEVICE (self);
	NMActRequest *req;
	NMConnection *connection;
	const char *setting_name;

	priv->supplicant_timeout_id = 0;

	req = nm_device_get_act_request (dev);

	if (nm_device_get_state (dev) == NM_DEVICE_STATE_ACTIVATED) {
		nm_device_state_changed (dev,
		                         NM_DEVICE_STATE_FAILED,
		                         NM_DEVICE_STATE_REASON_SUPPLICANT_TIMEOUT);
		return FALSE;
	}

	/* Disconnect event during initial authentication and credentials
	 * ARE checked - we are likely to have wrong key.  Ask the user for
	 * another one.
	 */
	if (nm_device_get_state (dev) != NM_DEVICE_STATE_CONFIG)
		goto time_out;

	connection = nm_act_request_get_connection (req);
	nm_connection_clear_secrets (connection);
	setting_name = nm_connection_need_secrets (connection, NULL);
	if (!setting_name)
		goto time_out;

	nm_log_info (LOGD_DEVICE | LOGD_ETHER,
	             "Activation (%s/wired): disconnected during authentication,"
	             " asking for new key.",
	             nm_device_get_iface (dev));
	supplicant_interface_release (self);

	nm_device_state_changed (dev, NM_DEVICE_STATE_NEED_AUTH, NM_DEVICE_STATE_REASON_SUPPLICANT_DISCONNECT);
	nm_act_request_get_secrets (req,
	                            setting_name,
	                            NM_SETTINGS_GET_SECRETS_FLAG_REQUEST_NEW,
	                            NULL,
	                            wired_secrets_cb,
	                            self);

	return FALSE;

time_out:
	nm_log_warn (LOGD_DEVICE | LOGD_ETHER,
	             "(%s): link timed out.", nm_device_get_iface (dev));
	nm_device_state_changed (dev, NM_DEVICE_STATE_FAILED, NM_DEVICE_STATE_REASON_SUPPLICANT_DISCONNECT);

	return FALSE;
}

static NMSupplicantConfig *
build_supplicant_config (NMDeviceEthernet *self)
{
	const char *con_uuid;
	NMSupplicantConfig *config = NULL;
	NMSetting8021x *security;
	NMConnection *connection;

	connection = nm_device_get_connection (NM_DEVICE (self));
	g_assert (connection);
	con_uuid = nm_connection_get_uuid (connection);

	config = nm_supplicant_config_new ();

	security = nm_connection_get_setting_802_1x (connection);
	if (!nm_supplicant_config_add_setting_8021x (config, security, con_uuid, TRUE)) {
		nm_log_warn (LOGD_DEVICE, "Couldn't add 802.1X security setting to supplicant config.");
		g_object_unref (config);
		config = NULL;
	}

	return config;
}

static void
supplicant_iface_state_cb (NMSupplicantInterface *iface,
                           guint32 new_state,
                           guint32 old_state,
                           int disconnect_reason,
                           gpointer user_data)
{
	NMDeviceEthernet *self = NM_DEVICE_ETHERNET (user_data);
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (self);
	NMDevice *device = NM_DEVICE (self);
	NMSupplicantConfig *config;
	gboolean success = FALSE;
	NMDeviceState devstate;

	if (new_state == old_state)
		return;

	nm_log_info (LOGD_DEVICE | LOGD_ETHER,
	             "(%s): supplicant interface state: %s -> %s",
	             nm_device_get_iface (device),
	             nm_supplicant_interface_state_to_string (old_state),
	             nm_supplicant_interface_state_to_string (new_state));

	devstate = nm_device_get_state (device);

	switch (new_state) {
	case NM_SUPPLICANT_INTERFACE_STATE_READY:
		config = build_supplicant_config (self);
		if (config) {
			success = nm_supplicant_interface_set_config (priv->supplicant.iface, config);
			g_object_unref (config);

			if (!success) {
				nm_log_err (LOGD_DEVICE | LOGD_ETHER,
				            "Activation (%s/wired): couldn't send security "
						    "configuration to the supplicant.",
						    nm_device_get_iface (device));
			}
		} else {
			nm_log_warn (LOGD_DEVICE | LOGD_ETHER,
			             "Activation (%s/wired): couldn't build security configuration.",
			             nm_device_get_iface (device));
		}

		if (!success) {
			nm_device_state_changed (device,
			                         NM_DEVICE_STATE_FAILED,
			                         NM_DEVICE_STATE_REASON_SUPPLICANT_CONFIG_FAILED);
		}
		break;
	case NM_SUPPLICANT_INTERFACE_STATE_COMPLETED:
		remove_supplicant_interface_error_handler (self);
		remove_supplicant_timeouts (self);

		/* If this is the initial association during device activation,
		 * schedule the next activation stage.
		 */
		if (devstate == NM_DEVICE_STATE_CONFIG) {
			nm_log_info (LOGD_DEVICE | LOGD_ETHER,
			             "Activation (%s/wired) Stage 2 of 5 (Device Configure) successful.",
				         nm_device_get_iface (device));
			nm_device_activate_schedule_stage3_ip_config_start (device);
		}
		break;
	case NM_SUPPLICANT_INTERFACE_STATE_DISCONNECTED:
		if ((devstate == NM_DEVICE_STATE_ACTIVATED) || nm_device_is_activating (device)) {
			/* Start the link timeout so we allow some time for reauthentication */
			if (!priv->supplicant_timeout_id)
				priv->supplicant_timeout_id = g_timeout_add_seconds (15, link_timeout_cb, device);
		}
		break;
	case NM_SUPPLICANT_INTERFACE_STATE_DOWN:
		supplicant_interface_release (self);
		remove_supplicant_timeouts (self);

		if ((devstate == NM_DEVICE_STATE_ACTIVATED) || nm_device_is_activating (device)) {
			nm_device_state_changed (device,
			                         NM_DEVICE_STATE_FAILED,
			                         NM_DEVICE_STATE_REASON_SUPPLICANT_FAILED);
		}
		break;
	default:
		break;
	}
}

static gboolean
supplicant_iface_connection_error_cb_handler (gpointer user_data)
{
	NMDeviceEthernet *self = NM_DEVICE_ETHERNET (user_data);
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (self);

	supplicant_interface_release (self);
	nm_device_state_changed (NM_DEVICE (self), NM_DEVICE_STATE_FAILED, NM_DEVICE_STATE_REASON_SUPPLICANT_CONFIG_FAILED);

	priv->supplicant.iface_con_error_cb_id = 0;
	return FALSE;
}

static void
supplicant_iface_connection_error_cb (NMSupplicantInterface *iface,
                                      const char *name,
                                      const char *message,
                                      gpointer user_data)
{
	NMDeviceEthernet *self = NM_DEVICE_ETHERNET (user_data);
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (self);
	guint id;

	nm_log_warn (LOGD_DEVICE | LOGD_ETHER,
	             "Activation (%s/wired): association request to the supplicant failed: %s - %s",
	             nm_device_get_iface (NM_DEVICE (self)), name, message);

	if (priv->supplicant.iface_con_error_cb_id)
		g_source_remove (priv->supplicant.iface_con_error_cb_id);

	id = g_idle_add (supplicant_iface_connection_error_cb_handler, self);
	priv->supplicant.iface_con_error_cb_id = id;
}

static NMActStageReturn
handle_auth_or_fail (NMDeviceEthernet *self,
                     NMActRequest *req,
                     gboolean new_secrets)
{
	const char *setting_name;
	guint32 tries;
	NMConnection *connection;

	connection = nm_act_request_get_connection (req);
	g_assert (connection);

	tries = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (connection), WIRED_SECRETS_TRIES));
	if (tries > 3)
		return NM_ACT_STAGE_RETURN_FAILURE;

	nm_device_state_changed (NM_DEVICE (self), NM_DEVICE_STATE_NEED_AUTH, NM_DEVICE_STATE_REASON_NONE);

	nm_connection_clear_secrets (connection);
	setting_name = nm_connection_need_secrets (connection, NULL);
	if (setting_name) {
		NMSettingsGetSecretsFlags flags = NM_SETTINGS_GET_SECRETS_FLAG_ALLOW_INTERACTION;

		if (new_secrets)
			flags |= NM_SETTINGS_GET_SECRETS_FLAG_REQUEST_NEW;
		nm_act_request_get_secrets (req, setting_name, flags, NULL, wired_secrets_cb, self);

		g_object_set_data (G_OBJECT (connection), WIRED_SECRETS_TRIES, GUINT_TO_POINTER (++tries));
	} else {
		nm_log_info (LOGD_DEVICE, "Cleared secrets, but setting didn't need any secrets.");
	}

	return NM_ACT_STAGE_RETURN_POSTPONE;
}

static gboolean
supplicant_connection_timeout_cb (gpointer user_data)
{
	NMDeviceEthernet *self = NM_DEVICE_ETHERNET (user_data);
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (self);
	NMDevice *device = NM_DEVICE (self);
	NMActRequest *req;
	NMConnection *connection;
	const char *iface;
	guint64 timestamp = 0;
	gboolean new_secrets = TRUE;

	priv->supplicant.con_timeout_id = 0;

	iface = nm_device_get_iface (device);

	/* Authentication failed; either driver problems, the encryption key is
	 * wrong, the passwords or certificates were wrong or the Ethernet switch's
	 * port is not configured for 802.1x. */
	nm_log_warn (LOGD_DEVICE | LOGD_ETHER,
	             "Activation (%s/wired): association took too long.", iface);

	supplicant_interface_release (self);
	req = nm_device_get_act_request (device);
	g_assert (req);

	connection = nm_act_request_get_connection (req);
	g_assert (connection);

	/* Ask for new secrets only if we've never activated this connection
	 * before.  If we've connected before, don't bother the user with dialogs,
	 * just retry or fail, and if we never connect the user can fix the
	 * password somewhere else. */
	if (nm_settings_connection_get_timestamp (NM_SETTINGS_CONNECTION (connection), &timestamp))
		new_secrets = !timestamp;

	if (handle_auth_or_fail (self, req, new_secrets) == NM_ACT_STAGE_RETURN_POSTPONE) {
		nm_log_info (LOGD_DEVICE | LOGD_ETHER,
		             "Activation (%s/wired): asking for new secrets", iface);
	} else
		nm_device_state_changed (device, NM_DEVICE_STATE_FAILED, NM_DEVICE_STATE_REASON_NO_SECRETS);

	return FALSE;
}

static gboolean
supplicant_interface_init (NMDeviceEthernet *self)
{
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (self);
	const char *iface;

	iface = nm_device_get_iface (NM_DEVICE (self));

	/* Create supplicant interface */
	priv->supplicant.iface = nm_supplicant_manager_iface_get (priv->supplicant.mgr, iface, FALSE);
	if (!priv->supplicant.iface) {
		nm_log_err (LOGD_DEVICE | LOGD_ETHER,
		            "Couldn't initialize supplicant interface for %s.",
		            iface);
		supplicant_interface_release (self);
		return FALSE;
	}

	/* Listen for it's state signals */
	priv->supplicant.iface_state_id = g_signal_connect (priv->supplicant.iface,
	                                                    NM_SUPPLICANT_INTERFACE_STATE,
	                                                    G_CALLBACK (supplicant_iface_state_cb),
	                                                    self);

	/* Hook up error signal handler to capture association errors */
	priv->supplicant.iface_error_id = g_signal_connect (priv->supplicant.iface,
	                                                    "connection-error",
	                                                    G_CALLBACK (supplicant_iface_connection_error_cb),
	                                                    self);

	/* Set up a timeout on the connection attempt to fail it after 25 seconds */
	priv->supplicant.con_timeout_id = g_timeout_add_seconds (25, supplicant_connection_timeout_cb, self);

	return TRUE;
}

static gboolean
pppoe_reconnect_delay (gpointer user_data)
{
	NMDevice *device = NM_DEVICE (user_data);
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (device);

	priv->pppoe_wait_id = 0;
	nm_log_info (LOGD_DEVICE, "(%s) PPPoE reconnect delay complete, resuming connection...",
	             nm_device_get_iface (device));
	nm_device_activate_schedule_stage2_device_config (device);
	return FALSE;
}

static NMActStageReturn
act_stage1_prepare (NMDevice *dev, NMDeviceStateReason *reason)
{
	NMDeviceEthernet *self = NM_DEVICE_ETHERNET (dev);
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (self);
	NMActRequest *req;
	NMSettingWired *s_wired;
	const GByteArray *cloned_mac;
	NMActStageReturn ret = NM_ACT_STAGE_RETURN_SUCCESS;

	g_return_val_if_fail (reason != NULL, NM_ACT_STAGE_RETURN_FAILURE);

	ret = NM_DEVICE_CLASS (nm_device_ethernet_parent_class)->act_stage1_prepare (dev, reason);
	if (ret == NM_ACT_STAGE_RETURN_SUCCESS) {
		req = nm_device_get_act_request (NM_DEVICE (self));
		g_return_val_if_fail (req != NULL, NM_ACT_STAGE_RETURN_FAILURE);

		s_wired = (NMSettingWired *) device_get_setting (dev, NM_TYPE_SETTING_WIRED);
		if (s_wired) {
			/* Set device MAC address if the connection wants to change it */
			cloned_mac = nm_setting_wired_get_cloned_mac_address (s_wired);
			if (cloned_mac && (cloned_mac->len == ETH_ALEN))
				nm_device_set_hw_addr (dev, cloned_mac->data, "set", LOGD_ETHER);
		}

		/* If we're re-activating a PPPoE connection a short while after
		 * a previous PPPoE connection was torn down, wait a bit to allow the
		 * remote side to handle the disconnection.  Otherwise the peer may
		 * get confused and fail to negotiate the new connection. (rh #1023503)
		 */
		if (priv->last_pppoe_time) {
			gint32 delay = nm_utils_get_monotonic_timestamp_s () - priv->last_pppoe_time;

			if (delay < PPPOE_RECONNECT_DELAY && device_get_setting (dev, NM_TYPE_SETTING_PPPOE)) {
				nm_log_info (LOGD_DEVICE, "(%s) delaying PPPoE reconnect for %d seconds to ensure peer is ready...",
				             nm_device_get_iface (dev), delay);
				g_assert (!priv->pppoe_wait_id);
				priv->pppoe_wait_id = g_timeout_add_seconds (delay,
				                                             pppoe_reconnect_delay,
				                                             self);
				ret = NM_ACT_STAGE_RETURN_POSTPONE;
			} else
				priv->last_pppoe_time = 0;
		}
	}

	return ret;
}

static NMActStageReturn
nm_8021x_stage2_config (NMDeviceEthernet *self, NMDeviceStateReason *reason)
{
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (self);
	NMConnection *connection;
	NMSetting8021x *security;
	const char *setting_name;
	const char *iface;
	NMActStageReturn ret = NM_ACT_STAGE_RETURN_FAILURE;

	connection = nm_device_get_connection (NM_DEVICE (self));
	g_assert (connection);
	security = nm_connection_get_setting_802_1x (connection);
	if (!security) {
		nm_log_err (LOGD_DEVICE, "Invalid or missing 802.1X security");
		*reason = NM_DEVICE_STATE_REASON_CONFIG_FAILED;
		return ret;
	}

	if (!priv->supplicant.mgr)
		priv->supplicant.mgr = nm_supplicant_manager_get ();

	iface = nm_device_get_iface (NM_DEVICE (self));

	/* If we need secrets, get them */
	setting_name = nm_connection_need_secrets (connection, NULL);
	if (setting_name) {
		NMActRequest *req = nm_device_get_act_request (NM_DEVICE (self));

		nm_log_info (LOGD_DEVICE | LOGD_ETHER,
		             "Activation (%s/wired): connection '%s' has security, but secrets are required.",
				     iface, nm_connection_get_id (connection));

		ret = handle_auth_or_fail (self, req, FALSE);
		if (ret != NM_ACT_STAGE_RETURN_POSTPONE)
			*reason = NM_DEVICE_STATE_REASON_NO_SECRETS;
	} else {
		nm_log_info (LOGD_DEVICE | LOGD_ETHER,
		             "Activation (%s/wired): connection '%s' requires no security. No secrets needed.",
				     iface, nm_connection_get_id (connection));

		if (supplicant_interface_init (self))
			ret = NM_ACT_STAGE_RETURN_POSTPONE;
		else
			*reason = NM_DEVICE_STATE_REASON_CONFIG_FAILED;
	}

	return ret;
}

/*****************************************************************************/
/* PPPoE */

static void
ppp_state_changed (NMPPPManager *ppp_manager, NMPPPStatus status, gpointer user_data)
{
	NMDevice *device = NM_DEVICE (user_data);

	switch (status) {
	case NM_PPP_STATUS_DISCONNECT:
		nm_device_state_changed (device, NM_DEVICE_STATE_FAILED, NM_DEVICE_STATE_REASON_PPP_DISCONNECT);
		break;
	case NM_PPP_STATUS_DEAD:
		nm_device_state_changed (device, NM_DEVICE_STATE_FAILED, NM_DEVICE_STATE_REASON_PPP_FAILED);
		break;
	default:
		break;
	}
}

static void
ppp_ip4_config (NMPPPManager *ppp_manager,
                const char *iface,
                NMIP4Config *config,
                gpointer user_data)
{
	NMDevice *device = NM_DEVICE (user_data);

	/* Ignore PPP IP4 events that come in after initial configuration */
	if (nm_device_activate_ip4_state_in_conf (device)) {
		nm_device_set_ip_iface (device, iface);
		nm_device_activate_schedule_ip4_config_result (device, config);
	}
}

static NMActStageReturn
pppoe_stage3_ip4_config_start (NMDeviceEthernet *self, NMDeviceStateReason *reason)
{
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (self);
	NMConnection *connection;
	NMSettingPPPOE *s_pppoe;
	NMActRequest *req;
	GError *err = NULL;
	NMActStageReturn ret = NM_ACT_STAGE_RETURN_FAILURE;

	req = nm_device_get_act_request (NM_DEVICE (self));
	g_assert (req);

	connection = nm_act_request_get_connection (req);
	g_assert (req);

	s_pppoe = nm_connection_get_setting_pppoe (connection);
	g_assert (s_pppoe);

	priv->ppp_manager = nm_ppp_manager_new (nm_device_get_iface (NM_DEVICE (self)));
	if (nm_ppp_manager_start (priv->ppp_manager, req, nm_setting_pppoe_get_username (s_pppoe), 30, &err)) {
		g_signal_connect (priv->ppp_manager, "state-changed",
					   G_CALLBACK (ppp_state_changed),
					   self);
		g_signal_connect (priv->ppp_manager, "ip4-config",
					   G_CALLBACK (ppp_ip4_config),
					   self);
		ret = NM_ACT_STAGE_RETURN_POSTPONE;
	} else {
		nm_log_warn (LOGD_DEVICE, "(%s): PPPoE failed to start: %s",
		             nm_device_get_iface (NM_DEVICE (self)), err->message);
		g_error_free (err);

		g_object_unref (priv->ppp_manager);
		priv->ppp_manager = NULL;

		*reason = NM_DEVICE_STATE_REASON_PPP_START_FAILED;
	}

	return ret;
}

/****************************************************************/

static void
dcb_timeout_cleanup (NMDevice *device)
{
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (device);

	if (priv->dcb_timeout_id) {
		g_source_remove (priv->dcb_timeout_id);
		priv->dcb_timeout_id = 0;
	}
}

static void
dcb_carrier_cleanup (NMDevice *device)
{
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (device);

	if (priv->dcb_carrier_id) {
		g_signal_handler_disconnect (device, priv->dcb_carrier_id);
		priv->dcb_carrier_id = 0;
	}
}

static void dcb_state (NMDevice *device, gboolean timeout);

static gboolean
dcb_carrier_timeout (gpointer user_data)
{
	NMDevice *device = NM_DEVICE (user_data);
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (device);

	g_return_val_if_fail (nm_device_get_state (device) == NM_DEVICE_STATE_CONFIG, G_SOURCE_REMOVE);

	priv->dcb_timeout_id = 0;
	if (priv->dcb_wait != DCB_WAIT_CARRIER_POSTCONFIG_DOWN) {
		nm_log_warn (LOGD_DCB,
		             "(%s): DCB: timed out waiting for carrier (step %d)",
		             nm_device_get_iface (device),
		             priv->dcb_wait);
	}
	dcb_state (device, TRUE);
	return G_SOURCE_REMOVE;
}

static gboolean
dcb_configure (NMDevice *device)
{
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (device);
	NMSettingDcb *s_dcb;
	const char *iface = nm_device_get_iface (device);
	GError *error = NULL;

	dcb_timeout_cleanup (device);

	s_dcb = (NMSettingDcb *) device_get_setting (device, NM_TYPE_SETTING_DCB);
	g_assert (s_dcb);
	if (!nm_dcb_setup (iface, s_dcb, &error)) {
		nm_log_warn (LOGD_DCB,
		             "Activation (%s/wired) failed to enable DCB/FCoE: %s",
		             iface, error->message);
		g_clear_error (&error);
		return FALSE;
	}

	/* Pause again just in case the device takes the carrier down when
	 * setting specific DCB attributes.
	 */
	nm_log_dbg (LOGD_DCB, "(%s): waiting for carrier (postconfig down)", iface);
	priv->dcb_wait = DCB_WAIT_CARRIER_POSTCONFIG_DOWN;
	priv->dcb_timeout_id = g_timeout_add_seconds (3, dcb_carrier_timeout, device);
	return TRUE;
}

static gboolean
dcb_enable (NMDevice *device)
{
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (device);
	const char *iface = nm_device_get_iface (device);
	GError *error = NULL;

	dcb_timeout_cleanup (device);
	if (!nm_dcb_enable (iface, TRUE, &error)) {
		nm_log_warn (LOGD_DCB,
		             "Activation (%s/wired) failed to enable DCB/FCoE: %s",
		             iface, error->message);
		g_clear_error (&error);
		return FALSE;
	}

	/* Pause for 3 seconds after enabling DCB to let the card reconfigure
	 * itself.  Drivers will often re-initialize internal settings which
	 * takes the carrier down for 2 or more seconds.  During this time,
	 * lldpad will refuse to do anything else with the card since the carrier
	 * is down.  But NM might get the carrier-down signal long after calling
	 * "dcbtool dcb on", so we have to first wait for the carrier to go down.
	 */
	nm_log_dbg (LOGD_DCB, "(%s): waiting for carrier (preconfig down)", iface);
	priv->dcb_wait = DCB_WAIT_CARRIER_PRECONFIG_DOWN;
	priv->dcb_timeout_id = g_timeout_add_seconds (3, dcb_carrier_timeout, device);
	return TRUE;
}

static void
dcb_state (NMDevice *device, gboolean timeout)
{
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (device);
	const char *iface = nm_device_get_iface (device);
	gboolean carrier;

	g_return_if_fail (nm_device_get_state (device) == NM_DEVICE_STATE_CONFIG);


	carrier = nm_platform_link_is_connected (nm_device_get_ifindex (device));
	nm_log_dbg (LOGD_DCB, "(%s): dcb_state() wait %d carrier %d timeout %d", iface, priv->dcb_wait, carrier, timeout);

	switch (priv->dcb_wait) {
	case DCB_WAIT_CARRIER_PREENABLE_UP:
		if (timeout || carrier) {
			nm_log_dbg (LOGD_DCB, "(%s): dcb_state() enabling DCB", iface);
			dcb_timeout_cleanup (device);
			if (!dcb_enable (device)) {
				dcb_carrier_cleanup (device);
				nm_device_state_changed (device,
				                         NM_ACT_STAGE_RETURN_FAILURE,
				                         NM_DEVICE_STATE_REASON_DCB_FCOE_FAILED);
			}
		}
		break;
	case DCB_WAIT_CARRIER_PRECONFIG_DOWN:
		dcb_timeout_cleanup (device);
		priv->dcb_wait = DCB_WAIT_CARRIER_PRECONFIG_UP;

		if (!carrier) {
			/* Wait for the carrier to come back up */
			nm_log_dbg (LOGD_DCB, "(%s): waiting for carrier (preconfig up)", iface);
			priv->dcb_timeout_id = g_timeout_add_seconds (5, dcb_carrier_timeout, device);
			break;
		}
		nm_log_dbg (LOGD_DCB, "(%s): dcb_state() preconfig down falling through", iface);
		/* carrier never went down? fall through */
	case DCB_WAIT_CARRIER_PRECONFIG_UP:
		if (timeout || carrier) {
			nm_log_dbg (LOGD_DCB, "(%s): dcb_state() preconfig up configuring DCB", iface);
			dcb_timeout_cleanup (device);
			if (!dcb_configure (device)) {
				dcb_carrier_cleanup (device);
				nm_device_state_changed (device,
				                         NM_ACT_STAGE_RETURN_FAILURE,
				                         NM_DEVICE_STATE_REASON_DCB_FCOE_FAILED);
			}
		}
		break;
	case DCB_WAIT_CARRIER_POSTCONFIG_DOWN:
		dcb_timeout_cleanup (device);
		priv->dcb_wait = DCB_WAIT_CARRIER_POSTCONFIG_UP;

		if (!carrier) {
			/* Wait for the carrier to come back up */
			nm_log_dbg (LOGD_DCB, "(%s): waiting for carrier (postconfig up)", iface);
			priv->dcb_timeout_id = g_timeout_add_seconds (5, dcb_carrier_timeout, device);
			break;
		}
		nm_log_dbg (LOGD_DCB, "(%s): dcb_state() postconfig down falling through", iface);
		/* carrier never went down? fall through */
	case DCB_WAIT_CARRIER_POSTCONFIG_UP:
		if (timeout || carrier) {
			nm_log_dbg (LOGD_DCB, "(%s): dcb_state() postconfig up starting IP", iface);
			dcb_timeout_cleanup (device);
			dcb_carrier_cleanup (device);
			priv->dcb_wait = DCB_WAIT_UNKNOWN;
			nm_device_activate_schedule_stage3_ip_config_start (device);
		}
		break;
	default:
		g_assert_not_reached ();
	}
}

static void
dcb_carrier_changed (NMDevice *device, GParamSpec *pspec, gpointer unused)
{
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (device);

	g_return_if_fail (nm_device_get_state (device) == NM_DEVICE_STATE_CONFIG);

	if (priv->dcb_timeout_id) {
		nm_log_dbg (LOGD_DCB, "(%s): carrier_changed() calling dcb_state()", nm_device_get_iface (device));
		dcb_state (device, FALSE);
	}
}

/****************************************************************/

static NMActStageReturn
act_stage2_config (NMDevice *device, NMDeviceStateReason *reason)
{
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (device);
	NMSettingConnection *s_con;
	const char *connection_type;
	NMActStageReturn ret = NM_ACT_STAGE_RETURN_SUCCESS;
	NMSettingDcb *s_dcb;

	g_return_val_if_fail (reason != NULL, NM_ACT_STAGE_RETURN_FAILURE);

	s_con = NM_SETTING_CONNECTION (device_get_setting (device, NM_TYPE_SETTING_CONNECTION));
	g_assert (s_con);

	dcb_timeout_cleanup (device);
	dcb_carrier_cleanup (device);

	/* 802.1x has to run before any IP configuration since the 802.1x auth
	 * process opens the port up for normal traffic.
	 */
	connection_type = nm_setting_connection_get_connection_type (s_con);
	if (!strcmp (connection_type, NM_SETTING_WIRED_SETTING_NAME)) {
		NMSetting8021x *security;

		security = (NMSetting8021x *) device_get_setting (device, NM_TYPE_SETTING_802_1X);
		if (security) {
			/* FIXME: for now 802.1x is mutually exclusive with DCB */
			return nm_8021x_stage2_config (NM_DEVICE_ETHERNET (device), reason);
		}
	}

	/* DCB and FCoE setup */
	s_dcb = (NMSettingDcb *) device_get_setting (device, NM_TYPE_SETTING_DCB);
	if (s_dcb) {
		/* lldpad really really wants the carrier to be up */
		if (nm_platform_link_is_connected (nm_device_get_ifindex (device))) {
			if (!dcb_enable (device)) {
				*reason = NM_DEVICE_STATE_REASON_DCB_FCOE_FAILED;
				return NM_ACT_STAGE_RETURN_FAILURE;
			}
		} else {
			nm_log_dbg (LOGD_DCB, "(%s): waiting for carrier (preenable up)",
			            nm_device_get_iface (device));
			priv->dcb_wait = DCB_WAIT_CARRIER_PREENABLE_UP;
			priv->dcb_timeout_id = g_timeout_add_seconds (4, dcb_carrier_timeout, device);
		}

		/* Watch carrier independently of NMDeviceClass::carrier_changed so
		 * we get instant notifications of disconnection that aren't deferred.
		 */
		priv->dcb_carrier_id = g_signal_connect (device,
		                                         "notify::" NM_DEVICE_CARRIER,
		                                         G_CALLBACK (dcb_carrier_changed),
		                                         NULL);
		ret = NM_ACT_STAGE_RETURN_POSTPONE;
	}

	return ret;
}

static NMActStageReturn
act_stage3_ip4_config_start (NMDevice *device,
                             NMIP4Config **out_config,
                             NMDeviceStateReason *reason)
{
	NMSettingConnection *s_con;
	const char *connection_type;

	g_return_val_if_fail (reason != NULL, NM_ACT_STAGE_RETURN_FAILURE);

	s_con = NM_SETTING_CONNECTION (device_get_setting (device, NM_TYPE_SETTING_CONNECTION));
	g_assert (s_con);

	connection_type = nm_setting_connection_get_connection_type (s_con);
	if (!strcmp (connection_type, NM_SETTING_PPPOE_SETTING_NAME))
		return pppoe_stage3_ip4_config_start (NM_DEVICE_ETHERNET (device), reason);

	return NM_DEVICE_CLASS (nm_device_ethernet_parent_class)->act_stage3_ip4_config_start (device, out_config, reason);
}

static void
ip4_config_pre_commit (NMDevice *device, NMIP4Config *config)
{
	NMConnection *connection;
	NMSettingWired *s_wired;
	guint32 mtu;

	/* MTU only set for plain ethernet */
	if (NM_DEVICE_ETHERNET_GET_PRIVATE (device)->ppp_manager)
		return;

	connection = nm_device_get_connection (device);
	g_assert (connection);
	s_wired = nm_connection_get_setting_wired (connection);
	g_assert (s_wired);

	/* MTU override */
	mtu = nm_setting_wired_get_mtu (s_wired);
	if (mtu)
		nm_ip4_config_set_mtu (config, mtu);
}

static void
deactivate (NMDevice *device)
{
	NMDeviceEthernet *self = NM_DEVICE_ETHERNET (device);
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (self);
	NMSettingDcb *s_dcb;
	GError *error = NULL;

	/* Clear wired secrets tries when deactivating */
	clear_secrets_tries (device);

	if (priv->pppoe_wait_id) {
		g_source_remove (priv->pppoe_wait_id);
		priv->pppoe_wait_id = 0;
	}

	if (priv->pending_ip4_config) {
		g_object_unref (priv->pending_ip4_config);
		priv->pending_ip4_config = NULL;
	}

	if (priv->ppp_manager) {
		g_object_unref (priv->ppp_manager);
		priv->ppp_manager = NULL;
	}

	supplicant_interface_release (self);

	priv->dcb_wait = DCB_WAIT_UNKNOWN;
	dcb_timeout_cleanup (device);
	dcb_carrier_cleanup (device);

	/* Tear down DCB/FCoE if it was enabled */
	s_dcb = (NMSettingDcb *) device_get_setting (device, NM_TYPE_SETTING_DCB);
	if (s_dcb) {
		if (!nm_dcb_cleanup (nm_device_get_iface (device), &error)) {
			nm_log_warn (LOGD_DEVICE | LOGD_HW,
			             "(%s) failed to disable DCB/FCoE: %s",
			             nm_device_get_iface (device), error->message);
			g_clear_error (&error);
		}
	}

	/* Set last PPPoE connection time */
	if (device_get_setting (device, NM_TYPE_SETTING_PPPOE))
		NM_DEVICE_ETHERNET_GET_PRIVATE (device)->last_pppoe_time = nm_utils_get_monotonic_timestamp_s ();

	/* Reset MAC address back to initial address */
	nm_device_set_hw_addr (device, priv->initial_hw_addr, "reset", LOGD_ETHER);
}

static gboolean
complete_connection (NMDevice *device,
                     NMConnection *connection,
                     const char *specific_object,
                     const GSList *existing_connections,
                     GError **error)
{
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (device);
	NMSettingWired *s_wired;
	NMSettingPPPOE *s_pppoe;
	const GByteArray *setting_mac;

	s_pppoe = nm_connection_get_setting_pppoe (connection);

	/* We can't telepathically figure out the service name or username, so if
	 * those weren't given, we can't complete the connection.
	 */
	if (s_pppoe && !nm_setting_verify (NM_SETTING (s_pppoe), NULL, error))
		return FALSE;

	/* Default to an ethernet-only connection, but if a PPPoE setting was given
	 * then PPPoE should be our connection type.
	 */
	nm_utils_complete_generic (connection,
	                           s_pppoe ? NM_SETTING_PPPOE_SETTING_NAME : NM_SETTING_WIRED_SETTING_NAME,
	                           existing_connections,
	                           s_pppoe ? _("PPPoE connection %d") : _("Wired connection %d"),
	                           NULL,
	                           s_pppoe ? FALSE : TRUE); /* No IPv6 by default yet for PPPoE */

	s_wired = nm_connection_get_setting_wired (connection);
	if (!s_wired) {
		s_wired = (NMSettingWired *) nm_setting_wired_new ();
		nm_connection_add_setting (connection, NM_SETTING (s_wired));
	}

	setting_mac = nm_setting_wired_get_mac_address (s_wired);
	if (setting_mac) {
		/* Make sure the setting MAC (if any) matches the device's permanent MAC */
		if (memcmp (setting_mac->data, priv->perm_hw_addr, ETH_ALEN)) {
			g_set_error_literal (error,
			                     NM_SETTING_WIRED_ERROR,
			                     NM_SETTING_WIRED_ERROR_INVALID_PROPERTY,
			                     NM_SETTING_WIRED_MAC_ADDRESS);
			return FALSE;
		}
	} else {
		GByteArray *mac;
		const guint8 null_mac[ETH_ALEN] = { 0, 0, 0, 0, 0, 0 };

		/* Lock the connection to this device by default */
		if (memcmp (priv->perm_hw_addr, null_mac, ETH_ALEN)) {
			mac = g_byte_array_sized_new (ETH_ALEN);
			g_byte_array_append (mac, priv->perm_hw_addr, ETH_ALEN);
			g_object_set (G_OBJECT (s_wired), NM_SETTING_WIRED_MAC_ADDRESS, mac, NULL);
			g_byte_array_free (mac, TRUE);
		}
	}

	return TRUE;
}

static gboolean
spec_match_list (NMDevice *device, const GSList *specs)
{
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (device);

	if (priv->subchannels && nm_match_spec_s390_subchannels (specs, priv->subchannels))
		return TRUE;

	return NM_DEVICE_CLASS (nm_device_ethernet_parent_class)->spec_match_list (device, specs);
}

static void
update_connection (NMDevice *device, NMConnection *connection)
{
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (device);
	NMSettingWired *s_wired = nm_connection_get_setting_wired (connection);
	guint maclen;
	const guint8 *mac = nm_device_get_hw_address (device, &maclen);
	static const guint8 null_mac[ETH_ALEN] = { 0, 0, 0, 0, 0, 0 };
	const char *mac_prop = NM_SETTING_WIRED_MAC_ADDRESS;
	GByteArray *array;
	GHashTableIter iter;
	gpointer key, value;

	if (!s_wired) {
		s_wired = (NMSettingWired *) nm_setting_wired_new ();
		nm_connection_add_setting (connection, (NMSetting *) s_wired);
	}

	/* If the device reports a permanent address, use that for the MAC address
	 * and the current MAC, if different, is the cloned MAC.
	 */
	if (memcmp (priv->perm_hw_addr, null_mac, ETH_ALEN)) {
		array = g_byte_array_sized_new (ETH_ALEN);
		g_byte_array_append (array, priv->perm_hw_addr, ETH_ALEN);
		g_object_set (s_wired, NM_SETTING_WIRED_MAC_ADDRESS, array, NULL);
		g_byte_array_unref (array);

		mac_prop = NULL;
		if (mac && memcmp (priv->perm_hw_addr, mac, ETH_ALEN))
			mac_prop = NM_SETTING_WIRED_CLONED_MAC_ADDRESS;
	}

	if (mac_prop && mac && maclen == ETH_ALEN) {
		array = g_byte_array_sized_new (ETH_ALEN);
		g_byte_array_append (array, (guint8 *) mac, maclen);
		g_object_set (s_wired, mac_prop, array, NULL);
		g_byte_array_unref (array);
	}

	/* We don't set the MTU as we don't know whether it was set explicitly */

	/* s390 */
	if (priv->subchannels) {
		GPtrArray *subchan_arr = g_ptr_array_sized_new (3);
		if (priv->subchan1)
			 g_ptr_array_add (subchan_arr, priv->subchan1);
		if (priv->subchan2)
			 g_ptr_array_add (subchan_arr, priv->subchan2);
		if (priv->subchan3)
			 g_ptr_array_add (subchan_arr, priv->subchan3);
		g_object_set (s_wired, NM_SETTING_WIRED_S390_SUBCHANNELS, subchan_arr, NULL);
		g_ptr_array_free (subchan_arr, TRUE);
	}
	if (priv->s390_nettype)
		g_object_set (s_wired, NM_SETTING_WIRED_S390_NETTYPE, priv->s390_nettype, NULL);
	g_hash_table_iter_init (&iter, priv->s390_options);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		nm_setting_wired_add_s390_option (s_wired, (const char *) key, (const char *) value);
	}

}

static void
get_link_speed (NMDevice *device)
{
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (device);
	struct ifreq ifr;
	struct ethtool_cmd edata = {
		.cmd = ETHTOOL_GSET,
	};
	guint32 speed;
	int fd;

	fd = socket (PF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		nm_log_warn (LOGD_HW | LOGD_ETHER, "couldn't open ethtool control socket.");
		return;
	}

	memset (&ifr, 0, sizeof (struct ifreq));
	strncpy (ifr.ifr_name, nm_device_get_iface (device), IFNAMSIZ);
	ifr.ifr_data = (char *) &edata;

	if (ioctl (fd, SIOCETHTOOL, &ifr) < 0) {
		close (fd);
		return;
	}
	close (fd);

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,27)
	speed = edata.speed;
#else
	speed = ethtool_cmd_speed (&edata);
#endif
	if (speed == G_MAXUINT16 || speed == G_MAXUINT32)
		speed = 0;

	if (priv->speed == speed)
		return;

	priv->speed = speed;
	g_object_notify (G_OBJECT (device), "speed");

	nm_log_dbg (LOGD_HW | LOGD_ETHER, "(%s): speed is now %d Mb/s",
	            nm_device_get_iface (device), speed);
}

static void
carrier_changed (NMDevice *device, gboolean carrier)
{
	if (carrier)
		get_link_speed (device);

	NM_DEVICE_CLASS (nm_device_ethernet_parent_class)->carrier_changed (device, carrier);
}

static void
dispose (GObject *object)
{
	NMDeviceEthernet *self = NM_DEVICE_ETHERNET (object);
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (self);

	if (priv->pppoe_wait_id) {
		g_source_remove (priv->pppoe_wait_id);
		priv->pppoe_wait_id = 0;
	}

	dcb_timeout_cleanup (NM_DEVICE (self));
	dcb_carrier_cleanup (NM_DEVICE (self));

	G_OBJECT_CLASS (nm_device_ethernet_parent_class)->dispose (object);
}

static void
finalize (GObject *object)
{
	NMDeviceEthernet *self = NM_DEVICE_ETHERNET (object);
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (self);

	g_clear_object (&priv->supplicant.mgr);
	g_free (priv->subchan1);
	g_free (priv->subchan2);
	g_free (priv->subchan3);
	g_free (priv->subchannels);
	g_free (priv->s390_nettype);
	g_hash_table_destroy (priv->s390_options);

	G_OBJECT_CLASS (nm_device_ethernet_parent_class)->finalize (object);
}

static void
get_property (GObject *object, guint prop_id,
              GValue *value, GParamSpec *pspec)
{
	NMDeviceEthernet *self = NM_DEVICE_ETHERNET (object);
	NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE (self);

	switch (prop_id) {
	case PROP_PERM_HW_ADDRESS:
		g_value_take_string (value, nm_utils_hwaddr_ntoa (&priv->perm_hw_addr, ARPHRD_ETHER));
		break;
	case PROP_SPEED:
		g_value_set_uint (value, priv->speed);
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
	switch (prop_id) {
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
nm_device_ethernet_class_init (NMDeviceEthernetClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	NMDeviceClass *parent_class = NM_DEVICE_CLASS (klass);

	g_type_class_add_private (object_class, sizeof (NMDeviceEthernetPrivate));

	parent_class->connection_type = NM_SETTING_WIRED_SETTING_NAME;

	/* virtual methods */
	object_class->constructor = constructor;
	object_class->dispose = dispose;
	object_class->finalize = finalize;
	object_class->get_property = get_property;
	object_class->set_property = set_property;

	parent_class->get_generic_capabilities = get_generic_capabilities;
	parent_class->update_permanent_hw_address = update_permanent_hw_address;
	parent_class->update_initial_hw_address = update_initial_hw_address;
	parent_class->check_connection_compatible = check_connection_compatible;
	parent_class->complete_connection = complete_connection;

	parent_class->act_stage1_prepare = act_stage1_prepare;
	parent_class->act_stage2_config = act_stage2_config;
	parent_class->act_stage3_ip4_config_start = act_stage3_ip4_config_start;
	parent_class->ip4_config_pre_commit = ip4_config_pre_commit;
	parent_class->deactivate = deactivate;
	parent_class->spec_match_list = spec_match_list;
	parent_class->update_connection = update_connection;
	parent_class->carrier_changed = carrier_changed;

	parent_class->state_changed = device_state_changed;

	/* properties */
	g_object_class_install_property
		(object_class, PROP_PERM_HW_ADDRESS,
		 g_param_spec_string (NM_DEVICE_ETHERNET_PERMANENT_HW_ADDRESS,
							  "Permanent MAC Address",
							  "Permanent hardware MAC address",
							  NULL,
							  G_PARAM_READABLE));

	g_object_class_install_property
		(object_class, PROP_SPEED,
		 g_param_spec_uint (NM_DEVICE_ETHERNET_SPEED,
						   "Speed",
						   "Speed",
						   0, G_MAXUINT32, 0,
						   G_PARAM_READABLE));

	nm_dbus_manager_register_exported_type (nm_dbus_manager_get (),
	                                        G_TYPE_FROM_CLASS (klass),
	                                        &dbus_glib_nm_device_ethernet_object_info);

	dbus_g_error_domain_register (NM_ETHERNET_ERROR, NULL, NM_TYPE_ETHERNET_ERROR);
}
