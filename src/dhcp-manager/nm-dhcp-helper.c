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
 * Copyright (C) 2007 - 2013 Red Hat, Inc.
 */

#include "nm-default.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#define NM_DHCP_CLIENT_DBUS_IFACE   "org.freedesktop.nm_dhcp_client"

static const char * ignore[] = {"PATH", "SHLVL", "_", "PWD", "dhc_dbus", NULL};

static GVariant *
build_signal_parameters (void)
{
	char **item;
	GVariantBuilder builder;

	g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);

	/* List environment and format for dbus dict */
	for (item = environ; *item; item++) {
		char *name, *val, **p;

		/* Split on the = */
		name = g_strdup (*item);
		val = strchr (name, '=');
		if (!val || val == name)
			goto next;
		*val++ = '\0';

		/* Ignore non-DCHP-related environment variables */
		for (p = (char **) ignore; *p; p++) {
			if (strncmp (name, *p, strlen (*p)) == 0)
				goto next;
		}

		/* Value passed as a byte array rather than a string, because there are
		 * no character encoding guarantees with DHCP, and D-Bus requires
		 * strings to be UTF-8.
		 *
		 * Note that we can't use g_variant_new_bytestring() here, because that
		 * includes the trailing '\0'. (??!?)
		 */
		g_variant_builder_add (&builder, "{sv}",
		                       name,
		                       g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
		                                                  val, strlen (val), 1));

	next:
		g_free (name);
	}

	return g_variant_new ("(a{sv})", &builder);
}

static void
fatal_error (void)
{
	const char *pid_str = getenv ("pid");
	int pid = 0;

	if (pid_str)
		pid = strtol (pid_str, NULL, 10);
	if (pid) {
		g_printerr ("Fatal error occured, killing dhclient instance with pid %d.\n", pid);
		kill (pid, SIGTERM);
	}

	exit (1);
}

int
main (int argc, char *argv[])
{
	GDBusConnection *connection;
	GError *error = NULL;

	nm_g_type_init ();

	connection = g_dbus_connection_new_for_address_sync ("unix:path=" NMRUNDIR "/private-dhcp",
	                                                     G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
	                                                     NULL, NULL, &error);
	if (!connection) {
		g_dbus_error_strip_remote_error (error);
		g_printerr ("Error: could not connect to NetworkManager D-Bus socket: %s\n",
		            error->message);
		g_error_free (error);
		fatal_error ();
	}

	if (!g_dbus_connection_emit_signal (connection,
	                                    NULL,
	                                    "/",
	                                    NM_DHCP_CLIENT_DBUS_IFACE,
	                                    "Event",
	                                    build_signal_parameters (),
	                                    &error)) {
		g_dbus_error_strip_remote_error (error);
		g_printerr ("Error: Could not send DHCP Event signal: %s\n", error->message);
		g_error_free (error);
		fatal_error ();
	}

	if (!g_dbus_connection_flush_sync (connection, NULL, &error)) {
		g_dbus_error_strip_remote_error (error);
		g_printerr ("Error: Could not flush D-Bus connection: %s\n", error->message);
		g_error_free (error);
		fatal_error ();
	}

	g_object_unref (connection);
	return 0;
}

