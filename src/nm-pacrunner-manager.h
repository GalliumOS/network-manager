/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager
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
 * Copyright 2016 Atul Anand <atulhjp@gmail.com>.
 * Copyright 2016 - 2017 Red Hat, Inc.
 */

#ifndef __NETWORKMANAGER_PACRUNNER_MANAGER_H__
#define __NETWORKMANAGER_PACRUNNER_MANAGER_H__

#define NM_TYPE_PACRUNNER_MANAGER            (nm_pacrunner_manager_get_type ())
#define NM_PACRUNNER_MANAGER(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), NM_TYPE_PACRUNNER_MANAGER, NMPacrunnerManager))
#define NM_PACRUNNER_MANAGER_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), NM_TYPE_PACRUNNER_MANAGER, NMPacrunnerManagerClass))
#define NM_IS_PACRUNNER_MANAGER(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), NM_TYPE_PACRUNNER_MANAGER))
#define NM_IS_PACRUNNER_MANAGER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), NM_TYPE_PACRUNNER_MANAGER))
#define NM_PACRUNNER_MANAGER_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), NM_TYPE_PACRUNNER_MANAGER, NMPacrunnerManagerClass))

typedef struct _NMPacrunnerManagerClass NMPacrunnerManagerClass;

typedef struct _NMPacrunnerCallId NMPacrunnerCallId;

GType nm_pacrunner_manager_get_type (void);

NMPacrunnerManager *nm_pacrunner_manager_get (void);

NMPacrunnerCallId *nm_pacrunner_manager_send (NMPacrunnerManager *self,
                                              const char *iface,
                                              NMProxyConfig *proxy_config,
                                              NMIP4Config *ip4_config,
                                              NMIP6Config *ip6_config);

void nm_pacrunner_manager_remove (NMPacrunnerManager *self,
                                  NMPacrunnerCallId *call_id);

gboolean nm_pacrunner_manager_remove_clear (NMPacrunnerManager *self,
                                            NMPacrunnerCallId **p_call_id);

#endif /* __NETWORKMANAGER_PACRUNNER_MANAGER_H__ */
