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
 * Copyright (C) 2007 - 2008 Novell, Inc.
 * Copyright (C) 2007 - 2013 Red Hat, Inc.
 */

#include <glib-object.h>

#ifndef NM_RFKILL_MANAGER_H
#define NM_RFKILL_MANAGER_H

typedef enum {
	RFKILL_UNBLOCKED = 0,
	RFKILL_SOFT_BLOCKED = 1,
	RFKILL_HARD_BLOCKED = 2
} RfKillState;

typedef enum {
	RFKILL_TYPE_WLAN = 0,
	RFKILL_TYPE_WWAN = 1,
	RFKILL_TYPE_WIMAX = 2,

	/* UNKNOWN and MAX should always be 1 more than
	 * the last rfkill type since RFKILL_TYPE_MAX is
	 * used as an array size.
	 */
	RFKILL_TYPE_UNKNOWN = 3, /* KEEP LAST */
	RFKILL_TYPE_MAX = RFKILL_TYPE_UNKNOWN
} RfKillType;


#define NM_TYPE_RFKILL_MANAGER            (nm_rfkill_manager_get_type ())
#define NM_RFKILL_MANAGER(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), NM_TYPE_RFKILL_MANAGER, NMRfkillManager))
#define NM_RFKILL_MANAGER_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), NM_TYPE_RFKILL_MANAGER, NMRfkillManagerClass))
#define NM_IS_RFKILL_MANAGER(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), NM_TYPE_RFKILL_MANAGER))
#define NM_IS_RFKILL_MANAGER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), NM_TYPE_RFKILL_MANAGER))
#define NM_RFKILL_MANAGER_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), NM_TYPE_RFKILL_MANAGER, NMRfkillManagerClass))

typedef struct {
	GObject parent;
} NMRfkillManager;

typedef struct {
	GObjectClass parent;

	/* signals */
	void (*rfkill_changed) (NMRfkillManager *manager, RfKillType rtype, RfKillState state);
} NMRfkillManagerClass;

GType nm_rfkill_manager_get_type (void);

NMRfkillManager *nm_rfkill_manager_new (void);

RfKillState nm_rfkill_manager_get_rfkill_state (NMRfkillManager *manager, RfKillType rtype);

#endif  /* NM_RFKILL_MANAGER_H */

