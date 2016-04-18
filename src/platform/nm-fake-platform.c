/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* nm-platform-fake.c - Fake platform interaction code for testing NetworkManager
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
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
 * Copyright (C) 2012–2013 Red Hat, Inc.
 */

#include "nm-default.h"

#include "nm-fake-platform.h"

#include <errno.h>
#include <unistd.h>
#include <netinet/icmp6.h>
#include <netinet/in.h>
#include <linux/rtnetlink.h>

#include "nm-utils.h"

#include "nm-core-utils.h"
#include "nmp-object.h"

#include "nm-test-utils.h"

/*********************************************************************************************/

#define _NMLOG_PREFIX_NAME                "platform-fake"
#define _NMLOG_DOMAIN                     LOGD_PLATFORM
#define _NMLOG(level, ...)                _LOG(level, _NMLOG_DOMAIN,  platform, __VA_ARGS__)

#define _LOG(level, domain, self, ...) \
    G_STMT_START { \
        const NMLogLevel __level = (level); \
        const NMLogDomain __domain = (domain); \
        \
        if (nm_logging_enabled (__level, __domain)) { \
            char __prefix[32]; \
            const char *__p_prefix = _NMLOG_PREFIX_NAME; \
            const void *const __self = (self); \
            \
            if (__self && __self != nm_platform_try_get ()) { \
                g_snprintf (__prefix, sizeof (__prefix), "%s[%p]", _NMLOG_PREFIX_NAME, __self); \
                __p_prefix = __prefix; \
            } \
            _nm_log (__level, __domain, 0, \
                     "%s: " _NM_UTILS_MACRO_FIRST (__VA_ARGS__), \
                     __p_prefix _NM_UTILS_MACRO_REST (__VA_ARGS__)); \
        } \
    } G_STMT_END

/*********************************************************************************************/

typedef struct {
	GHashTable *options;
	GArray *links;
	GArray *ip4_addresses;
	GArray *ip6_addresses;
	GArray *ip4_routes;
	GArray *ip6_routes;
} NMFakePlatformPrivate;

typedef struct {
	NMPlatformLink link;

	char *udi;
	NMPObject *lnk;
	struct in6_addr ip6_lladdr;
} NMFakePlatformLink;

#define NM_FAKE_PLATFORM_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), NM_TYPE_FAKE_PLATFORM, NMFakePlatformPrivate))

G_DEFINE_TYPE (NMFakePlatform, nm_fake_platform, NM_TYPE_PLATFORM)

/******************************************************************/

static void link_changed (NMPlatform *platform, NMFakePlatformLink *device, gboolean raise_signal);

static gboolean ip6_address_add (NMPlatform *platform,
                                 int ifindex,
                                 struct in6_addr addr,
                                 int plen,
                                 struct in6_addr peer_addr,
                                 guint32 lifetime,
                                 guint32 preferred,
                                 guint flags);
static gboolean ip6_address_delete (NMPlatform *platform, int ifindex, struct in6_addr addr, int plen);

/******************************************************************/

static gboolean
_ip4_address_equal_peer_net (in_addr_t peer1, in_addr_t peer2, int plen)
{
	return ((peer1 ^ peer2) & nm_utils_ip4_prefix_to_netmask (plen)) == 0;
}

/******************************************************************/

static gboolean
sysctl_set (NMPlatform *platform, const char *path, const char *value)
{
	NMFakePlatformPrivate *priv = NM_FAKE_PLATFORM_GET_PRIVATE (platform);

	g_hash_table_insert (priv->options, g_strdup (path), g_strdup (value));

	return TRUE;
}

static char *
sysctl_get (NMPlatform *platform, const char *path)
{
	NMFakePlatformPrivate *priv = NM_FAKE_PLATFORM_GET_PRIVATE (platform);

	return g_strdup (g_hash_table_lookup (priv->options, path));
}

static const char *
type_to_type_name (NMLinkType type)
{
	switch (type) {
	case NM_LINK_TYPE_UNKNOWN:
		return "unknown";
	case NM_LINK_TYPE_LOOPBACK:
		return "loopback";
	case NM_LINK_TYPE_ETHERNET:
		return "ethernet";
	case NM_LINK_TYPE_DUMMY:
		return "dummy";
	case NM_LINK_TYPE_BRIDGE:
		return "bridge";
	case NM_LINK_TYPE_BOND:
		return "bond";
	case NM_LINK_TYPE_TEAM:
		return "team";
	case NM_LINK_TYPE_VLAN:
		return "vlan";
	case NM_LINK_TYPE_NONE:
	default:
		return NULL;
	}
}

static void
link_init (NMFakePlatformLink *device, int ifindex, int type, const char *name)
{
	gs_free char *ip6_lladdr = NULL;

	g_assert (!name || strlen (name) < sizeof(device->link.name));

	memset (device, 0, sizeof (*device));

	ip6_lladdr = ifindex > 0 ? g_strdup_printf ("fe80::fa1e:%0x:%0x", ifindex / 256, ifindex % 256) : NULL;

	device->link.ifindex = name ? ifindex : 0;
	device->link.type = type;
	device->link.kind = type_to_type_name (type);
	device->link.driver = type_to_type_name (type);
	device->udi = g_strdup_printf ("fake:%d", ifindex);
	device->link.initialized = TRUE;
	device->ip6_lladdr = *nmtst_inet6_from_string (ip6_lladdr);
	if (name)
		strcpy (device->link.name, name);
	switch (device->link.type) {
	case NM_LINK_TYPE_DUMMY:
		device->link.n_ifi_flags = NM_FLAGS_SET (device->link.n_ifi_flags, IFF_NOARP);
		break;
	default:
		device->link.n_ifi_flags = NM_FLAGS_UNSET (device->link.n_ifi_flags, IFF_NOARP);
		break;
	}
}

static NMFakePlatformLink *
link_get (NMPlatform *platform, int ifindex)
{
	NMFakePlatformPrivate *priv = NM_FAKE_PLATFORM_GET_PRIVATE (platform);
	NMFakePlatformLink *device;

	if (ifindex >= priv->links->len)
		goto not_found;
	device = &g_array_index (priv->links, NMFakePlatformLink, ifindex);
	if (!device->link.ifindex)
		goto not_found;

	return device;
not_found:
	_LOGD ("link not found: %d", ifindex);
	return NULL;
}

static GArray *
link_get_all (NMPlatform *platform)
{
	NMFakePlatformPrivate *priv = NM_FAKE_PLATFORM_GET_PRIVATE (platform);
	GArray *links = g_array_sized_new (TRUE, TRUE, sizeof (NMPlatformLink), priv->links->len);
	int i;

	for (i = 0; i < priv->links->len; i++)
		if (g_array_index (priv->links, NMFakePlatformLink, i).link.ifindex)
			g_array_append_val (links, g_array_index (priv->links, NMFakePlatformLink, i).link);

	return links;
}

static const NMPlatformLink *
_nm_platform_link_get (NMPlatform *platform, int ifindex)
{
	NMFakePlatformLink *device = link_get (platform, ifindex);

	return device ? &device->link : NULL;
}

static const NMPlatformLink *
_nm_platform_link_get_by_ifname (NMPlatform *platform, const char *ifname)
{
	NMFakePlatformPrivate *priv = NM_FAKE_PLATFORM_GET_PRIVATE (platform);
	guint i;

	for (i = 0; i < priv->links->len; i++) {
		NMFakePlatformLink *device = &g_array_index (priv->links, NMFakePlatformLink, i);

		if (!strcmp (device->link.name, ifname))
			return &device->link;
	}
	return NULL;
}

static const NMPlatformLink *
_nm_platform_link_get_by_address (NMPlatform *platform,
                                  gconstpointer address,
                                  size_t length)
{
	NMFakePlatformPrivate *priv = NM_FAKE_PLATFORM_GET_PRIVATE (platform);
	guint i;

	if (   length == 0
	    || length > NM_UTILS_HWADDR_LEN_MAX
	    || !address)
		g_return_val_if_reached (NULL);

	for (i = 0; i < priv->links->len; i++) {
		NMFakePlatformLink *device = &g_array_index (priv->links, NMFakePlatformLink, i);

		if (   device->link.addr.len == length
		    && memcmp (device->link.addr.data, address, length) == 0) {
			return &device->link;
		}
	}
	return NULL;
}

static const NMPObject *
link_get_lnk (NMPlatform *platform,
              int ifindex,
              NMLinkType link_type,
              const NMPlatformLink **out_link)
{
	NMFakePlatformLink *device = link_get (platform, ifindex);

	if (!device)
		return NULL;

	NM_SET_OUT (out_link, &device->link);

	if (!device->lnk)
		return NULL;

	if (link_type == NM_LINK_TYPE_NONE)
		return device->lnk;

	if (   link_type != device->link.type
	    || link_type != NMP_OBJECT_GET_CLASS (device->lnk)->lnk_link_type)
		return NULL;

	return device->lnk;
}

static gboolean
link_add (NMPlatform *platform,
          const char *name,
          NMLinkType type,
          const void *address,
          size_t address_len,
          const NMPlatformLink **out_link)
{
	NMFakePlatformPrivate *priv = NM_FAKE_PLATFORM_GET_PRIVATE (platform);
	NMFakePlatformLink device;
	NMFakePlatformLink *new_device;

	link_init (&device, priv->links->len, type, name);

	if (address) {
		g_return_val_if_fail (address_len > 0 && address_len <= sizeof (device.link.addr.data), FALSE);
		memcpy (device.link.addr.data, address, address_len);
		device.link.addr.len = address_len;
	}

	g_array_append_val (priv->links, device);
	new_device = &g_array_index (priv->links, NMFakePlatformLink, priv->links->len - 1);

	if (device.link.ifindex) {
		g_signal_emit_by_name (platform, NM_PLATFORM_SIGNAL_LINK_CHANGED, NMP_OBJECT_TYPE_LINK, device.link.ifindex, &device, NM_PLATFORM_SIGNAL_ADDED);

		link_changed (platform, &g_array_index (priv->links, NMFakePlatformLink, priv->links->len - 1), FALSE);
	}

	if (out_link)
		*out_link = &new_device->link;
	return TRUE;
}

static gboolean
link_delete (NMPlatform *platform, int ifindex)
{
	NMFakePlatformPrivate *priv = NM_FAKE_PLATFORM_GET_PRIVATE (platform);
	NMFakePlatformLink *device = link_get (platform, ifindex);
	NMPlatformLink deleted_device;
	int i;

	if (!device || !device->link.ifindex)
		return FALSE;

	memcpy (&deleted_device, &device->link, sizeof (deleted_device));
	memset (&device->link, 0, sizeof (device->link));
	g_clear_pointer (&device->lnk, nmp_object_unref);
	g_clear_pointer (&device->udi, g_free);

	/* Remove addresses and routes which belong to the deleted interface */
	for (i = 0; i < priv->ip4_addresses->len; i++) {
		NMPlatformIP4Address *address = &g_array_index (priv->ip4_addresses, NMPlatformIP4Address, i);

		if (address->ifindex == ifindex)
			memset (address, 0, sizeof (*address));
	}
	for (i = 0; i < priv->ip6_addresses->len; i++) {
		NMPlatformIP6Address *address = &g_array_index (priv->ip6_addresses, NMPlatformIP6Address, i);

		if (address->ifindex == ifindex)
			memset (address, 0, sizeof (*address));
	}
	for (i = 0; i < priv->ip4_routes->len; i++) {
		NMPlatformIP4Route *route = &g_array_index (priv->ip4_routes, NMPlatformIP4Route, i);

		if (route->ifindex == ifindex)
			memset (route, 0, sizeof (*route));
	}
	for (i = 0; i < priv->ip6_routes->len; i++) {
		NMPlatformIP6Route *route = &g_array_index (priv->ip6_routes, NMPlatformIP6Route, i);

		if (route->ifindex == ifindex)
			memset (route, 0, sizeof (*route));
	}

	g_signal_emit_by_name (platform, NM_PLATFORM_SIGNAL_LINK_CHANGED, NMP_OBJECT_TYPE_LINK, ifindex, &deleted_device, NM_PLATFORM_SIGNAL_REMOVED);

	return TRUE;
}

static const char *
link_get_type_name (NMPlatform *platform, int ifindex)
{
	return type_to_type_name (nm_platform_link_get_type (platform, ifindex));
}

static void
link_changed (NMPlatform *platform, NMFakePlatformLink *device, gboolean raise_signal)
{
	NMFakePlatformPrivate *priv = NM_FAKE_PLATFORM_GET_PRIVATE (platform);
	int i;

	if (raise_signal)
		g_signal_emit_by_name (platform, NM_PLATFORM_SIGNAL_LINK_CHANGED, NMP_OBJECT_TYPE_LINK, device->link.ifindex, &device->link, NM_PLATFORM_SIGNAL_CHANGED);

	if (device->link.ifindex && !IN6_IS_ADDR_UNSPECIFIED (&device->ip6_lladdr)) {
		if (device->link.connected)
			ip6_address_add (platform, device->link.ifindex, in6addr_any, 64, device->ip6_lladdr, NM_PLATFORM_LIFETIME_PERMANENT, NM_PLATFORM_LIFETIME_PERMANENT, 0);
		else
			ip6_address_delete (platform, device->link.ifindex, device->ip6_lladdr, 64);
	}

	if (device->link.master) {
		gboolean connected = FALSE;

		NMFakePlatformLink *master = link_get (platform, device->link.master);

		g_return_if_fail (master && master != device);

		for (i = 0; i < priv->links->len; i++) {
			NMFakePlatformLink *slave = &g_array_index (priv->links, NMFakePlatformLink, i);

			if (slave && slave->link.master == master->link.ifindex && slave->link.connected)
				connected = TRUE;
		}

		if (master->link.connected != connected) {
			master->link.connected = connected;
			link_changed (platform, master, TRUE);
		}
	}
}

static gboolean
link_set_up (NMPlatform *platform, int ifindex, gboolean *out_no_firmware)
{
	NMFakePlatformLink *device = link_get (platform, ifindex);
	gboolean up, connected;

	if (out_no_firmware)
		*out_no_firmware = FALSE;

	if (!device) {
		_LOGE ("failure changing link: netlink error (No such device)");
		return FALSE;
	}

	up = TRUE;
	connected = TRUE;
	switch (device->link.type) {
	case NM_LINK_TYPE_DUMMY:
	case NM_LINK_TYPE_VLAN:
		break;
	case NM_LINK_TYPE_BRIDGE:
	case NM_LINK_TYPE_BOND:
	case NM_LINK_TYPE_TEAM:
		connected = FALSE;
		break;
	default:
		connected = FALSE;
		g_error ("Unexpected device type: %d", device->link.type);
	}

	if (   NM_FLAGS_HAS (device->link.n_ifi_flags, IFF_UP) != !!up
	    || device->link.connected != connected) {
		device->link.n_ifi_flags = NM_FLAGS_ASSIGN (device->link.n_ifi_flags, IFF_UP, up);
		device->link.connected = connected;
		link_changed (platform, device, TRUE);
	}

	return TRUE;
}

static gboolean
link_set_down (NMPlatform *platform, int ifindex)
{
	NMFakePlatformLink *device = link_get (platform, ifindex);

	if (!device) {
		_LOGE ("failure changing link: netlink error (No such device)");
		return FALSE;
	}

	if (NM_FLAGS_HAS (device->link.n_ifi_flags, IFF_UP) || device->link.connected) {
		device->link.n_ifi_flags = NM_FLAGS_UNSET (device->link.n_ifi_flags, IFF_UP);
		device->link.connected = FALSE;

		link_changed (platform, device, TRUE);
	}

	return TRUE;
}

static gboolean
link_set_arp (NMPlatform *platform, int ifindex)
{
	NMFakePlatformLink *device = link_get (platform, ifindex);

	if (!device) {
		_LOGE ("failure changing link: netlink error (No such device)");
		return FALSE;
	}

	device->link.n_ifi_flags = NM_FLAGS_UNSET (device->link.n_ifi_flags, IFF_NOARP);

	link_changed (platform, device, TRUE);

	return TRUE;
}

static gboolean
link_set_noarp (NMPlatform *platform, int ifindex)
{
	NMFakePlatformLink *device = link_get (platform, ifindex);

	if (!device) {
		_LOGE ("failure changing link: netlink error (No such device)");
		return FALSE;
	}

	device->link.n_ifi_flags = NM_FLAGS_SET (device->link.n_ifi_flags, IFF_NOARP);

	link_changed (platform, device, TRUE);

	return TRUE;
}

static gboolean
link_set_address (NMPlatform *platform, int ifindex, gconstpointer addr, size_t len)
{
	NMFakePlatformLink *device = link_get (platform, ifindex);

	if (   !device
	    || len == 0
	    || len > NM_UTILS_HWADDR_LEN_MAX
	    || !addr)
		g_return_val_if_reached (FALSE);

	if (   device->link.addr.len != len
	    || (   len > 0
	        && memcmp (device->link.addr.data, addr, len) != 0)) {
		memcpy (device->link.addr.data, addr, len);
		device->link.addr.len = len;
		link_changed (platform, link_get (platform, ifindex), TRUE);
	}

	return TRUE;
}

static gboolean
link_set_mtu (NMPlatform *platform, int ifindex, guint32 mtu)
{
	NMFakePlatformLink *device = link_get (platform, ifindex);

	if (device) {
		device->link.mtu = mtu;
		link_changed (platform, device, TRUE);
	} else
		_LOGE ("failure changing link: netlink error (No such device)");

	return !!device;
}

static const char *
link_get_udi (NMPlatform *platform, int ifindex)
{
	NMFakePlatformLink *device = link_get (platform, ifindex);

	if (!device)
		return NULL;
	return device->udi;
}

static gboolean
link_get_driver_info (NMPlatform *platform,
                      int ifindex,
                      char **out_driver_name,
                      char **out_driver_version,
                      char **out_fw_version)
{
	if (out_driver_name)
		*out_driver_name = NULL;
	if (out_driver_version)
		*out_driver_version = NULL;
	if (out_fw_version)
		*out_fw_version = NULL;

	return TRUE;
}

static gboolean
link_supports_carrier_detect (NMPlatform *platform, int ifindex)
{
	NMFakePlatformLink *device = link_get (platform, ifindex);

	if (!device)
		return FALSE;

	switch (device->link.type) {
	case NM_LINK_TYPE_DUMMY:
		return FALSE;
	default:
		return TRUE;
	}
}

static gboolean
link_supports_vlans (NMPlatform *platform, int ifindex)
{
	NMFakePlatformLink *device = link_get (platform, ifindex);

	if (!device)
		return FALSE;

	switch (device->link.type) {
	case NM_LINK_TYPE_LOOPBACK:
		return FALSE;
	default:
		return TRUE;
	}
}

static gboolean
link_enslave (NMPlatform *platform, int master, int slave)
{
	NMFakePlatformLink *device = link_get (platform, slave);
	NMFakePlatformLink *master_device = link_get (platform, master);

	g_return_val_if_fail (device, FALSE);
	g_return_val_if_fail (master_device, FALSE);

	if (device->link.master != master) {
		device->link.master = master;

		if (NM_IN_SET (master_device->link.type, NM_LINK_TYPE_BOND, NM_LINK_TYPE_TEAM)) {
			device->link.n_ifi_flags = NM_FLAGS_SET (device->link.n_ifi_flags, IFF_UP);
			device->link.connected = TRUE;
		}

		link_changed (platform, device, TRUE);
	}

	return TRUE;
}

static gboolean
link_release (NMPlatform *platform, int master_idx, int slave_idx)
{
	NMFakePlatformLink *master = link_get (platform, master_idx);
	NMFakePlatformLink *slave = link_get (platform, slave_idx);

	g_return_val_if_fail (master, FALSE);
	g_return_val_if_fail (slave, FALSE);

	if (slave->link.master != master->link.ifindex)
		return FALSE;

	slave->link.master = 0;

	link_changed (platform, slave, TRUE);
	link_changed (platform, master, TRUE);

	return TRUE;
}

static gboolean
vlan_add (NMPlatform *platform, const char *name, int parent, int vlan_id, guint32 vlan_flags, const NMPlatformLink **out_link)
{
	NMFakePlatformLink *device;

	if (!link_add (platform, name, NM_LINK_TYPE_VLAN, NULL, 0, out_link))
		return FALSE;

	device = link_get (platform, nm_platform_link_get_ifindex (platform, name));

	g_return_val_if_fail (device, FALSE);
	g_return_val_if_fail (!device->lnk, FALSE);

	device->lnk = nmp_object_new (NMP_OBJECT_TYPE_LNK_VLAN, NULL);
	device->lnk->lnk_vlan.id = vlan_id;
	device->link.parent = parent;

	if (out_link)
		*out_link = &device->link;
	return TRUE;
}

static gboolean
link_vlan_change (NMPlatform *platform,
                  int ifindex,
                  NMVlanFlags flags_mask,
                  NMVlanFlags flags_set,
                  gboolean ingress_reset_all,
                  const NMVlanQosMapping *ingress_map,
                  gsize n_ingress_map,
                  gboolean egress_reset_all,
                  const NMVlanQosMapping *egress_map,
                  gsize n_egress_map)
{
	return FALSE;
}

static gboolean
link_vxlan_add (NMPlatform *platform,
                const char *name,
                const NMPlatformLnkVxlan *props,
                const NMPlatformLink **out_link)
{
	NMFakePlatformLink *device;

	if (!link_add (platform, name, NM_LINK_TYPE_VXLAN, NULL, 0, out_link))
		return FALSE;

	device = link_get (platform, nm_platform_link_get_ifindex (platform, name));

	g_return_val_if_fail (device, FALSE);
	g_return_val_if_fail (!device->lnk, FALSE);

	device->lnk = nmp_object_new (NMP_OBJECT_TYPE_LNK_VXLAN, NULL);
	device->lnk->lnk_vxlan = *props;
	device->link.parent = props->parent_ifindex;

	if (out_link)
		*out_link = &device->link;
	return TRUE;
}

static gboolean
infiniband_partition_add (NMPlatform *platform, int parent, int p_key, const NMPlatformLink **out_link)
{
	NMFakePlatformLink *device, *parent_device;
	gs_free char *name = NULL;

	parent_device = link_get (platform, parent);
	g_return_val_if_fail (parent_device != NULL, FALSE);

	name = g_strdup_printf ("%s.%04x", parent_device->link.name, p_key);
	if (!link_add (platform, name, NM_LINK_TYPE_INFINIBAND, NULL, 0, out_link))
		return FALSE;

	device = link_get (platform, nm_platform_link_get_ifindex (platform, name));
	g_return_val_if_fail (device, FALSE);
	g_return_val_if_fail (!device->lnk, FALSE);

	device->lnk = nmp_object_new (NMP_OBJECT_TYPE_LNK_VLAN, NULL);
	device->lnk->lnk_infiniband.p_key = p_key;
	device->lnk->lnk_infiniband.mode = "datagram";
	device->link.parent = parent;

	return TRUE;
}

static gboolean
wifi_get_capabilities (NMPlatform *platform, int ifindex, NMDeviceWifiCapabilities *caps)
{
	NMFakePlatformLink *device = link_get (platform, ifindex);

	g_return_val_if_fail (device, FALSE);

	if (device->link.type != NM_LINK_TYPE_WIFI)
		return FALSE;

	if (caps) {
		*caps = (  NM_WIFI_DEVICE_CAP_CIPHER_WEP40
		         | NM_WIFI_DEVICE_CAP_CIPHER_WEP104
		         | NM_WIFI_DEVICE_CAP_CIPHER_TKIP
		         | NM_WIFI_DEVICE_CAP_CIPHER_CCMP
		         | NM_WIFI_DEVICE_CAP_WPA
		         | NM_WIFI_DEVICE_CAP_RSN
		         | NM_WIFI_DEVICE_CAP_AP
		         | NM_WIFI_DEVICE_CAP_ADHOC);
	}
	return TRUE;
}

static gboolean
wifi_get_bssid (NMPlatform *platform, int ifindex, guint8 *bssid)
{
	return FALSE;
}

static GByteArray *
wifi_get_ssid (NMPlatform *platform, int ifindex)
{
	return NULL;
}

static guint32
wifi_get_frequency (NMPlatform *platform, int ifindex)
{
	return 0;
}

static int
wifi_get_quality (NMPlatform *platform, int ifindex)
{
	return 0;
}

static guint32
wifi_get_rate (NMPlatform *platform, int ifindex)
{
	return 0;
}

static NM80211Mode
wifi_get_mode (NMPlatform *platform, int ifindex)
{
	return NM_802_11_MODE_UNKNOWN;
}

static void
wifi_set_mode (NMPlatform *platform, int ifindex, NM80211Mode mode)
{
	;
}

static guint32
wifi_find_frequency (NMPlatform *platform, int ifindex, const guint32 *freqs)
{
	return freqs[0];
}

static void
wifi_indicate_addressing_running (NMPlatform *platform, int ifindex, gboolean running)
{
	;
}

static guint32
mesh_get_channel (NMPlatform *platform, int ifindex)
{
	return 0;
}

static gboolean
mesh_set_channel (NMPlatform *platform, int ifindex, guint32 channel)
{
	return FALSE;
}

static gboolean
mesh_set_ssid (NMPlatform *platform, int ifindex, const guint8 *ssid, gsize len)
{
	return FALSE;
}

/******************************************************************/

static GArray *
ip4_address_get_all (NMPlatform *platform, int ifindex)
{
	NMFakePlatformPrivate *priv = NM_FAKE_PLATFORM_GET_PRIVATE (platform);
	GArray *addresses;
	NMPlatformIP4Address *address;
	int count = 0, i;

	/* Count addresses */
	for (i = 0; i < priv->ip4_addresses->len; i++) {
		address = &g_array_index (priv->ip4_addresses, NMPlatformIP4Address, i);
		if (address && address->ifindex == ifindex)
			count++;
	}

	addresses = g_array_sized_new (TRUE, TRUE, sizeof (NMPlatformIP4Address), count);

	/* Fill addresses */
	for (i = 0; i < priv->ip4_addresses->len; i++) {
		address = &g_array_index (priv->ip4_addresses, NMPlatformIP4Address, i);
		if (address && address->ifindex == ifindex)
			g_array_append_val (addresses, *address);
	}

	return addresses;
}

static GArray *
ip6_address_get_all (NMPlatform *platform, int ifindex)
{
	NMFakePlatformPrivate *priv = NM_FAKE_PLATFORM_GET_PRIVATE (platform);
	GArray *addresses;
	NMPlatformIP6Address *address;
	int count = 0, i;

	/* Count addresses */
	for (i = 0; i < priv->ip6_addresses->len; i++) {
		address = &g_array_index (priv->ip6_addresses, NMPlatformIP6Address, i);
		if (address && address->ifindex == ifindex)
			count++;
	}

	addresses = g_array_sized_new (TRUE, TRUE, sizeof (NMPlatformIP6Address), count);

	/* Fill addresses */
	count = 0;
	for (i = 0; i < priv->ip6_addresses->len; i++) {
		address = &g_array_index (priv->ip6_addresses, NMPlatformIP6Address, i);
		if (address && address->ifindex == ifindex)
			g_array_append_val (addresses, *address);
	}

	return addresses;
}

static gboolean
ip4_address_add (NMPlatform *platform,
                 int ifindex,
                 in_addr_t addr,
                 int plen,
                 in_addr_t peer_addr,
                 guint32 lifetime,
                 guint32 preferred,
                 guint32 flags,
                 const char *label)
{
	NMFakePlatformPrivate *priv = NM_FAKE_PLATFORM_GET_PRIVATE (platform);
	NMPlatformIP4Address address;
	int i;

	memset (&address, 0, sizeof (address));
	address.source = NM_IP_CONFIG_SOURCE_KERNEL;
	address.ifindex = ifindex;
	address.address = addr;
	address.peer_address = peer_addr;
	address.plen = plen;
	address.timestamp = nm_utils_get_monotonic_timestamp_s ();
	address.lifetime = lifetime;
	address.preferred = preferred;
	address.n_ifa_flags = flags;
	if (label)
		g_strlcpy (address.label, label, sizeof (address.label));

	for (i = 0; i < priv->ip4_addresses->len; i++) {
		NMPlatformIP4Address *item = &g_array_index (priv->ip4_addresses, NMPlatformIP4Address, i);
		gboolean changed;

		if (   item->ifindex != address.ifindex
		    || item->address != address.address
		    || item->plen != address.plen
		    || !_ip4_address_equal_peer_net (item->peer_address, address.peer_address, address.plen))
			continue;

		changed = !nm_platform_ip4_address_cmp (item, &address);

		memcpy (item, &address, sizeof (address));
		if (changed)
			g_signal_emit_by_name (platform, NM_PLATFORM_SIGNAL_IP4_ADDRESS_CHANGED, NMP_OBJECT_TYPE_IP4_ADDRESS, ifindex, &address, NM_PLATFORM_SIGNAL_CHANGED);
		return TRUE;
	}

	g_array_append_val (priv->ip4_addresses, address);
	g_signal_emit_by_name (platform, NM_PLATFORM_SIGNAL_IP4_ADDRESS_CHANGED, NMP_OBJECT_TYPE_IP4_ADDRESS, ifindex, &address, NM_PLATFORM_SIGNAL_ADDED);

	return TRUE;
}

static gboolean
ip6_address_add (NMPlatform *platform,
                 int ifindex,
                 struct in6_addr addr,
                 int plen,
                 struct in6_addr peer_addr,
                 guint32 lifetime,
                 guint32 preferred,
                 guint32 flags)
{
	NMFakePlatformPrivate *priv = NM_FAKE_PLATFORM_GET_PRIVATE (platform);
	NMPlatformIP6Address address;
	int i;

	memset (&address, 0, sizeof (address));
	address.source = NM_IP_CONFIG_SOURCE_KERNEL;
	address.ifindex = ifindex;
	address.address = addr;
	address.peer_address = (IN6_IS_ADDR_UNSPECIFIED (&peer_addr) || IN6_ARE_ADDR_EQUAL (&addr, &peer_addr)) ? in6addr_any : peer_addr;
	address.plen = plen;
	address.timestamp = nm_utils_get_monotonic_timestamp_s ();
	address.lifetime = lifetime;
	address.preferred = preferred;
	address.n_ifa_flags = flags;

	for (i = 0; i < priv->ip6_addresses->len; i++) {
		NMPlatformIP6Address *item = &g_array_index (priv->ip6_addresses, NMPlatformIP6Address, i);
		gboolean changed;

		if (   item->ifindex != address.ifindex
		    || !IN6_ARE_ADDR_EQUAL (&item->address, &address.address))
			continue;

		changed = !nm_platform_ip6_address_cmp (item, &address);

		memcpy (item, &address, sizeof (address));
		if (changed)
			g_signal_emit_by_name (platform, NM_PLATFORM_SIGNAL_IP6_ADDRESS_CHANGED, NMP_OBJECT_TYPE_IP6_ADDRESS, ifindex, &address, NM_PLATFORM_SIGNAL_CHANGED);
		return TRUE;
	}

	g_array_append_val (priv->ip6_addresses, address);
	g_signal_emit_by_name (platform, NM_PLATFORM_SIGNAL_IP6_ADDRESS_CHANGED, NMP_OBJECT_TYPE_IP6_ADDRESS, ifindex, &address, NM_PLATFORM_SIGNAL_ADDED);

	return TRUE;
}

static gboolean
ip4_address_delete (NMPlatform *platform, int ifindex, in_addr_t addr, int plen, in_addr_t peer_address)
{
	NMFakePlatformPrivate *priv = NM_FAKE_PLATFORM_GET_PRIVATE (platform);
	int i;

	for (i = 0; i < priv->ip4_addresses->len; i++) {
		NMPlatformIP4Address *address = &g_array_index (priv->ip4_addresses, NMPlatformIP4Address, i);

		if (   address->ifindex == ifindex
		    && address->plen == plen
		    && address->address == addr
		    && ((peer_address ^ address->peer_address) & nm_utils_ip4_prefix_to_netmask (plen)) == 0) {
			NMPlatformIP4Address deleted_address;

			memcpy (&deleted_address, address, sizeof (deleted_address));
			memset (address, 0, sizeof (*address));
			g_signal_emit_by_name (platform, NM_PLATFORM_SIGNAL_IP4_ADDRESS_CHANGED, NMP_OBJECT_TYPE_IP4_ADDRESS, ifindex, &deleted_address, NM_PLATFORM_SIGNAL_REMOVED);
			return TRUE;
		}
	}

	return TRUE;
}

static gboolean
ip6_address_delete (NMPlatform *platform, int ifindex, struct in6_addr addr, int plen)
{
	NMFakePlatformPrivate *priv = NM_FAKE_PLATFORM_GET_PRIVATE (platform);
	int i;

	for (i = 0; i < priv->ip6_addresses->len; i++) {
		NMPlatformIP6Address *address = &g_array_index (priv->ip6_addresses, NMPlatformIP6Address, i);

		if (   address->ifindex == ifindex
		    && address->plen == plen
		    && IN6_ARE_ADDR_EQUAL (&address->address, &addr)) {
			NMPlatformIP6Address deleted_address;

			memcpy (&deleted_address, address, sizeof (deleted_address));
			memset (address, 0, sizeof (*address));
			g_signal_emit_by_name (platform, NM_PLATFORM_SIGNAL_IP6_ADDRESS_CHANGED, NMP_OBJECT_TYPE_IP6_ADDRESS, ifindex, &deleted_address, NM_PLATFORM_SIGNAL_REMOVED);
			return TRUE;
		}
	}

	return TRUE;
}

static const NMPlatformIP4Address *
ip4_address_get (NMPlatform *platform, int ifindex, in_addr_t addr, int plen, in_addr_t peer_address)
{
	NMFakePlatformPrivate *priv = NM_FAKE_PLATFORM_GET_PRIVATE (platform);
	int i;

	for (i = 0; i < priv->ip4_addresses->len; i++) {
		NMPlatformIP4Address *address = &g_array_index (priv->ip4_addresses, NMPlatformIP4Address, i);

		if (   address->ifindex == ifindex
		    && address->plen == plen
		    && address->address == addr
		    && _ip4_address_equal_peer_net (address->peer_address, peer_address, plen))
			return address;
	}

	return NULL;
}

static const NMPlatformIP6Address *
ip6_address_get (NMPlatform *platform, int ifindex, struct in6_addr addr, int plen)
{
	NMFakePlatformPrivate *priv = NM_FAKE_PLATFORM_GET_PRIVATE (platform);
	int i;

	for (i = 0; i < priv->ip6_addresses->len; i++) {
		NMPlatformIP6Address *address = &g_array_index (priv->ip6_addresses, NMPlatformIP6Address, i);

		if (address->ifindex == ifindex && address->plen == plen &&
				IN6_ARE_ADDR_EQUAL (&address->address, &addr))
			return address;
	}

	return NULL;
}

/******************************************************************/

static GArray *
ip4_route_get_all (NMPlatform *platform, int ifindex, NMPlatformGetRouteFlags flags)
{
	NMFakePlatformPrivate *priv = NM_FAKE_PLATFORM_GET_PRIVATE (platform);
	GArray *routes;
	NMPlatformIP4Route *route;
	guint i;

	routes = g_array_new (TRUE, TRUE, sizeof (NMPlatformIP4Route));

	if (!NM_FLAGS_ANY (flags, NM_PLATFORM_GET_ROUTE_FLAGS_WITH_DEFAULT | NM_PLATFORM_GET_ROUTE_FLAGS_WITH_NON_DEFAULT))
		flags |= NM_PLATFORM_GET_ROUTE_FLAGS_WITH_DEFAULT | NM_PLATFORM_GET_ROUTE_FLAGS_WITH_NON_DEFAULT;

	/* Fill routes */
	for (i = 0; i < priv->ip4_routes->len; i++) {
		route = &g_array_index (priv->ip4_routes, NMPlatformIP4Route, i);
		if (route && (!ifindex || route->ifindex == ifindex)) {
			if (NM_PLATFORM_IP_ROUTE_IS_DEFAULT (route)) {
				if (NM_FLAGS_HAS (flags, NM_PLATFORM_GET_ROUTE_FLAGS_WITH_DEFAULT))
					g_array_append_val (routes, *route);
			} else {
				if (NM_FLAGS_HAS (flags, NM_PLATFORM_GET_ROUTE_FLAGS_WITH_NON_DEFAULT))
					g_array_append_val (routes, *route);
			}
		}
	}

	return routes;
}

static GArray *
ip6_route_get_all (NMPlatform *platform, int ifindex, NMPlatformGetRouteFlags flags)
{
	NMFakePlatformPrivate *priv = NM_FAKE_PLATFORM_GET_PRIVATE (platform);
	GArray *routes;
	NMPlatformIP6Route *route;
	guint i;

	routes = g_array_new (TRUE, TRUE, sizeof (NMPlatformIP6Route));

	if (!NM_FLAGS_ANY (flags, NM_PLATFORM_GET_ROUTE_FLAGS_WITH_DEFAULT | NM_PLATFORM_GET_ROUTE_FLAGS_WITH_NON_DEFAULT))
		flags |= NM_PLATFORM_GET_ROUTE_FLAGS_WITH_DEFAULT | NM_PLATFORM_GET_ROUTE_FLAGS_WITH_NON_DEFAULT;

	/* Fill routes */
	for (i = 0; i < priv->ip6_routes->len; i++) {
		route = &g_array_index (priv->ip6_routes, NMPlatformIP6Route, i);
		if (route && (!ifindex || route->ifindex == ifindex)) {
			if (NM_PLATFORM_IP_ROUTE_IS_DEFAULT (route)) {
				if (NM_FLAGS_HAS (flags, NM_PLATFORM_GET_ROUTE_FLAGS_WITH_DEFAULT))
					g_array_append_val (routes, *route);
			} else {
				if (NM_FLAGS_HAS (flags, NM_PLATFORM_GET_ROUTE_FLAGS_WITH_NON_DEFAULT))
					g_array_append_val (routes, *route);
			}
		}
	}

	return routes;
}

static gboolean
ip4_route_delete (NMPlatform *platform, int ifindex, in_addr_t network, int plen, guint32 metric)
{
	NMFakePlatformPrivate *priv = NM_FAKE_PLATFORM_GET_PRIVATE (platform);
	int i;

	for (i = 0; i < priv->ip4_routes->len; i++) {
		NMPlatformIP4Route *route = &g_array_index (priv->ip4_routes, NMPlatformIP4Route, i);
		NMPlatformIP4Route deleted_route;

		if (   route->ifindex != ifindex
		    || route->network != network
		    || route->plen != plen
		    || route->metric != metric)
			continue;

		memcpy (&deleted_route, route, sizeof (deleted_route));
		g_array_remove_index (priv->ip4_routes, i);
		g_signal_emit_by_name (platform, NM_PLATFORM_SIGNAL_IP4_ROUTE_CHANGED, NMP_OBJECT_TYPE_IP4_ROUTE, ifindex, &deleted_route, NM_PLATFORM_SIGNAL_REMOVED);
	}

	return TRUE;
}

static gboolean
ip6_route_delete (NMPlatform *platform, int ifindex, struct in6_addr network, int plen, guint32 metric)
{
	NMFakePlatformPrivate *priv = NM_FAKE_PLATFORM_GET_PRIVATE (platform);
	int i;

	metric = nm_utils_ip6_route_metric_normalize (metric);

	for (i = 0; i < priv->ip6_routes->len; i++) {
		NMPlatformIP6Route *route = &g_array_index (priv->ip6_routes, NMPlatformIP6Route, i);
		NMPlatformIP6Route deleted_route;

		if (   route->ifindex != ifindex
		    || !IN6_ARE_ADDR_EQUAL (&route->network, &network)
		    || route->plen != plen
		    || route->metric != metric)
			continue;

		memcpy (&deleted_route, route, sizeof (deleted_route));
		g_array_remove_index (priv->ip6_routes, i);
		g_signal_emit_by_name (platform, NM_PLATFORM_SIGNAL_IP6_ROUTE_CHANGED, NMP_OBJECT_TYPE_IP6_ROUTE, ifindex, &deleted_route, NM_PLATFORM_SIGNAL_REMOVED);
	}

	return TRUE;
}

static gboolean
ip4_route_add (NMPlatform *platform, int ifindex, NMIPConfigSource source,
               in_addr_t network, int plen, in_addr_t gateway,
               in_addr_t pref_src, guint32 metric, guint32 mss)
{
	NMFakePlatformPrivate *priv = NM_FAKE_PLATFORM_GET_PRIVATE (platform);
	NMPlatformIP4Route route;
	guint i;
	guint8 scope;

	scope = gateway == 0 ? RT_SCOPE_LINK : RT_SCOPE_UNIVERSE;

	memset (&route, 0, sizeof (route));
	route.source = NM_IP_CONFIG_SOURCE_KERNEL;
	route.ifindex = ifindex;
	route.source = source;
	route.network = nm_utils_ip4_address_clear_host_address (network, plen);
	route.plen = plen;
	route.gateway = gateway;
	route.metric = metric;
	route.mss = mss;
	route.scope_inv = nm_platform_route_scope_inv (scope);

	if (gateway) {
		for (i = 0; i < priv->ip4_routes->len; i++) {
			NMPlatformIP4Route *item = &g_array_index (priv->ip4_routes,
			                                           NMPlatformIP4Route, i);
			guint32 gate = ntohl (item->network) >> (32 - item->plen);
			guint32 host = ntohl (gateway) >> (32 - item->plen);

			if (ifindex == item->ifindex && gate == host)
				break;
		}
		if (i == priv->ip4_routes->len) {
			nm_log_warn (LOGD_PLATFORM, "Fake platform: failure adding ip4-route '%d: %s/%d %d': Network Unreachable",
			             route.ifindex, nm_utils_inet4_ntop (route.network, NULL), route.plen, route.metric);
			return FALSE;
		}
	}

	for (i = 0; i < priv->ip4_routes->len; i++) {
		NMPlatformIP4Route *item = &g_array_index (priv->ip4_routes, NMPlatformIP4Route, i);

		if (item->network != route.network)
			continue;
		if (item->plen != route.plen)
			continue;
		if (item->metric != metric)
			continue;

		if (item->ifindex != route.ifindex) {
			ip4_route_delete (platform, item->ifindex, item->network, item->plen, item->metric);
			i--;
			continue;
		}

		memcpy (item, &route, sizeof (route));
		g_signal_emit_by_name (platform, NM_PLATFORM_SIGNAL_IP4_ROUTE_CHANGED, NMP_OBJECT_TYPE_IP4_ROUTE, ifindex, &route, NM_PLATFORM_SIGNAL_CHANGED);
		return TRUE;
	}

	g_array_append_val (priv->ip4_routes, route);
	g_signal_emit_by_name (platform, NM_PLATFORM_SIGNAL_IP4_ROUTE_CHANGED, NMP_OBJECT_TYPE_IP4_ROUTE, ifindex, &route, NM_PLATFORM_SIGNAL_ADDED);

	return TRUE;
}

static gboolean
ip6_route_add (NMPlatform *platform, int ifindex, NMIPConfigSource source,
               struct in6_addr network, int plen, struct in6_addr gateway,
               guint32 metric, guint32 mss)
{
	NMFakePlatformPrivate *priv = NM_FAKE_PLATFORM_GET_PRIVATE (platform);
	NMPlatformIP6Route route;
	guint i;

	metric = nm_utils_ip6_route_metric_normalize (metric);

	memset (&route, 0, sizeof (route));
	route.source = NM_IP_CONFIG_SOURCE_KERNEL;
	route.ifindex = ifindex;
	route.source = source;
	nm_utils_ip6_address_clear_host_address (&route.network, &network, plen);
	route.plen = plen;
	route.gateway = gateway;
	route.metric = metric;
	route.mss = mss;

	if (!IN6_IS_ADDR_UNSPECIFIED(&gateway)) {
		for (i = 0; i < priv->ip6_routes->len; i++) {
			NMPlatformIP6Route *item = &g_array_index (priv->ip6_routes,
			                                           NMPlatformIP6Route, i);
			guint8 gate_bits = gateway.s6_addr[item->plen / 8] >> (8 - item->plen % 8);
			guint8 host_bits = item->network.s6_addr[item->plen / 8] >> (8 - item->plen % 8);

			if (   ifindex == item->ifindex
			    && memcmp (&gateway, &item->network, item->plen / 8) == 0
			    && gate_bits == host_bits)
				break;
		}
		if (i == priv->ip6_routes->len) {
			nm_log_warn (LOGD_PLATFORM, "Fake platform: failure adding ip6-route '%d: %s/%d %d': Network Unreachable",
			             route.ifindex, nm_utils_inet6_ntop (&route.network, NULL), route.plen, route.metric);
			return FALSE;
		}
	}

	for (i = 0; i < priv->ip6_routes->len; i++) {
		NMPlatformIP6Route *item = &g_array_index (priv->ip6_routes, NMPlatformIP6Route, i);

		if (!IN6_ARE_ADDR_EQUAL (&item->network, &route.network))
			continue;
		if (item->plen != route.plen)
			continue;
		if (item->metric != metric)
			continue;

		if (item->ifindex != route.ifindex) {
			ip6_route_delete (platform, item->ifindex, item->network, item->plen, item->metric);
			i--;
			continue;
		}

		memcpy (item, &route, sizeof (route));
		g_signal_emit_by_name (platform, NM_PLATFORM_SIGNAL_IP6_ROUTE_CHANGED, NMP_OBJECT_TYPE_IP6_ROUTE, ifindex, &route, NM_PLATFORM_SIGNAL_CHANGED);
		return TRUE;
	}

	g_array_append_val (priv->ip6_routes, route);
	g_signal_emit_by_name (platform, NM_PLATFORM_SIGNAL_IP6_ROUTE_CHANGED, NMP_OBJECT_TYPE_IP6_ROUTE, ifindex, &route, NM_PLATFORM_SIGNAL_ADDED);

	return TRUE;
}

static const NMPlatformIP4Route *
ip4_route_get (NMPlatform *platform, int ifindex, in_addr_t network, int plen, guint32 metric)
{
	NMFakePlatformPrivate *priv = NM_FAKE_PLATFORM_GET_PRIVATE (platform);
	int i;

	for (i = 0; i < priv->ip4_routes->len; i++) {
		NMPlatformIP4Route *route = &g_array_index (priv->ip4_routes, NMPlatformIP4Route, i);

		if (route->ifindex == ifindex
				&& route->network == network
				&& route->plen == plen
				&& route->metric == metric)
			return route;
	}

	return NULL;
}

static const NMPlatformIP6Route *
ip6_route_get (NMPlatform *platform, int ifindex, struct in6_addr network, int plen, guint32 metric)
{
	NMFakePlatformPrivate *priv = NM_FAKE_PLATFORM_GET_PRIVATE (platform);
	int i;

	metric = nm_utils_ip6_route_metric_normalize (metric);

	for (i = 0; i < priv->ip6_routes->len; i++) {
		NMPlatformIP6Route *route = &g_array_index (priv->ip6_routes, NMPlatformIP6Route, i);

		if (route->ifindex == ifindex
				&& IN6_ARE_ADDR_EQUAL (&route->network, &network)
				&& route->plen == plen
				&& route->metric == metric)
			return route;
	}

	return NULL;
}

/******************************************************************/

static void
nm_fake_platform_init (NMFakePlatform *fake_platform)
{
	NMFakePlatformPrivate *priv = NM_FAKE_PLATFORM_GET_PRIVATE (fake_platform);

	priv->options = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
	priv->links = g_array_new (TRUE, TRUE, sizeof (NMFakePlatformLink));
	priv->ip4_addresses = g_array_new (TRUE, TRUE, sizeof (NMPlatformIP4Address));
	priv->ip6_addresses = g_array_new (TRUE, TRUE, sizeof (NMPlatformIP6Address));
	priv->ip4_routes = g_array_new (TRUE, TRUE, sizeof (NMPlatformIP4Route));
	priv->ip6_routes = g_array_new (TRUE, TRUE, sizeof (NMPlatformIP6Route));
}

void
nm_fake_platform_setup (void)
{
	NMPlatform *platform;

	platform = g_object_new (NM_TYPE_FAKE_PLATFORM, NULL);

	nm_platform_setup (platform);

	/* skip zero element */
	link_add (platform, NULL, NM_LINK_TYPE_NONE, NULL, 0, NULL);

	/* add loopback interface */
	link_add (platform, "lo", NM_LINK_TYPE_LOOPBACK, NULL, 0, NULL);

	/* add some ethernets */
	link_add (platform, "eth0", NM_LINK_TYPE_ETHERNET, NULL, 0, NULL);
	link_add (platform, "eth1", NM_LINK_TYPE_ETHERNET, NULL, 0, NULL);
	link_add (platform, "eth2", NM_LINK_TYPE_ETHERNET, NULL, 0, NULL);
}

static void
nm_fake_platform_finalize (GObject *object)
{
	NMFakePlatformPrivate *priv = NM_FAKE_PLATFORM_GET_PRIVATE (object);
	int i;

	g_hash_table_unref (priv->options);
	for (i = 0; i < priv->links->len; i++) {
		NMFakePlatformLink *device = &g_array_index (priv->links, NMFakePlatformLink, i);

		g_free (device->udi);
		g_clear_pointer (&device->lnk, nmp_object_unref);
	}
	g_array_unref (priv->links);
	g_array_unref (priv->ip4_addresses);
	g_array_unref (priv->ip6_addresses);
	g_array_unref (priv->ip4_routes);
	g_array_unref (priv->ip6_routes);

	G_OBJECT_CLASS (nm_fake_platform_parent_class)->finalize (object);
}

static void
nm_fake_platform_class_init (NMFakePlatformClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	NMPlatformClass *platform_class = NM_PLATFORM_CLASS (klass);

	g_type_class_add_private (klass, sizeof (NMFakePlatformPrivate));

	/* virtual methods */
	object_class->finalize = nm_fake_platform_finalize;

	platform_class->sysctl_set = sysctl_set;
	platform_class->sysctl_get = sysctl_get;

	platform_class->link_get = _nm_platform_link_get;
	platform_class->link_get_by_ifname = _nm_platform_link_get_by_ifname;
	platform_class->link_get_by_address = _nm_platform_link_get_by_address;
	platform_class->link_get_all = link_get_all;
	platform_class->link_add = link_add;
	platform_class->link_delete = link_delete;
	platform_class->link_get_type_name = link_get_type_name;

	platform_class->link_get_lnk = link_get_lnk;

	platform_class->link_get_udi = link_get_udi;

	platform_class->link_set_up = link_set_up;
	platform_class->link_set_down = link_set_down;
	platform_class->link_set_arp = link_set_arp;
	platform_class->link_set_noarp = link_set_noarp;

	platform_class->link_set_address = link_set_address;
	platform_class->link_set_mtu = link_set_mtu;

	platform_class->link_get_driver_info = link_get_driver_info;

	platform_class->link_supports_carrier_detect = link_supports_carrier_detect;
	platform_class->link_supports_vlans = link_supports_vlans;

	platform_class->link_enslave = link_enslave;
	platform_class->link_release = link_release;

	platform_class->vlan_add = vlan_add;
	platform_class->link_vlan_change = link_vlan_change;
	platform_class->link_vxlan_add = link_vxlan_add;

	platform_class->infiniband_partition_add = infiniband_partition_add;

	platform_class->wifi_get_capabilities = wifi_get_capabilities;
	platform_class->wifi_get_bssid = wifi_get_bssid;
	platform_class->wifi_get_ssid = wifi_get_ssid;
	platform_class->wifi_get_frequency = wifi_get_frequency;
	platform_class->wifi_get_quality = wifi_get_quality;
	platform_class->wifi_get_rate = wifi_get_rate;
	platform_class->wifi_get_mode = wifi_get_mode;
	platform_class->wifi_set_mode = wifi_set_mode;
	platform_class->wifi_find_frequency = wifi_find_frequency;
	platform_class->wifi_indicate_addressing_running = wifi_indicate_addressing_running;

	platform_class->mesh_get_channel = mesh_get_channel;
	platform_class->mesh_set_channel = mesh_set_channel;
	platform_class->mesh_set_ssid = mesh_set_ssid;

	platform_class->ip4_address_get = ip4_address_get;
	platform_class->ip6_address_get = ip6_address_get;
	platform_class->ip4_address_get_all = ip4_address_get_all;
	platform_class->ip6_address_get_all = ip6_address_get_all;
	platform_class->ip4_address_add = ip4_address_add;
	platform_class->ip6_address_add = ip6_address_add;
	platform_class->ip4_address_delete = ip4_address_delete;
	platform_class->ip6_address_delete = ip6_address_delete;

	platform_class->ip4_route_get = ip4_route_get;
	platform_class->ip6_route_get = ip6_route_get;
	platform_class->ip4_route_get_all = ip4_route_get_all;
	platform_class->ip6_route_get_all = ip6_route_get_all;
	platform_class->ip4_route_add = ip4_route_add;
	platform_class->ip6_route_add = ip6_route_add;
	platform_class->ip4_route_delete = ip4_route_delete;
	platform_class->ip6_route_delete = ip6_route_delete;
}
