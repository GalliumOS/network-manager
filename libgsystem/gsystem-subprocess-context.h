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

#ifndef __GSYSTEM_SUBPROCESS_CONTEXT_H__
#define __GSYSTEM_SUBPROCESS_CONTEXT_H__

#include <gio/gio.h>

#if GLIB_CHECK_VERSION(2,34,0)

G_BEGIN_DECLS

#define GS_TYPE_SUBPROCESS_CONTEXT         (gs_subprocess_context_get_type ())
#define GS_SUBPROCESS_CONTEXT(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), GS_TYPE_SUBPROCESS_CONTEXT, GSSubprocessContext))
#define GS_IS_SUBPROCESS_CONTEXT(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), GS_TYPE_SUBPROCESS_CONTEXT))

typedef struct _GSSubprocessContext GSSubprocessContext;

/**
 * GSSubprocessStreamDisposition:
 * @GS_SUBPROCESS_STREAM_DISPOSITION_NULL: Redirect to operating system's null output stream
 * @GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT: Keep the stream from the parent process
 * @GS_SUBPROCESS_STREAM_DISPOSITION_PIPE: Open a private unidirectional channel between the processes
 * @GS_SUBPROCESS_STREAM_DISPOSITION_STDERR_MERGE: Only applicable to standard error; causes it to be merged with standard output
 *
 * Flags to define the behaviour of the standard input/output/error of
 * a #GSSubprocess.
 *
 * Since: 2.36
 **/
typedef enum {
  GS_SUBPROCESS_STREAM_DISPOSITION_NULL,
  GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT,
  GS_SUBPROCESS_STREAM_DISPOSITION_PIPE,
  GS_SUBPROCESS_STREAM_DISPOSITION_STDERR_MERGE
} GSSubprocessStreamDisposition;

GType            gs_subprocess_context_get_type (void) G_GNUC_CONST;

GSSubprocessContext * gs_subprocess_context_new (gchar           **argv);
GSSubprocessContext * gs_subprocess_context_newv (const gchar  *first_arg,
                                                ...);
GSSubprocessContext * gs_subprocess_context_newa (const gchar  *first_arg,
                                                va_list       args);

#ifdef G_OS_UNIX
GSSubprocessContext * gs_subprocess_context_new_argv0 (const gchar   *argv0,
                                                     gchar        **argv);
#endif

void             gs_subprocess_context_argv_append (GSSubprocessContext  *self,
                                                    const gchar          *arg);

/* Environment */

void             gs_subprocess_context_set_environment (GSSubprocessContext           *self,
						       gchar                       **environ);
void             gs_subprocess_context_set_cwd (GSSubprocessContext           *self,
					       const gchar                  *cwd);
void             gs_subprocess_context_set_keep_descriptors (GSSubprocessContext           *self,
							    gboolean                      keep_descriptors);
void             gs_subprocess_context_set_search_path (GSSubprocessContext           *self,
						       gboolean                      search_path,
						       gboolean                      search_path_from_envp);

/* Basic I/O control */

void             gs_subprocess_context_set_stdin_disposition (GSSubprocessContext           *self,
							     GSSubprocessStreamDisposition  disposition);
void             gs_subprocess_context_set_stdout_disposition (GSSubprocessContext           *self,
							      GSSubprocessStreamDisposition  disposition);
void             gs_subprocess_context_set_stderr_disposition (GSSubprocessContext           *self,
							      GSSubprocessStreamDisposition  disposition);

/* Extended I/O control, only available on UNIX */

#ifdef G_OS_UNIX
void             gs_subprocess_context_set_stdin_file_path (GSSubprocessContext           *self,
							   const gchar                  *path);
void             gs_subprocess_context_set_stdin_fd        (GSSubprocessContext           *self,
							   gint                          fd);
void             gs_subprocess_context_set_stdout_file_path (GSSubprocessContext           *self,
							    const gchar                  *path);
void             gs_subprocess_context_set_stdout_fd        (GSSubprocessContext           *self,
							    gint                          fd);
void             gs_subprocess_context_set_stderr_file_path (GSSubprocessContext           *self,
							    const gchar                  *path);
void             gs_subprocess_context_set_stderr_fd        (GSSubprocessContext           *self,
							    gint                          fd);

gboolean         gs_subprocess_context_open_pipe_read       (GSSubprocessContext         *self,
                                                             GInputStream               **out_stream,
                                                             gint                        *out_fdno,
                                                             GError                     **error);
gboolean         gs_subprocess_context_open_pipe_write      (GSSubprocessContext         *self,
                                                             GOutputStream              **out_stream,
                                                             gint                        *out_fdno,
                                                             GError                     **error);
#endif

/* Child setup, only available on UNIX */
#ifdef G_OS_UNIX
void             gs_subprocess_context_set_child_setup        (GSSubprocessContext           *self,
							      GSpawnChildSetupFunc          child_setup,
							      gpointer                      user_data);
#endif

G_END_DECLS

#endif
#endif
