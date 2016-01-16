/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012 Colin Walters <walters@verbum.org>.
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

#ifndef __GSYSTEM_SUBPROCESS_H__
#define __GSYSTEM_SUBPROCESS_H__

#include <gio/gio.h>

#if GLIB_CHECK_VERSION(2,34,0)

#include "gsystem-subprocess-context.h"

G_BEGIN_DECLS

#define GS_TYPE_SUBPROCESS         (gs_subprocess_get_type ())
#define GS_SUBPROCESS(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), GS_TYPE_SUBPROCESS, GSSubprocess))
#define GS_IS_SUBPROCESS(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), GS_TYPE_SUBPROCESS))

typedef struct _GSSubprocess GSSubprocess;

GType            gs_subprocess_get_type (void) G_GNUC_CONST;

/**** Core API ****/

GSSubprocess *    gs_subprocess_new (GSSubprocessContext   *context,
                                     GCancellable          *cancellable,
                                     GError               **error);

GOutputStream *  gs_subprocess_get_stdin_pipe (GSSubprocess       *self);

GInputStream *   gs_subprocess_get_stdout_pipe (GSSubprocess      *self);

GInputStream *   gs_subprocess_get_stderr_pipe (GSSubprocess      *self);

void             gs_subprocess_wait (GSSubprocess                *self,
				    GCancellable               *cancellable,
				    GAsyncReadyCallback         callback,
				    gpointer                    user_data);

gboolean         gs_subprocess_wait_finish (GSSubprocess                *self,
					   GAsyncResult               *result,
					   int                        *out_exit_status,
					   GError                    **error);

gboolean         gs_subprocess_wait_sync (GSSubprocess   *self,
					 int           *out_exit_status,
					 GCancellable  *cancellable,
					 GError       **error);

gboolean         gs_subprocess_wait_sync_check (GSSubprocess   *self,
					       GCancellable  *cancellable,
					       GError       **error);

GPid             gs_subprocess_get_pid (GSSubprocess     *self);

gboolean         gs_subprocess_request_exit (GSSubprocess       *self);

void             gs_subprocess_force_exit (GSSubprocess       *self);

/**** High level helpers ****/

GSSubprocess *    gs_subprocess_new_simple_argl (GSSubprocessStreamDisposition   stdout_disposition,
                                                 GSSubprocessStreamDisposition   stderr_disposition,
                                                 GCancellable                   *cancellable,
                                                 GError                        **error,
                                                 const char                     *first_arg,
                                                 ...) G_GNUC_NULL_TERMINATED;
GSSubprocess *    gs_subprocess_new_simple_argv (char                         **argv,
                                                 GSSubprocessStreamDisposition  stdout_disposition,
                                                 GSSubprocessStreamDisposition  stderr_disposition,
                                                 GCancellable                  *cancellable,
                                                 GError                       **error);

gboolean          gs_subprocess_simple_run_sync (const char                    *cwd,
                                                 GSSubprocessStreamDisposition  stdin_disposition,
                                                 GCancellable                  *cancellable,
                                                 GError                       **error,
                                                 const char                    *first_arg,
                                                 ...) G_GNUC_NULL_TERMINATED;

G_END_DECLS

#endif
#endif
