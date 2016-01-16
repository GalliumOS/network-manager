/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012 Colin Walters <walters@verbum.org>
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

#include "config.h"

#include "libgsystem.h"

#if GLIB_CHECK_VERSION(2,34,0)

#ifdef G_OS_UNIX
#include <gio/gunixoutputstream.h>
#include <gio/gfiledescriptorbased.h>
#include <gio/gunixinputstream.h>
#include <glib-unix.h>
#endif

/**
 * SECTION:gssubprocesscontext
 * @title: GSSubprocess Context
 * @short_description: Environment options for launching a child process
 *
 * This class contains a set of options for launching child processes,
 * such as where its standard input and output will be directed, the
 * argument list, the environment, and more.
 *
 * While the #GSSubprocess class has high level functions covering
 * popular cases, use of this class allows access to more advanced
 * options.  It can also be used to launch multiple subprocesses with
 * a similar configuration.
 *
 * Since: 2.36
 */

#include "config.h"

#include "gsystem-subprocess-context-private.h"
#include "gsystem-subprocess.h"

#include <string.h>

typedef GObjectClass GSSubprocessContextClass;

G_DEFINE_TYPE (GSSubprocessContext, gs_subprocess_context, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_ARGV,
  N_PROPS
};

static GParamSpec *gs_subprocess_context_pspecs[N_PROPS];

/**
 * gs_subprocess_context_new:
 * @argv: Argument list
 *
 * Returns: (transfer full): A new instance of a #GSSubprocessContext.
 */
GSSubprocessContext *
gs_subprocess_context_new (gchar           **argv)
{
  g_return_val_if_fail (argv != NULL && argv[0] != NULL, NULL);

  return g_object_new (GS_TYPE_SUBPROCESS_CONTEXT,
		       "argv", argv,
		       NULL);
}

GSSubprocessContext *
gs_subprocess_context_newv (const gchar  *first_arg,
                           ...)
{
  GSSubprocessContext *result;
  va_list args;

  g_return_val_if_fail (first_arg != NULL, NULL);

  va_start (args, first_arg);
  result = gs_subprocess_context_newa (first_arg, args);
  va_end (args);
  
  return result;
}

/**
 * gs_subprocess_context_newa:
 * @first_arg: First argument
 * @args: a va_list
 *
 * Returns: (transfer full): A new instance of a #GSSubprocessContext.
 */
GSSubprocessContext *
gs_subprocess_context_newa (const gchar *first_arg,
                           va_list      args)
{
  GSSubprocessContext *result;
  GPtrArray *argv;

  g_return_val_if_fail (first_arg != NULL, NULL);

  argv = g_ptr_array_new ();
  do
    g_ptr_array_add (argv, (gchar*)first_arg);
  while ((first_arg = va_arg (args, const gchar *)) != NULL);
  g_ptr_array_add (argv, NULL);

  result = gs_subprocess_context_new ((gchar**)argv->pdata);
  
  return result;
}

#ifdef G_OS_UNIX
GSSubprocessContext *
gs_subprocess_context_new_argv0 (const gchar      *argv0,
                                gchar           **argv)
{
  GSSubprocessContext *result;
  GPtrArray *real_argv;
  gchar **iter;
  
  g_return_val_if_fail (argv0 != NULL, NULL);
  g_return_val_if_fail (argv != NULL && argv[0] != NULL, NULL);
  
  real_argv = g_ptr_array_new ();
  g_ptr_array_add (real_argv, (gchar*)argv0);
  for (iter = argv; *iter; iter++)
    g_ptr_array_add (real_argv, (gchar*) *iter);
  g_ptr_array_add (real_argv, NULL);

  result = g_object_new (GS_TYPE_SUBPROCESS_CONTEXT,
                         "argv", real_argv->pdata,
                         NULL);
  result->has_argv0 = TRUE;

  return result;
}
#endif

static void
gs_subprocess_context_init (GSSubprocessContext  *self)
{
  self->stdin_fd = -1;
  self->stdout_fd = -1;
  self->stderr_fd = -1;
  self->stdout_disposition = GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT;
  self->stderr_disposition = GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT;
  self->postfork_close_fds = g_array_new (FALSE, FALSE, sizeof (int));
  self->inherit_fds = g_array_new (FALSE, FALSE, sizeof (int));
}

static void
gs_subprocess_context_finalize (GObject *object)
{
  GSSubprocessContext *self = GS_SUBPROCESS_CONTEXT (object);

  g_strfreev (self->argv);
  g_strfreev (self->envp);
  g_free (self->cwd);

  g_free (self->stdin_path);
  g_free (self->stdout_path);
  g_free (self->stderr_path);

  g_array_unref (self->postfork_close_fds);
  g_array_unref (self->inherit_fds);

  if (G_OBJECT_CLASS (gs_subprocess_context_parent_class)->finalize != NULL)
    G_OBJECT_CLASS (gs_subprocess_context_parent_class)->finalize (object);
}

static void
gs_subprocess_context_set_property (GObject      *object,
				   guint         prop_id,
				   const GValue *value,
				   GParamSpec   *pspec)
{
  GSSubprocessContext *self = GS_SUBPROCESS_CONTEXT (object);

  switch (prop_id)
    {
    case PROP_ARGV:
      self->argv = (gchar**) g_value_dup_boxed (value);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
gs_subprocess_context_get_property (GObject    *object,
				   guint       prop_id,
				   GValue     *value,
				   GParamSpec *pspec)
{
  GSSubprocessContext *self = GS_SUBPROCESS_CONTEXT (object);

  switch (prop_id)
    {
    case PROP_ARGV:
      g_value_set_boxed (value, self->argv);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
gs_subprocess_context_class_init (GSSubprocessContextClass *class)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (class);

  gobject_class->finalize = gs_subprocess_context_finalize;
  gobject_class->get_property = gs_subprocess_context_get_property;
  gobject_class->set_property = gs_subprocess_context_set_property;

  /**
   * GSSubprocessContext:argv:
   *
   * Array of arguments passed to child process; must have at least
   * one element.  The first element has special handling - if it is
   * an not absolute path ( as determined by g_path_is_absolute() ),
   * then the system search path will be used.  See
   * %G_SPAWN_SEARCH_PATH.
   * 
   * Note that in order to use the Unix-specific argv0 functionality,
   * you must use the setter function
   * gs_subprocess_context_set_args_and_argv0().  For more information
   * about this, see %G_SPAWN_FILE_AND_ARGV_ZERO.
   *
   * Since: 2.36
   */
  gs_subprocess_context_pspecs[PROP_ARGV] = g_param_spec_boxed ("argv", "Arguments", "Arguments for child process", G_TYPE_STRV,
							       G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, N_PROPS, gs_subprocess_context_pspecs);
}

/**
 * gs_subprocess_context_argv_append:
 * @self:
 * @arg: An argument
 *
 * Append an argument to the child's argument vector.
 */
void
gs_subprocess_context_argv_append (GSSubprocessContext  *self,
                                   const gchar          *arg)
{
  GPtrArray *new_argv = g_ptr_array_new ();
  gchar **iter;

  for (iter = self->argv; *iter; iter++)
    g_ptr_array_add (new_argv, *iter);
  g_ptr_array_add (new_argv, g_strdup (arg));
  g_ptr_array_add (new_argv, NULL);

  /* Don't free elements */
  g_free (self->argv);
  self->argv = (char**)g_ptr_array_free (new_argv, FALSE);
}

/* Environment */

/**
 * gs_subprocess_context_set_environment:
 * @self:
 * @environ: (array zero-terminated=1) (element-type utf8): Environment KEY=VALUE pairs
 *
 * Replace the environment that will be used for the child process.
 * The default is to inherit the current process.
 */
void
gs_subprocess_context_set_environment (GSSubprocessContext           *self,
                                       gchar                        **env)
{
  g_strfreev (self->envp);
  self->envp = g_strdupv (env);
}

void
gs_subprocess_context_set_cwd (GSSubprocessContext           *self,
			      const gchar                  *cwd)
{
  g_free (self->cwd);
  self->cwd = g_strdup (cwd);
}

void
gs_subprocess_context_set_keep_descriptors (GSSubprocessContext           *self,
					   gboolean                      keep_descriptors)

{
  self->keep_descriptors = keep_descriptors ? 1 : 0;
}

void
gs_subprocess_context_set_search_path (GSSubprocessContext           *self,
				      gboolean                      search_path,
				      gboolean                      search_path_from_envp)
{
  self->search_path = search_path ? 1 : 0;
  self->search_path_from_envp = search_path_from_envp ? 1 : 0;
}

void
gs_subprocess_context_set_stdin_disposition (GSSubprocessContext           *self,
					    GSSubprocessStreamDisposition  disposition)
{
  g_return_if_fail (disposition != GS_SUBPROCESS_STREAM_DISPOSITION_STDERR_MERGE);
  self->stdin_disposition = disposition;
}

void
gs_subprocess_context_set_stdout_disposition (GSSubprocessContext           *self,
					     GSSubprocessStreamDisposition  disposition)
{
  g_return_if_fail (disposition != GS_SUBPROCESS_STREAM_DISPOSITION_STDERR_MERGE);
  self->stdout_disposition = disposition;
}

void
gs_subprocess_context_set_stderr_disposition (GSSubprocessContext           *self,
					     GSSubprocessStreamDisposition  disposition)
{
  self->stderr_disposition = disposition;
}

#ifdef G_OS_UNIX
void
gs_subprocess_context_set_stdin_file_path (GSSubprocessContext           *self,
					  const gchar                  *path)
{
  self->stdin_disposition = GS_SUBPROCESS_STREAM_DISPOSITION_NULL;
  g_free (self->stdin_path);
  self->stdin_path = g_strdup (path);
}

void
gs_subprocess_context_set_stdin_fd        (GSSubprocessContext           *self,
					  gint                          fd)
{
  self->stdin_disposition = GS_SUBPROCESS_STREAM_DISPOSITION_NULL;
  self->stdin_fd = fd;
}

void
gs_subprocess_context_set_stdout_file_path (GSSubprocessContext           *self,
					   const gchar                  *path)
{
  self->stdout_disposition = GS_SUBPROCESS_STREAM_DISPOSITION_NULL;
  g_free (self->stdout_path);
  self->stdout_path = g_strdup (path);
}

void
gs_subprocess_context_set_stdout_fd (GSSubprocessContext           *self,
				    gint                          fd)
{
  self->stdout_disposition = GS_SUBPROCESS_STREAM_DISPOSITION_NULL;
  self->stdout_fd = fd;
}

void
gs_subprocess_context_set_stderr_file_path (GSSubprocessContext           *self,
					   const gchar                  *path)
{
  self->stderr_disposition = GS_SUBPROCESS_STREAM_DISPOSITION_NULL;
  g_free (self->stderr_path);
  self->stderr_path = g_strdup (path);
}

void
gs_subprocess_context_set_stderr_fd        (GSSubprocessContext           *self,
					   gint                          fd)
{
  self->stderr_disposition = GS_SUBPROCESS_STREAM_DISPOSITION_NULL;
  self->stderr_fd = fd;
}
#endif

#ifdef G_OS_UNIX
/**
 * gs_subprocess_context_set_child_setup: (skip)
 * @self:
 * @child_setup: Function to call in the newly forked child, before execve()
 * @user_data: Data passed to child
 *
 * FIXME - note extensive restricitons on GSpawnChildSetupFunc here
 */
void
gs_subprocess_context_set_child_setup (GSSubprocessContext           *self,
				      GSpawnChildSetupFunc          child_setup,
				      gpointer                      user_data)
{
  self->child_setup_func = child_setup;
  self->child_setup_data = user_data;
}

static gboolean
open_pipe_internal (GSSubprocessContext         *self,
                    gboolean                     for_read,
                    void                       **out_stream,
                    gint                        *out_fdno,
                    GError                     **error)
{
  int pipefds[2];

  g_return_val_if_fail (out_stream != NULL, FALSE);
  g_return_val_if_fail (out_fdno != NULL, FALSE);

  if (!g_unix_open_pipe (pipefds, FD_CLOEXEC, error))
    return FALSE;

  if (for_read)
    {
      *out_stream = g_unix_input_stream_new (pipefds[0], TRUE);
      *out_fdno = pipefds[1];
    }
  else
    {
      *out_stream = g_unix_output_stream_new (pipefds[1], TRUE);
      *out_fdno = pipefds[0];
    }
  g_array_append_val (self->inherit_fds, *out_fdno);
  g_array_append_val (self->postfork_close_fds, *out_fdno);

  return TRUE;
}

/**
 * gs_subprocess_context_open_pipe_read:
 * @self:
 * @out_stream: (out) (transfer full): A newly referenced output stream
 * @out_fdno: (out): File descriptor number for the subprocess side of the pipe
 *
 * This allows you to open a pipe between the parent and child
 * processes, independent of the standard streams.  For this function,
 * the pipe is set up so that the parent can read, and the child can
 * write.  For the opposite version, see
 * gs_subprocess_context_open_pipe_write().
 *
 * The returned @out_fdno is the file descriptor number that the child
 * will see; you need to communicate this number via a separate
 * channel, such as the argument list.  For example, if you're using
 * this pipe to send a password, provide
 * <literal>--password-fd=&lt;fdno string&gt;</literal>.
 *
 * Returns: %TRUE on success, %FALSE on error (and @error will be set)
 */
gboolean
gs_subprocess_context_open_pipe_read (GSSubprocessContext         *self,
                                      GInputStream               **out_stream,
                                      gint                        *out_fdno,
                                      GError                     **error)
{
  return open_pipe_internal (self, TRUE, (void**)out_stream, out_fdno, error);
}

/**
 * gs_subprocess_context_open_pipe_write:
 * @self:
 * @out_stream: (out) (transfer full): A newly referenced stream
 * @out_fdno: (out): File descriptor number for the subprocess side of the pipe
 *
 * Like gs_subprocess_context_open_pipe_read(), but returns a writable
 * channel from which the child process can read.
 *
 * Returns: %TRUE on success, %FALSE on error (and @error will be set)
 */
gboolean
gs_subprocess_context_open_pipe_write (GSSubprocessContext         *self,
                                       GOutputStream              **out_stream,
                                       gint                        *out_fdno,
                                       GError                     **error)
{
  return open_pipe_internal (self, FALSE, (void**)out_stream, out_fdno, error);
}

#endif

#endif
