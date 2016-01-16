/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright © 2012 Red Hat, Inc.
 * Copyright © 2012 Canonical Limited
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * See the included COPYING file for more information.
 *
 * Authors: Colin Walters <walters@verbum.org>
 *          Ryan Lortie <desrt@desrt.ca>
 */

#include "config.h"

#define _GSYSTEM_NO_LOCAL_ALLOC
#include "libgsystem.h"

#if GLIB_CHECK_VERSION(2,34,0)

/**
 * SECTION:gssubprocess
 * @title: GSSubprocess
 * @short_description: Create child processes and monitor their status
 *
 * This class wraps the lower-level g_spawn_async_with_pipes() API,
 * providing a more modern GIO-style API, such as returning
 * #GInputStream objects for child output pipes.
 *
 * One major advantage that GIO brings over the core GLib library is
 * comprehensive API for asynchronous I/O, such
 * g_output_stream_splice_async().  This makes GSubprocess
 * significantly more powerful and flexible than equivalent APIs in
 * some other languages such as the <literal>subprocess.py</literal>
 * included with Python.  For example, using #GSubprocess one could
 * create two child processes, reading standard output from the first,
 * processing it, and writing to the input stream of the second, all
 * without blocking the main loop.
 *
 * Since: 2.36
 */

#include "config.h"

#include "gsystem-subprocess.h"
#include "gsystem-subprocess-context-private.h"

#include <string.h>
#ifdef G_OS_UNIX
#include <gio/gunixoutputstream.h>
#include <gio/gfiledescriptorbased.h>
#include <gio/gunixinputstream.h>
#include <glib-unix.h>
#endif
#include <fcntl.h>
#ifdef G_OS_WIN32
#define _WIN32_WINNT 0x0500
#include <windows.h>
#include "giowin32-priv.h"
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif

static void initable_iface_init (GInitableIface         *initable_iface);

typedef GObjectClass GSSubprocessClass;

#ifdef G_OS_UNIX
static void
gs_subprocess_unix_queue_waitpid (GSSubprocess  *self);
#endif

struct _GSSubprocess
{
  GObject parent;

  GSSubprocessContext *context;
  GPid pid;

  guint pid_valid : 1;
  guint reaped_child : 1;
  guint unused : 30;

  /* These are the streams created if a pipe is requested via flags. */
  GOutputStream *stdin_pipe;
  GInputStream  *stdout_pipe;
  GInputStream  *stderr_pipe;
};

G_DEFINE_TYPE_WITH_CODE (GSSubprocess, gs_subprocess, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, initable_iface_init));

enum
{
  PROP_0,
  PROP_CONTEXT,
  N_PROPS
};

static GParamSpec *gs_subprocess_pspecs[N_PROPS];

static void
gs_subprocess_init (GSSubprocess  *self)
{
}

static void
gs_subprocess_finalize (GObject *object)
{
  GSSubprocess *self = GS_SUBPROCESS (object);

  if (self->pid_valid)
    {
#ifdef G_OS_UNIX
      /* Here we need to actually call waitpid() to clean up the
       * zombie.  In case the child hasn't actually exited, defer this
       * cleanup to the worker thread.
       */
      if (!self->reaped_child)
        gs_subprocess_unix_queue_waitpid (self);
#endif
      g_spawn_close_pid (self->pid);
    }

  g_clear_object (&self->stdin_pipe);
  g_clear_object (&self->stdout_pipe);
  g_clear_object (&self->stderr_pipe);

  if (G_OBJECT_CLASS (gs_subprocess_parent_class)->finalize != NULL)
    G_OBJECT_CLASS (gs_subprocess_parent_class)->finalize (object);
}

static void
gs_subprocess_set_property (GObject      *object,
                           guint         prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  GSSubprocess *self = GS_SUBPROCESS (object);

  switch (prop_id)
    {
    case PROP_CONTEXT:
      self->context = g_value_dup_object (value);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
gs_subprocess_get_property (GObject    *object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  GSSubprocess *self = GS_SUBPROCESS (object);

  switch (prop_id)
    {
    case PROP_CONTEXT:
      g_value_set_object (value, self->context);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
gs_subprocess_class_init (GSSubprocessClass *class)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (class);

  gobject_class->finalize = gs_subprocess_finalize;
  gobject_class->get_property = gs_subprocess_get_property;
  gobject_class->set_property = gs_subprocess_set_property;

  /**
   * GSSubprocess:context:
   *
   *
   * Since: 2.36
   */
  gs_subprocess_pspecs[PROP_CONTEXT] = g_param_spec_object ("context", "Context", "Subprocess options", GS_TYPE_SUBPROCESS_CONTEXT,
							   G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
							   G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, N_PROPS, gs_subprocess_pspecs);
}

#ifdef G_OS_UNIX

static gboolean
gs_subprocess_unix_waitpid_dummy (gpointer data)
{
  return FALSE;
}

static void
gs_subprocess_unix_queue_waitpid (GSSubprocess  *self)
{
  GMainContext *worker_context;
  GSource *waitpid_source;

#ifdef GLIB_COMPILATION
  worker_context = GLIB_PRIVATE_CALL (g_get_worker_context) ();
#else
  worker_context = g_main_context_get_thread_default ();
#endif
  waitpid_source = g_child_watch_source_new (self->pid); 
  g_source_set_callback (waitpid_source, gs_subprocess_unix_waitpid_dummy, NULL, NULL);
  g_source_attach (waitpid_source, worker_context);
  g_source_unref (waitpid_source);
}

#endif

static GInputStream *
platform_input_stream_from_spawn_fd (gint fd)
{
  if (fd < 0)
    return NULL;

#ifdef G_OS_UNIX
  return g_unix_input_stream_new (fd, TRUE);
#else
  return g_win32_input_stream_new_from_fd (fd, TRUE);
#endif
}

static GOutputStream *
platform_output_stream_from_spawn_fd (gint fd)
{
  if (fd < 0)
    return NULL;

#ifdef G_OS_UNIX
  return g_unix_output_stream_new (fd, TRUE);
#else
  return g_win32_output_stream_new_from_fd (fd, TRUE);
#endif
}

#ifdef G_OS_UNIX
static gint
unix_open_file (const char  *filename,
                gint         mode,
                GError     **error)
{
  gint my_fd;

  do
    my_fd = open (filename, mode | O_BINARY | O_CLOEXEC, 0666);
  while (my_fd == -1 && errno == EINTR);

  /* If we return -1 we should also set the error */
  if (my_fd < 0)
    {
      gint saved_errno = errno;
      char *display_name;

      display_name = g_filename_display_name (filename);
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved_errno),
                   "Error opening file '%s': %s", display_name,
                   g_strerror (saved_errno));
      g_free (display_name);
      /* fall through... */
    }

  return my_fd;
}
#endif

typedef struct
{
  gint                   fds[3];
  GArray                *inherit_fds;
  GSpawnChildSetupFunc   child_setup_func;
  gpointer               child_setup_data;
} ChildData;

static void
child_setup (gpointer user_data)
{
  ChildData *child_data = user_data;
  guint i;
  gint result;

  /* We're on the child side now.  "Rename" the file descriptors in
   * child_data.fds[] to stdin/stdout/stderr.
   *
   * We don't close the originals.  It's possible that the originals
   * should not be closed and if they should be closed then they should
   * have been created O_CLOEXEC.
   */
  for (i = 0; i < 3; i++)
    {
      if (child_data->fds[i] != -1 && child_data->fds[i] != (int) i)
        {
          do
            result = dup2 (child_data->fds[i], i);
          while (G_UNLIKELY (result == -1 && errno == EINTR));
        }
    }

  /* Unset the CLOEXEC flag for the child *should* inherit */
  for (i = 0; i < child_data->inherit_fds->len; i++)
    {
      int fd = g_array_index (child_data->inherit_fds, int, i);
      int flags;

      do
        flags = fcntl (fd, F_GETFL);
      while (G_UNLIKELY (flags == -1 && errno == EINTR));

      flags &= ~FD_CLOEXEC;
      
      do
        result = fcntl (fd, F_SETFD, flags);
      while (G_UNLIKELY (result == -1 && errno == EINTR));
    }

  if (child_data->child_setup_func)
    child_data->child_setup_func (child_data->child_setup_data);
}

static gboolean
initable_init (GInitable     *initable,
               GCancellable  *cancellable,
               GError       **error)
{
  GSSubprocess *self = GS_SUBPROCESS (initable);
  ChildData child_data = { { -1, -1, -1 } };
  gint *pipe_ptrs[3] = { NULL, NULL, NULL };
  gint pipe_fds[3] = { -1, -1, -1 };
  gint close_fds[3] = { -1, -1, -1 };
  GSpawnFlags spawn_flags = 0;
  gboolean success = FALSE;
  guint i;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  /* We must setup the three fds that will end up in the child as stdin,
   * stdout and stderr.
   *
   * First, stdin.
   */
#ifdef G_OS_UNIX
  if (self->context->stdin_fd != -1)
    child_data.fds[0] = self->context->stdin_fd;
  else if (self->context->stdin_path != NULL)
    {
      child_data.fds[0] = close_fds[0] = unix_open_file (self->context->stdin_path, 
							 O_RDONLY, error);
      if (child_data.fds[0] == -1)
	goto out;
    }
  else
#endif
  if (self->context->stdin_disposition == GS_SUBPROCESS_STREAM_DISPOSITION_NULL)
    ; /* nothing */
  else if (self->context->stdin_disposition == GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT)
    spawn_flags |= G_SPAWN_CHILD_INHERITS_STDIN;
  else if (self->context->stdin_disposition == GS_SUBPROCESS_STREAM_DISPOSITION_PIPE)
    pipe_ptrs[0] = &pipe_fds[0];
  else
    g_assert_not_reached ();

  /* Next, stdout. */
#ifdef G_OS_UNIX
  if (self->context->stdout_fd != -1)
    child_data.fds[1] = self->context->stdout_fd;
  else if (self->context->stdout_path != NULL)
    {
      child_data.fds[1] = close_fds[1] = unix_open_file (self->context->stdout_path, 
							 O_CREAT | O_WRONLY, error);
      if (child_data.fds[1] == -1)
	goto out;
    }
  else
#endif
  if (self->context->stdout_disposition == GS_SUBPROCESS_STREAM_DISPOSITION_NULL)
    spawn_flags |= G_SPAWN_STDOUT_TO_DEV_NULL;
  else if (self->context->stdout_disposition == GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT)
    ; /* Nothing */
  else if (self->context->stdout_disposition == GS_SUBPROCESS_STREAM_DISPOSITION_PIPE)
    pipe_ptrs[1] = &pipe_fds[1];
  else
    g_assert_not_reached ();

  /* Finally, stderr. */
#ifdef G_OS_UNIX
  if (self->context->stderr_fd != -1)
    child_data.fds[2] = self->context->stderr_fd;
  else if (self->context->stderr_path != NULL)
    {
      child_data.fds[2] = close_fds[2] = unix_open_file (self->context->stderr_path, 
							 O_CREAT | O_WRONLY, error);
      if (child_data.fds[2] == -1)
	goto out;
    }
  else
#endif
  if (self->context->stderr_disposition == GS_SUBPROCESS_STREAM_DISPOSITION_NULL)
    spawn_flags |= G_SPAWN_STDERR_TO_DEV_NULL;
  else if (self->context->stderr_disposition == GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT)
    ; /* Nothing */
  else if (self->context->stderr_disposition == GS_SUBPROCESS_STREAM_DISPOSITION_PIPE)
    pipe_ptrs[2] = &pipe_fds[2];
  else if (self->context->stderr_disposition == GS_SUBPROCESS_STREAM_DISPOSITION_STDERR_MERGE)
    /* This will work because stderr gets setup after stdout. */
    child_data.fds[2] = 1;
  else
    g_assert_not_reached ();

  child_data.inherit_fds = self->context->inherit_fds;

  if (self->context->keep_descriptors)
    spawn_flags |= G_SPAWN_LEAVE_DESCRIPTORS_OPEN;

  if (self->context->search_path)
    spawn_flags |= G_SPAWN_SEARCH_PATH;
  else if (self->context->search_path_from_envp)
    spawn_flags |= G_SPAWN_SEARCH_PATH_FROM_ENVP;
  else if (!g_path_is_absolute (((gchar**)self->context->argv)[0]))
    spawn_flags |= G_SPAWN_SEARCH_PATH;

  if (self->context->has_argv0)
    spawn_flags |= G_SPAWN_FILE_AND_ARGV_ZERO;

  spawn_flags |= G_SPAWN_DO_NOT_REAP_CHILD;
#ifdef GLIB_COMPILATION
  spawn_flags |= G_SPAWN_CLOEXEC_PIPES;
#endif

  child_data.child_setup_func = self->context->child_setup_func;
  child_data.child_setup_data = self->context->child_setup_data;
  success = g_spawn_async_with_pipes (self->context->cwd,
				      (char**)self->context->argv,
				      self->context->envp,
                                      spawn_flags,
                                      child_setup, &child_data,
                                      &self->pid,
                                      pipe_ptrs[0], pipe_ptrs[1], pipe_ptrs[2],
                                      error);
  if (success)
    self->pid_valid = TRUE;

out:
  for (i = 0; i < 3; i++)
    if (close_fds[i] != -1)
      close (close_fds[i]);

  for (i = 0; i < self->context->postfork_close_fds->len; i++)
    (void) close (g_array_index (self->context->postfork_close_fds, int, i));

  self->stdin_pipe = platform_output_stream_from_spawn_fd (pipe_fds[0]);
  self->stdout_pipe = platform_input_stream_from_spawn_fd (pipe_fds[1]);
  self->stderr_pipe = platform_input_stream_from_spawn_fd (pipe_fds[2]);

  return success;
}

static void
initable_iface_init (GInitableIface *initable_iface)
{
  initable_iface->init = initable_init;
}

/**
 * gs_subprocess_new:
 *
 * Create a new process, using the parameters specified by
 * GSSubprocessContext.
 *
 * Returns: (transfer full): A newly created %GSSubprocess, or %NULL on error (and @error will be set)
 *
 * Since: 2.36
 */
GSSubprocess *
gs_subprocess_new (GSSubprocessContext   *context,
                   GCancellable          *cancellable,
                   GError               **error)
{
  return g_initable_new (GS_TYPE_SUBPROCESS,
                         cancellable, error,
                         "context", context,
                         NULL);
}

/**
 * gs_subprocess_get_pid:
 * @self: a #GSSubprocess
 *
 * The identifier for this child process; it is valid as long as the
 * process @self is referenced.  In particular, do
 * <emphasis>not</emphasis> call g_spawn_close_pid() on this value;
 * that is handled internally.
 *
 * On some Unix versions, it is possible for there to be a race
 * condition where waitpid() may have been called to collect the child
 * before any watches (such as that installed by
 * gs_subprocess_add_watch()) have fired.  If you are planning to use
 * native functions such as kill() on the pid, your program should
 * gracefully handle an %ESRCH result to mitigate this.
 *
 * If you want to request process termination, using the high level
 * gs_subprocess_request_exit() and gs_subprocess_force_exit() API is
 * recommended.
 *
 * Returns: Operating-system specific identifier for child process
 *
 * Since: 2.36
 */
GPid
gs_subprocess_get_pid (GSSubprocess     *self)
{
  g_return_val_if_fail (GS_IS_SUBPROCESS (self), 0);

  return self->pid;
}

/**
 * gs_subprocess_get_stdin_pipe:
 *
 * Returns: (transfer none): Pipe
 */
GOutputStream *
gs_subprocess_get_stdin_pipe (GSSubprocess       *self)
{
  g_return_val_if_fail (GS_IS_SUBPROCESS (self), NULL);
  g_return_val_if_fail (self->stdin_pipe, NULL);

  return self->stdin_pipe;
}

/**
 * gs_subprocess_get_stdout_pipe:
 *
 * Returns: (transfer none): Pipe
 */
GInputStream *
gs_subprocess_get_stdout_pipe (GSSubprocess      *self)
{
  g_return_val_if_fail (GS_IS_SUBPROCESS (self), NULL);
  g_return_val_if_fail (self->stdout_pipe, NULL);

  return self->stdout_pipe;
}

/**
 * gs_subprocess_get_stderr_pipe:
 *
 * Returns: (transfer none): Pipe
 */
GInputStream *
gs_subprocess_get_stderr_pipe (GSSubprocess      *self)
{
  g_return_val_if_fail (GS_IS_SUBPROCESS (self), NULL);
  g_return_val_if_fail (self->stderr_pipe, NULL);

  return self->stderr_pipe;
}

typedef struct {
  GSSubprocess *self;
  GCancellable *cancellable;
  GSimpleAsyncResult *result;
} GSSubprocessWatchData;

static gboolean
gs_subprocess_on_child_exited (GPid       pid,
			      gint       status_code,
			      gpointer   user_data)
{
  GSSubprocessWatchData *data = user_data;
  GError *error = NULL;
  
  if (g_cancellable_set_error_if_cancelled (data->cancellable, &error))
    {
      g_simple_async_result_take_error (data->result, error);
    }
  else
    {
      data->self->reaped_child = TRUE;

      g_simple_async_result_set_op_res_gssize (data->result, status_code);
    }

  g_simple_async_result_complete (data->result);

  g_object_unref (data->result);
  g_object_unref (data->self);
  g_free (data);

  return FALSE;
}

/**
 * gs_subprocess_wait:
 * @self: a #GSSubprocess
 * @cancellable: a #GCancellable
 * @callback: Invoked when process exits, or @cancellable is cancelled
 * @user_data: Data for @callback
 *
 * Start an asynchronous wait for the subprocess @self to exit.
 *
 * Since: 2.36
 */
void
gs_subprocess_wait (GSSubprocess                *self,
		   GCancellable               *cancellable,
		   GAsyncReadyCallback         callback,
		   gpointer                    user_data)
{
  GSource *source;
  GSSubprocessWatchData *data;

  data = g_new0 (GSSubprocessWatchData, 1);

  data->self = g_object_ref (self);
  data->result = g_simple_async_result_new ((GObject*)self, callback, user_data,
					    gs_subprocess_wait);

  source = g_child_watch_source_new (self->pid);

  g_source_set_callback (source, (GSourceFunc)gs_subprocess_on_child_exited,
			 data, NULL);
  if (cancellable)
    {
      GSource *cancellable_source;

      data->cancellable = g_object_ref (cancellable);

      cancellable_source = g_cancellable_source_new (cancellable);
      g_source_add_child_source (source, cancellable_source);
      g_source_unref (cancellable_source);
    }

  g_source_attach (source, g_main_context_get_thread_default ());
  g_source_unref (source);
}

/**
 * gs_subprocess_wait_finish:
 * @self: a #GSSubprocess
 * @result: a #GAsyncResult
 * @out_exit_status: (out): Exit status of the process encoded in platform-specific way
 * @error: a #GError
 *
 * The exit status of the process will be stored in @out_exit_status.
 * See the documentation of g_spawn_check_exit_status() for more
 * details.
 *
 * Note that @error is not set if the process exits abnormally; you
 * must use g_spawn_check_exit_status() for that.
 *
 * Since: 2.36
 */
gboolean
gs_subprocess_wait_finish (GSSubprocess                *self,
			  GAsyncResult               *result,
			  int                        *out_exit_status,
			  GError                    **error)
{
  GSimpleAsyncResult *simple;

  simple = G_SIMPLE_ASYNC_RESULT (result);

  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  *out_exit_status = g_simple_async_result_get_op_res_gssize (simple);
  
  return TRUE;
}

typedef struct {
  GMainLoop *loop;
  gint *exit_status_ptr;
  gboolean caught_error;
  GError **error;
} GSSubprocessSyncWaitData;

static void
gs_subprocess_on_sync_wait_complete (GObject       *object,
				    GAsyncResult  *result,
				    gpointer       user_data)
{
  GSSubprocessSyncWaitData *data = user_data;

  if (!gs_subprocess_wait_finish ((GSSubprocess*)object, result, 
				 data->exit_status_ptr, data->error))
    data->caught_error = TRUE;

  g_main_loop_quit (data->loop);
}

/**
 * gs_subprocess_wait_sync:
 * @self: a #GSSubprocess
 * @out_exit_status: (out): Platform-specific exit code
 * @cancellable: a #GCancellable
 * @error: a #GError
 *
 * Synchronously wait for the subprocess to terminate, returning the
 * status code in @out_exit_status.  See the documentation of
 * g_spawn_check_exit_status() for how to interpret it.  Note that if
 * @error is set, then @out_exit_status will be left uninitialized.
 * 
 * Returns: %TRUE on success, %FALSE if @cancellable was cancelled
 *
 * Since: 2.36
 */
gboolean
gs_subprocess_wait_sync (GSSubprocess        *self,
			int                *out_exit_status,
			GCancellable       *cancellable,
			GError            **error)
{
  gboolean ret = FALSE;
  gboolean pushed_thread_default = FALSE;
  GMainContext *context = NULL;
  GSSubprocessSyncWaitData data;

  memset (&data, 0, sizeof (data));

  g_return_val_if_fail (GS_IS_SUBPROCESS (self), FALSE);

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  context = g_main_context_new ();
  g_main_context_push_thread_default (context);
  pushed_thread_default = TRUE;

  data.exit_status_ptr = out_exit_status;
  data.loop = g_main_loop_new (context, TRUE);
  data.error = error;

  gs_subprocess_wait (self, cancellable,
		     gs_subprocess_on_sync_wait_complete, &data);

  g_main_loop_run (data.loop);

  if (data.caught_error)
    goto out;

  ret = TRUE;
 out:
  if (pushed_thread_default)
    g_main_context_pop_thread_default (context);
  if (context)
    g_main_context_unref (context);
  if (data.loop)
    g_main_loop_unref (data.loop);

  return ret;
}

/**
 * gs_subprocess_wait_sync_check:
 * @self: a #GSSubprocess
 * @cancellable: a #GCancellable
 * @error: a #GError
 *
 * Combines gs_subprocess_wait_sync() with g_spawn_check_exit_status().
 * 
 * Returns: %TRUE on success, %FALSE if process exited abnormally, or @cancellable was cancelled
 *
 * Since: 2.36
 */
gboolean
gs_subprocess_wait_sync_check (GSSubprocess        *self,
			      GCancellable       *cancellable,
			      GError            **error)
{
  gboolean ret = FALSE;
  int exit_status;

  if (!gs_subprocess_wait_sync (self, &exit_status, cancellable, error))
    goto out;

  if (!g_spawn_check_exit_status (exit_status, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

/**
 * gs_subprocess_request_exit:
 * @self: a #GSSubprocess
 *
 * This API uses an operating-system specific mechanism to request
 * that the subprocess gracefully exit.  This API is not available on
 * all operating systems; for those not supported, it will do nothing
 * and return %FALSE.  Portable code should handle this situation
 * gracefully.  For example, if you are communicating via input or
 * output pipe with the child, many programs will automatically exit
 * when one of their standard input or output are closed.
 *
 * On Unix, this API sends %SIGTERM.
 *
 * A %TRUE return value does <emphasis>not</emphasis> mean the
 * subprocess has exited, merely that an exit request was initiated.
 * You can use gs_subprocess_add_watch() to monitor the status of the
 * process after calling this function.
 *
 * This function returns %TRUE if the process has already exited.
 *
 * Returns: %TRUE if the operation is supported, %FALSE otherwise.
 *
 * Since: 2.36
 */
gboolean
gs_subprocess_request_exit (GSSubprocess *self)
{
  g_return_val_if_fail (GS_IS_SUBPROCESS (self), FALSE);

#ifdef G_OS_UNIX
  (void) kill (self->pid, SIGTERM);
  return TRUE;
#else
  return FALSE;
#endif
}

/**
 * gs_subprocess_force_exit:
 * @self: a #GSSubprocess
 *
 * Use an operating-system specific method to attempt an immediate,
 * forceful termination of the process.  There is no mechanism to
 * determine whether or not the request itself was successful;
 * however, you can use gs_subprocess_wait() to monitor the status of
 * the process after calling this function.
 *
 * On Unix, this function sends %SIGKILL.
 */
void
gs_subprocess_force_exit (GSSubprocess *self)
{
  g_return_if_fail (GS_IS_SUBPROCESS (self));

#if !defined(GLIB_COMPIATION)
  {
    int ret;
    do
      ret = kill (self->pid, SIGKILL);
    while (ret == -1 && errno == EINTR);
  }
#elif defined(G_OS_UNIX)
  GLIB_PRIVATE_CALL (g_main_send_signal) (self->pid, SIGKILL);
#else
  TerminateProcess (self->pid, 1);
#endif
}

GSSubprocess *
gs_subprocess_new_simple_argl (GSSubprocessStreamDisposition stdout_disposition,
			      GSSubprocessStreamDisposition  stderr_disposition,
                               GCancellable                 *cancellable,
			      GError                       **error,
			      const gchar                   *first_arg,
			      ...)
{
  va_list args;
  GSSubprocess *result;
  GSSubprocessContext *context;

  va_start (args, first_arg);
  context = gs_subprocess_context_newa (first_arg, args);
  va_end (args);
  result = gs_subprocess_new (context, cancellable, error);
  g_object_unref (context);
  
  return result;
}

/**
 * gs_subprocess_new_simple_argv:
 * @argv: (array zero-terminated=1) (element-type utf8): Argument array
 * @stdout_disposition: Where to redirect stdout
 * @stderr_disposition: Where to redirect stdout
 * @error: a #GError
 *
 * Create a new subprocess using the provided argument array and
 * stream dispositions.
 */
GSSubprocess *
gs_subprocess_new_simple_argv (gchar                       **argv,
			      GSSubprocessStreamDisposition  stdout_disposition,
			      GSSubprocessStreamDisposition  stderr_disposition,
                               GCancellable                 *cancellable,
			      GError                      **error)
{
  GSSubprocessContext *context;
  GSSubprocess *result;

  context = gs_subprocess_context_new (argv);
  gs_subprocess_context_set_stdout_disposition (context, stdout_disposition);
  gs_subprocess_context_set_stderr_disposition (context, stderr_disposition);

  result = gs_subprocess_new (context, cancellable, error);
  g_object_unref (context);

  return result;
}

/**
 * gs_subprocess_simple_run_sync:
 * @cwd: Current working directory
 * @stdin_disposition: What to do with standard input
 * @cancellable: a #GCancellable
 * @error: a #GError
 * @first_arg: First argument
 * @...: Remaining arguments, %NULL terminated
 *
 * Run a process synchronously, throw an error if it fails.
 */
gboolean
gs_subprocess_simple_run_sync (const char                    *cwd,
                               GSSubprocessStreamDisposition  stdin_disposition,
                               GCancellable                  *cancellable,
                               GError                       **error,
                               const char                    *first_arg,
                               ...)
{
  gboolean ret = FALSE;
  va_list args;
  GSSubprocess *proc = NULL;
  GSSubprocessContext *context = NULL;

  va_start (args, first_arg);
  context = gs_subprocess_context_newa (first_arg, args);
  va_end (args);
  gs_subprocess_context_set_stdin_disposition (context, stdin_disposition);
  gs_subprocess_context_set_cwd (context, cwd);
  proc = gs_subprocess_new (context, cancellable, error);
  if (!proc)
    goto out;

  if (!gs_subprocess_wait_sync_check (proc, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  g_object_unref (context);
  if (proc)
    g_object_unref (proc);
  return ret;
}

#endif
