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
 * Copyright (C) 2008 - 2011 Red Hat, Inc.
 */

#ifndef __NETWORKMANAGER_DISPATCHER_UTILS_H__
#define __NETWORKMANAGER_DISPATCHER_UTILS_H__

#include "nm-default.h"

char **
nm_dispatcher_utils_construct_envp (const char *action,
                                    GVariant *connection_dict,
                                    GVariant *connection_props,
                                    GVariant *device_props,
                                    GVariant *device_ip4_props,
                                    GVariant *device_ip6_props,
                                    GVariant *device_dhcp4_props,
                                    GVariant *device_dhcp6_props,
                                    const char *vpn_ip_iface,
                                    GVariant *vpn_ip4_props,
                                    GVariant *vpn_ip6_props,
                                    char **out_iface,
                                    const char **out_error_message);

#endif  /* __NETWORKMANAGER_DISPATCHER_UTILS_H__ */

