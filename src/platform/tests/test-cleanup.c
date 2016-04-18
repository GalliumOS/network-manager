/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager audit support
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
 * Copyright 2016 Red Hat, Inc.
 */

#include "nm-default.h"

#include "test-common.h"

#define DEVICE_NAME "nm-test-device"

static void
test_cleanup_internal (void)
{
	SignalData *link_added = add_signal_ifname (NM_PLATFORM_SIGNAL_LINK_CHANGED, NM_PLATFORM_SIGNAL_ADDED, link_callback, DEVICE_NAME);
	int ifindex;
	GArray *addresses4;
	GArray *addresses6;
	GArray *routes4;
	GArray *routes6;
	in_addr_t addr4;
	in_addr_t network4;
	int plen4 = 24;
	in_addr_t gateway4;
	struct in6_addr addr6;
	struct in6_addr network6;
	int plen6 = 64;
	struct in6_addr gateway6;
	int lifetime = NM_PLATFORM_LIFETIME_PERMANENT;
	int preferred = NM_PLATFORM_LIFETIME_PERMANENT;
	int metric = 20;
	int mss = 1000;
	guint32 flags = 0;

	inet_pton (AF_INET, "192.0.2.1", &addr4);
	inet_pton (AF_INET, "192.0.3.0", &network4);
	inet_pton (AF_INET, "198.51.100.1", &gateway4);
	inet_pton (AF_INET6, "2001:db8:a:b:1:2:3:4", &addr6);
	inet_pton (AF_INET6, "2001:db8:c:d:0:0:0:0", &network6);
	inet_pton (AF_INET6, "2001:db8:e:f:1:2:3:4", &gateway6);

	/* Create and set up device */
	g_assert (nm_platform_link_dummy_add (NM_PLATFORM_GET, DEVICE_NAME, NULL) == NM_PLATFORM_ERROR_SUCCESS);
	accept_signal (link_added);
	free_signal (link_added);
	g_assert (nm_platform_link_set_up (NM_PLATFORM_GET, nm_platform_link_get_ifindex (NM_PLATFORM_GET, DEVICE_NAME), NULL));
	ifindex = nm_platform_link_get_ifindex (NM_PLATFORM_GET, DEVICE_NAME);
	g_assert (ifindex > 0);

	/* Add routes and addresses */
	g_assert (nm_platform_ip4_address_add (NM_PLATFORM_GET, ifindex, addr4, plen4, addr4, lifetime, preferred, 0, NULL));
	g_assert (nm_platform_ip6_address_add (NM_PLATFORM_GET, ifindex, addr6, plen6, in6addr_any, lifetime, preferred, flags));
	g_assert (nm_platform_ip4_route_add (NM_PLATFORM_GET, ifindex, NM_IP_CONFIG_SOURCE_USER, gateway4, 32, INADDR_ANY, 0, metric, mss));
	g_assert (nm_platform_ip4_route_add (NM_PLATFORM_GET, ifindex, NM_IP_CONFIG_SOURCE_USER, network4, plen4, gateway4, 0, metric, mss));
	g_assert (nm_platform_ip4_route_add (NM_PLATFORM_GET, ifindex, NM_IP_CONFIG_SOURCE_USER, 0, 0, gateway4, 0, metric, mss));
	g_assert (nm_platform_ip6_route_add (NM_PLATFORM_GET, ifindex, NM_IP_CONFIG_SOURCE_USER, gateway6, 128, in6addr_any, metric, mss));
	g_assert (nm_platform_ip6_route_add (NM_PLATFORM_GET, ifindex, NM_IP_CONFIG_SOURCE_USER, network6, plen6, gateway6, metric, mss));
	g_assert (nm_platform_ip6_route_add (NM_PLATFORM_GET, ifindex, NM_IP_CONFIG_SOURCE_USER, in6addr_any, 0, gateway6, metric, mss));

	addresses4 = nm_platform_ip4_address_get_all (NM_PLATFORM_GET, ifindex);
	addresses6 = nm_platform_ip6_address_get_all (NM_PLATFORM_GET, ifindex);
	routes4 = nm_platform_ip4_route_get_all (NM_PLATFORM_GET, ifindex, NM_PLATFORM_GET_ROUTE_FLAGS_WITH_DEFAULT | NM_PLATFORM_GET_ROUTE_FLAGS_WITH_NON_DEFAULT);
	routes6 = nm_platform_ip6_route_get_all (NM_PLATFORM_GET, ifindex, NM_PLATFORM_GET_ROUTE_FLAGS_WITH_DEFAULT | NM_PLATFORM_GET_ROUTE_FLAGS_WITH_NON_DEFAULT);

	g_assert_cmpint (addresses4->len, ==, 1);
	g_assert_cmpint (addresses6->len, ==, 2); /* also has a IPv6 LL address. */
	g_assert_cmpint (routes4->len, ==, 3);
	g_assert_cmpint (routes6->len, ==, 3);

	g_array_unref (addresses4);
	g_array_unref (addresses6);
	g_array_unref (routes4);
	g_array_unref (routes6);

	/* Delete interface with all addresses and routes */
	g_assert (nm_platform_link_delete (NM_PLATFORM_GET, ifindex));

	addresses4 = nm_platform_ip4_address_get_all (NM_PLATFORM_GET, ifindex);
	addresses6 = nm_platform_ip6_address_get_all (NM_PLATFORM_GET, ifindex);
	routes4 = nm_platform_ip4_route_get_all (NM_PLATFORM_GET, ifindex, NM_PLATFORM_GET_ROUTE_FLAGS_WITH_DEFAULT | NM_PLATFORM_GET_ROUTE_FLAGS_WITH_NON_DEFAULT);
	routes6 = nm_platform_ip6_route_get_all (NM_PLATFORM_GET, ifindex, NM_PLATFORM_GET_ROUTE_FLAGS_WITH_DEFAULT | NM_PLATFORM_GET_ROUTE_FLAGS_WITH_NON_DEFAULT);

	g_assert_cmpint (addresses4->len, ==, 0);
	g_assert_cmpint (addresses6->len, ==, 0);
	g_assert_cmpint (routes4->len, ==, 0);
	g_assert_cmpint (routes6->len, ==, 0);

	g_array_unref (addresses4);
	g_array_unref (addresses6);
	g_array_unref (routes4);
	g_array_unref (routes6);
}

void
init_tests (int *argc, char ***argv)
{
	nmtst_init_with_logging (argc, argv, NULL, "ALL");
}

void
setup_tests (void)
{
	nm_platform_link_delete (NM_PLATFORM_GET, nm_platform_link_get_ifindex (NM_PLATFORM_GET, DEVICE_NAME));
	g_assert (!nm_platform_link_get_by_ifname (NM_PLATFORM_GET, DEVICE_NAME));

	g_test_add_func ("/internal", test_cleanup_internal);
	/* FIXME: add external cleanup check */
}
