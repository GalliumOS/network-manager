/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright 2013 Red Hat, Inc.
 */

#ifndef NMT_PAGE_GRID_H
#define NMT_PAGE_GRID_H

#include "nmt-newt.h"

G_BEGIN_DECLS

#define NMT_TYPE_PAGE_GRID            (nmt_page_grid_get_type ())
#define NMT_PAGE_GRID(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), NMT_TYPE_PAGE_GRID, NmtPageGrid))
#define NMT_PAGE_GRID_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), NMT_TYPE_PAGE_GRID, NmtPageGridClass))
#define NMT_IS_PAGE_GRID(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), NMT_TYPE_PAGE_GRID))
#define NMT_IS_PAGE_GRID_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), NMT_TYPE_PAGE_GRID))
#define NMT_PAGE_GRID_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), NMT_TYPE_PAGE_GRID, NmtPageGridClass))

typedef struct {
	NmtNewtContainer parent;

} NmtPageGrid;

typedef struct {
	NmtNewtContainerClass parent;

} NmtPageGridClass;

GType nmt_page_grid_get_type (void);

typedef enum {
	NMT_PAGE_GRID_ROW_LABEL_ALIGN_LEFT = (1 << 0),
	NMT_PAGE_GRID_ROW_EXTRA_ALIGN_RIGHT = (1 << 1)
} NmtPageGridRowFlags;

NmtNewtWidget *nmt_page_grid_new              (void);

void           nmt_page_grid_append           (NmtPageGrid         *grid,
                                               const char          *label,
                                               NmtNewtWidget       *widget,
                                               NmtNewtWidget       *extra);
void           nmt_page_grid_set_row_flags    (NmtPageGrid         *grid,
                                               NmtNewtWidget       *widget,
                                               NmtPageGridRowFlags  flags);

G_END_DECLS

#endif /* NMT_PAGE_GRID_H */
