/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef __GSYSTEM_CONSOLE_H__
#define __GSYSTEM_CONSOLE_H__

#include <gio/gio.h>

G_BEGIN_DECLS

#define GS_TYPE_CONSOLE         (gs_console_get_type ())
#define GS_CONSOLE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), GS_TYPE_CONSOLE, GSConsole))
#define GS_IS_CONSOLE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), GS_TYPE_CONSOLE))

typedef struct _GSConsole GSConsole;

GType            gs_console_get_type (void) G_GNUC_CONST;

GInputStream *   gs_console_get_stdin (void);
GOutputStream *  gs_console_get_stdout (void);
GOutputStream *  gs_console_get_stderr (void);

GSConsole *      gs_console_get (void);

char *           gs_console_read_password (GSConsole     *console,
                                           const char    *prompt,
                                           GCancellable  *cancellable,
                                           GError       **error);

gboolean         gs_console_begin_status_line (GSConsole     *console,
                                               const char    *line,
                                               GCancellable  *cancellable,
                                               GError       **error);

gboolean         gs_console_end_status_line (GSConsole      *console,
                                             GCancellable   *cancellable,
                                             GError        **error);


G_END_DECLS

#endif
