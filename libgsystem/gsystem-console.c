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

#include "config.h"

#define _GSYSTEM_NO_LOCAL_ALLOC
#include "libgsystem.h"

/**
 * SECTION:gsconsole
 * @title: GSConsole
 * @short_description: Interact with standard input/output as well as terminal
 *
 * First, this class offers API to access the standard input and
 * output/error, streams as #GInputStream and #GOutputStream
 * respectively.
 *
 * In the case where the process is connected to a controlling
 * terminal, the gs_console_get() API is available, which exposes a
 * number of additional features such as no-echo password reading.
 *
 * Since: 2.36
 */

#include "config.h"

#include "gsystem-console.h"

#include <string.h>
#ifdef G_OS_UNIX
#include <gio/gunixoutputstream.h>
#include <gio/gfiledescriptorbased.h>
#include <gio/gunixinputstream.h>
#include <glib-unix.h>
#include <unistd.h>
#include <termios.h>
#endif
#include <fcntl.h>

typedef GObjectClass GSConsoleClass;

struct _GSConsole
{
  GObject parent;

  gboolean in_status_line;
  gssize last_line_written;
};

G_DEFINE_TYPE (GSConsole, gs_console, G_TYPE_OBJECT);

static void
gs_console_init (GSConsole  *self)
{
  self->last_line_written = -1;
}

static void
gs_console_class_init (GSConsoleClass *class)
{
}

/**
 * gs_console_get:
 *
 * If the current process has an interactive console, return the
 * singleton #GSConsole instance.  On Unix, this is equivalent to
 * isatty().  For all other cases, such as pipes, sockets, /dev/null,
 * this function will return %NULL.
 *
 * Returns: (transfer none): The console instance, or %NULL if not interactive
 *
 * Since: 2.36
 */
GSConsole *
gs_console_get (void)
{
  static gsize checked = 0;
  static GSConsole *instance = NULL;

  if (g_once_init_enter (&checked))
    {
#ifdef G_OS_UNIX
      if (isatty (0) && isatty (1))
        instance = g_object_new (GS_TYPE_CONSOLE, NULL);
#endif
      g_once_init_leave (&checked, 1);
    }
  
  return (GSConsole*) instance;
}

/**
 * gs_console_get_stdin:
 *
 * Returns: (transfer none): The singleton stream connected to standard input
 */
GInputStream *
gs_console_get_stdin (void)
{
#ifdef G_OS_UNIX
  static gsize instance = 0;

  if (g_once_init_enter (&instance))
    g_once_init_leave (&instance, (gsize) g_unix_input_stream_new (0, FALSE));
  
  return (GInputStream*) instance;
#else
  g_error ("not implemented");
#endif
}

/**
 * gs_console_get_stdout:
 *
 * Returns: (transfer none): The singleton stream connected to standard output
 */
GOutputStream *
gs_console_get_stdout (void)
{
#ifdef G_OS_UNIX
  static gsize instance = 0;

  if (g_once_init_enter (&instance))
    g_once_init_leave (&instance, (gsize) g_unix_output_stream_new (1, FALSE));
  
  return (GOutputStream*) instance;
#else
  g_error ("not implemented");
#endif
}

/**
 * gs_console_get_stderr:
 *
 * Returns: (transfer none): The singleton stream connected to standard error
 */
GOutputStream *
gs_console_get_stderr (void)
{
#ifdef G_OS_UNIX
  static gsize instance = 0;

  if (g_once_init_enter (&instance))
    g_once_init_leave (&instance, (gsize) g_unix_output_stream_new (2, FALSE));
  
  return (GOutputStream*) instance;
#else
  g_error ("not implemented");
#endif
}

#ifdef G_OS_UNIX
static inline void
_set_error_from_errno (GError **error)
{
  int errsv = errno;
  g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                       g_strerror (errsv));
}
#endif

/**
 * gs_console_read_password:
 * @console: the #GSConsole
 * @prompt: A string to output before reading the password
 * @error: a #GError
 *
 * Write @prompt to standard output, then switch output echo off, read
 * a result string, then switch output echo back on.
 *
 * Returns: A string, or %NULL on error
 */
char *
gs_console_read_password (GSConsole     *console,
                          const char    *prompt,
                          GCancellable  *cancellable,
                          GError       **error)
{
#ifdef G_OS_UNIX
  gboolean ret = FALSE;
  /* This code is modified from that found in
   * polkit/src/polkittextagentlistener.c, reused under the LGPL v2.1
   */
  int res;
  struct termios ts, ots;
  GInputStream *in;
  GOutputStream *out;
  GString *str = NULL;
  gsize bytes_written;
  gboolean reset_terminal = FALSE;

  in = gs_console_get_stdin ();
  out = gs_console_get_stdout ();

  if (!g_output_stream_write_all (out, prompt, strlen (prompt), &bytes_written,
                                  cancellable, error))
    goto out;
  if (!g_output_stream_flush (out, cancellable, error))
    goto out;

  /* TODO: We really ought to block SIGINT and STGSTP (and probably
   *       other signals too) so we can restore the terminal (since we
   *       turn off echoing). See e.g. Advanced Programming in the
   *       UNIX Environment 2nd edition (Steves and Rago) section
   *       18.10, pg 660 where this is suggested. See also various
   *       getpass(3) implementations
   *
   *       However, since we are a library routine the user could have
   *       multiple threads - in fact, typical usage of
   *       PolkitAgentTextListener is to run it in a thread. And
   *       unfortunately threads and POSIX signals is a royal PITA.
   *
   *       Maybe we could fork(2) and ask for the password in the
   *       child and send it back to the parent over a pipe? (we are
   *       guaranteed that there is only one thread in the child
   *       process).
   *
   *       (Side benefit of doing this in a child process is that we
   *       could avoid blocking the thread where the
   *       PolkitAgentTextListener object is being serviced from. But
   *       since this class is normally used in a dedicated thread
   *       it doesn't really matter *anyway*.)
   *
   *       Anyway, On modern Linux not doing this doesn't seem to be a
   *       problem - looks like modern shells restore echoing anyway
   *       on the first input. So maybe it's not even worth solving
   *       the problem.
   */

  do
    res = tcgetattr (1, &ts);
  while (G_UNLIKELY (res == -1 && errno == EINTR));
  if (res == -1)
    {
      _set_error_from_errno (error);
      goto out;
    }
  ots = ts;
  ts.c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHONL);
  do
    res = tcsetattr (1, TCSAFLUSH, &ts);
  while (G_UNLIKELY (res == -1 && errno == EINTR));
  if (res == -1)
    {
      _set_error_from_errno (error);
      goto out;
    }

  /* After this point, we'll need to clean up the terminal in case of
   * error.
   */
  reset_terminal = TRUE;

  str = g_string_new (NULL);
  while (TRUE)
    {
      gssize bytes_read;
      guint8 buf[1];

      /* FIXME - we should probably be converting from the system
       * codeset, in case it's not UTF-8.
       */
      bytes_read = g_input_stream_read (in, buf, sizeof (buf),
                                        cancellable, error);
      if (bytes_read < 0)
        goto out;
      else if (bytes_read == 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
                       "End of stream while reading password");
          goto out;
        }
      else if (buf[0] == '\n')
        {
          break;
        }
      else
        {
          g_string_append_c (str, buf[0]);
        }
    }

  ret = TRUE;
 out:
  if (reset_terminal)
    {
      do
        res = tcsetattr (1, TCSAFLUSH, &ots);
      while (G_UNLIKELY (res == -1 && errno == EINTR));
      if (res == -1)
        {
          _set_error_from_errno (error);
          g_string_free (str, TRUE);
          return NULL;
        }
    }

  if (!ret)
    {
      g_string_free (str, TRUE);
      return NULL;
    }
  else
    {
      return g_string_free (str, FALSE);
    }
#else
  g_error ("not implemented");
#endif
}

/**
 * gs_console_begin_status_line:
 * @console: the #GSConsole
 * @line: String to output
 *
 * The primary use case for this function is to output periodic
 * "status" or "progress" information.  The first time this function
 * is called, @line will be output normally.  Subsequent invocations
 * will overwrite the previous.
 *
 * You must invoke gs_console_end_status_line() to return the console
 * to normal mode.  In particular, concurrent use of this function and
 * the stream returned by gs_console_get_stdout() results in undefined
 * behavior.
 */
gboolean
gs_console_begin_status_line (GSConsole     *console,
                              const char    *line,
                              GCancellable  *cancellable,
                              GError       **error)
{
#ifdef G_OS_UNIX
  gboolean ret = FALSE;
  gsize linelen;
  GOutputStream *out;
  gsize bytes_written;

  out = gs_console_get_stdout ();

  if (!console->in_status_line)
    {
      guint8 buf[3] = { (guint8)'\n', 0x1B, 0x37 };
      if (!g_output_stream_write_all (out, buf, sizeof (buf), &bytes_written,
                                      cancellable, error))
        goto out;
      console->in_status_line = TRUE;
      console->last_line_written = -1;
    }

  {
    guint8 buf[2] = { 0x1B, 0x38 };
    if (!g_output_stream_write_all (out, buf, sizeof (buf), &bytes_written,
                                    cancellable, error))
      goto out;
  }

  linelen = strlen (line);
  if (!g_output_stream_write_all (out, line, linelen, &bytes_written,
                                  cancellable, error))
    goto out;

  /* Now we need to pad with spaces enough to overwrite our last line
   */
  if (console->last_line_written >= 0
      && linelen < (gsize) console->last_line_written)
    {
      gsize towrite = console->last_line_written - linelen;
      const char c = ' ';
      while (towrite > 0)
        {
          if (!g_output_stream_write_all (out, &c, 1, &bytes_written,
                                          cancellable, error))
            goto out;
          towrite--;
        }
    }
  
  console->last_line_written = linelen;
  
  ret = TRUE;
 out:
  return ret;
#else
  g_error ("not implemented");
#endif
}

/**
 * gs_console_end_status_line:
 * @console: the #GSConsole
 *
 * Complete a series of invocations of gs_console_begin_status_line(),
 * returning the stream to normal mode.  The last printed status line
 * remains on the console; if this is not desired, print an empty
 * string to clear it before invoking this function.
 */
gboolean
gs_console_end_status_line (GSConsole     *console,
                            GCancellable  *cancellable,
                            GError       **error)
{
#ifdef G_OS_UNIX  
  gboolean ret = FALSE;
  GOutputStream *out;
  gsize bytes_written;
  char c = '\n';

  g_return_val_if_fail (console->in_status_line, FALSE);

  out = gs_console_get_stdout ();

  if (!g_output_stream_write_all (out, &c, 1, &bytes_written,
                                  cancellable, error))
    goto out;

  console->in_status_line = FALSE;

  ret = TRUE;
 out:
  return ret;
#else
  g_error ("not implemented");
#endif
}
