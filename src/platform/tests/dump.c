#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>

#include "nm-platform.h"
#include "nm-linux-platform.h"
#include "nm-fake-platform.h"

static void
dump_interface (NMPlatformLink *link)
{
	GArray *ip6_addresses;
	GArray *ip4_addresses;
	const NMPlatformIP6Address *ip6_address;
	const NMPlatformIP4Address *ip4_address;
	GArray *ip6_routes;
	GArray *ip4_routes;
	const NMPlatformIP6Route *ip6_route;
	const NMPlatformIP4Route *ip4_route;
	char networkstr[INET6_ADDRSTRLEN];
	char gatewaystr[INET6_ADDRSTRLEN];
	int vlan_id, vlan_parent;
	const char *address;
	size_t addrlen;
	int i;

	g_assert (link->up || !link->connected);

	printf ("%d: %s: %s", link->ifindex, link->name, link->type_name);
	if (link->up)
		printf (" %s", link->connected ? "CONNECTED" : "DISCONNECTED");
	else
		printf (" DOWN");
	if (!link->arp)
		printf (" noarp");
	if (link->master)
		printf (" master %d", link->master);
	if (link->parent)
		printf (" parent %d", link->parent);
	printf (" mtu %d", link->mtu);
	printf ("\n");
	if (link->driver)
		printf ("    driver: %s\n", link->driver);
	printf ("    UDI: %s\n", link->udi);
	if (!nm_platform_vlan_get_info (link->ifindex, &vlan_parent, &vlan_id))
		g_assert_not_reached ();
	if (vlan_parent)
		printf ("    vlan parent %d id %d\n", vlan_parent, vlan_id);

	if (nm_platform_link_is_software (link->ifindex))
		printf ("    class software\n");
	if (nm_platform_link_supports_slaves (link->ifindex))
		printf ("    class supports-slaves\n");
	if (nm_platform_link_supports_carrier_detect (link->ifindex))
		printf ("    feature carrier-detect\n");
	if (nm_platform_link_supports_vlans (link->ifindex))
		printf ("    feature vlans\n");

	address = nm_platform_link_get_address (link->ifindex, &addrlen);
	if (address) {
		printf ("    link-address ");
		for (i = 0; i < addrlen; i++)
			printf ("%s%02hhx", i ? ":" : "", address[i]);
		printf ("\n");
	}

	ip4_addresses = nm_platform_ip4_address_get_all (link->ifindex);
	ip6_addresses = nm_platform_ip6_address_get_all (link->ifindex);

	g_assert (ip4_addresses);
	g_assert (ip6_addresses);

	for (i = 0; i < ip4_addresses->len; i++) {
		ip4_address = &g_array_index (ip4_addresses, NMPlatformIP4Address, i);
		printf ("    ip4-address %s\n", nm_platform_ip4_address_to_string (ip4_address));
	}

	for (i = 0; i < ip6_addresses->len; i++) {
		ip6_address = &g_array_index (ip6_addresses, NMPlatformIP6Address, i);
		printf ("    ip6-address %s\n", nm_platform_ip6_address_to_string (ip6_address));
	}

	g_array_unref (ip4_addresses);
	g_array_unref (ip6_addresses);

	ip4_routes = nm_platform_ip4_route_get_all (link->ifindex, TRUE);
	ip6_routes = nm_platform_ip6_route_get_all (link->ifindex, TRUE);

	g_assert (ip4_routes);
	g_assert (ip6_routes);

	for (i = 0; i < ip4_routes->len; i++) {
		ip4_route = &g_array_index (ip4_routes, NMPlatformIP4Route, i);
		inet_ntop (AF_INET, &ip4_route->network, networkstr, sizeof (networkstr));
		inet_ntop (AF_INET, &ip4_route->gateway, gatewaystr, sizeof (gatewaystr));
		printf ("    ip4-route %s/%d via %s\n", networkstr, ip4_route->plen, gatewaystr);
	}

	for (i = 0; i < ip6_routes->len; i++) {
		ip6_route = &g_array_index (ip6_routes, NMPlatformIP6Route, i);
		inet_ntop (AF_INET6, &ip6_route->network, networkstr, sizeof (networkstr));
		inet_ntop (AF_INET6, &ip6_route->gateway, gatewaystr, sizeof (gatewaystr));
		printf ("    ip6-route %s/%d via %s\n", networkstr, ip6_route->plen, gatewaystr);
	}

	g_array_unref (ip4_routes);
	g_array_unref (ip6_routes);
}

static void
dump_all (void)
{
	GArray *links = nm_platform_link_get_all ();
	int i;

	for (i = 0; i < links->len; i++)
		dump_interface (&g_array_index (links, NMPlatformLink, i));
}

int
main (int argc, char **argv)
{
#if !GLIB_CHECK_VERSION (2, 35, 0)
	g_type_init ();
#endif

	g_assert (argc <= 2);
	if (argc > 1 && !g_strcmp0 (argv[1], "--fake"))
		nm_fake_platform_setup ();
	else
		nm_linux_platform_setup ();

	dump_all ();

	return EXIT_SUCCESS;
}
