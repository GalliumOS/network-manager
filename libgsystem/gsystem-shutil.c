/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012 William Jon McCann <mccann@redhat.com>
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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define _GSYSTEM_NO_LOCAL_ALLOC
#include "libgsystem.h"
#include "gsystem-glib-compat.h"
#include <glib-unix.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>

/* Taken from systemd/src/shared/util.h */
union dirent_storage {
        struct dirent dent;
        guint8 storage[offsetof(struct dirent, d_name) +
                        ((NAME_MAX + 1 + sizeof(long)) & ~(sizeof(long) - 1))];
};

static inline void
_set_error_from_errno (GError **error)
{
  int errsv = errno;
  g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                       g_strerror (errsv));
}

static gboolean
copy_xattrs_from_file_to_fd (GFile         *src,
                             int            dest_fd,
                             GCancellable  *cancellable,
                             GError       **error)
{
  gboolean ret = FALSE;
  GVariant *src_xattrs = NULL;

  if (!gs_file_get_all_xattrs (src, &src_xattrs, cancellable, error))
    goto out;

  if (src_xattrs)
    {
      if (!gs_fd_set_all_xattrs (dest_fd, src_xattrs, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  g_clear_pointer (&src_xattrs, g_variant_unref);
  return ret;
}

typedef enum {
  GS_CP_MODE_NONE,
  GS_CP_MODE_HARDLINK,
  GS_CP_MODE_COPY_ALL
} GsCpMode;

static gboolean
cp_internal (GFile         *src,
             GFile         *dest,
             GsCpMode       mode,
             GCancellable  *cancellable,
             GError       **error)
{
  gboolean ret = FALSE;
  GFileEnumerator *enumerator = NULL;
  GFileInfo *src_info = NULL;
  GFile *dest_child = NULL;
  int dest_dfd = -1;
  int r;

  enumerator = g_file_enumerate_children (src, "standard::type,standard::name,unix::uid,unix::gid,unix::mode",
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          cancellable, error);
  if (!enumerator)
    goto out;

  src_info = g_file_query_info (src, "standard::name,unix::mode,unix::uid,unix::gid," \
                                "time::modified,time::modified-usec,time::access,time::access-usec",
                                G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                cancellable, error);
  if (!src_info)
    goto out;

  do
    r = mkdir (gs_file_get_path_cached (dest), 0755);
  while (G_UNLIKELY (r == -1 && errno == EINTR));
  if (r == -1)
    {
      _set_error_from_errno (error);
      goto out;
    }

  if (mode != GS_CP_MODE_NONE)
    {
      if (!gs_file_open_dir_fd (dest, &dest_dfd,
                                cancellable, error))
        goto out;

      do
        r = fchown (dest_dfd,
                    g_file_info_get_attribute_uint32 (src_info, "unix::uid"),
                    g_file_info_get_attribute_uint32 (src_info, "unix::gid"));
      while (G_UNLIKELY (r == -1 && errno == EINTR));
      if (r == -1)
        {
          _set_error_from_errno (error);
          goto out;
        }

      do
        r = fchmod (dest_dfd, g_file_info_get_attribute_uint32 (src_info, "unix::mode"));
      while (G_UNLIKELY (r == -1 && errno == EINTR));

      if (!copy_xattrs_from_file_to_fd (src, dest_dfd, cancellable, error))
        goto out;

      if (dest_dfd != -1)
        {
          (void) close (dest_dfd);
          dest_dfd = -1;
        }
    }

  while (TRUE)
    {
      GFileInfo *file_info = NULL;
      GFile *src_child = NULL;

      if (!gs_file_enumerator_iterate (enumerator, &file_info, &src_child,
                                       cancellable, error))
        goto out;
      if (!file_info)
        break;

      if (dest_child) g_object_unref (dest_child);
      dest_child = g_file_get_child (dest, g_file_info_get_name (file_info));

      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
        {
          if (!cp_internal (src_child, dest_child, mode,
                            cancellable, error))
            goto out;
        }
      else
        {
          gboolean did_link = FALSE;
          (void) unlink (gs_file_get_path_cached (dest_child));
          if (mode == GS_CP_MODE_HARDLINK)
            {
              if (link (gs_file_get_path_cached (src_child), gs_file_get_path_cached (dest_child)) == -1)
                {
                  if (!(errno == EMLINK || errno == EXDEV))
                    {
                      int errsv = errno;
                      g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                                           g_strerror (errsv));
                      goto out;
                    }
                  /* We failed to hardlink; fall back to copying all; this will
                   * affect subsequent directory copies too.
                   */
                  mode = GS_CP_MODE_COPY_ALL;
                }
              else
                did_link = TRUE;
            }
          if (!did_link)
            {
              GFileCopyFlags copyflags = G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS;
              if (mode == GS_CP_MODE_COPY_ALL)
                copyflags |= G_FILE_COPY_ALL_METADATA;
              if (!g_file_copy (src_child, dest_child, copyflags,
                                cancellable, NULL, NULL, error))
                goto out;
            }
        }
    }

  ret = TRUE;
 out:
  if (dest_dfd != -1)
    (void) close (dest_dfd);
  g_clear_object (&src_info);
  g_clear_object (&enumerator);
  g_clear_object (&dest_child);
  return ret;
}

/**
 * gs_shutil_cp_al_or_fallback:
 * @src: Source path
 * @dest: Destination path
 * @cancellable:
 * @error:
 *
 * Recursively copy path @src (which must be a directory) to the
 * target @dest.  If possible, hardlinks are used; if a hardlink is
 * not possible, a regular copy is created.  Any existing files are
 * overwritten.
 *
 * Returns: %TRUE on success
 */
gboolean
gs_shutil_cp_al_or_fallback (GFile         *src,
                             GFile         *dest,
                             GCancellable  *cancellable,
                             GError       **error)
{
  return cp_internal (src, dest, GS_CP_MODE_HARDLINK,
                      cancellable, error);
}

/**
 * gs_shutil_cp_a:
 * @src: Source path
 * @dest: Destination path
 * @cancellable:
 * @error:
 *
 * Recursively copy path @src (which must be a directory) to the
 * target @dest.  Any existing files are overwritten.
 *
 * Returns: %TRUE on success
 */
gboolean
gs_shutil_cp_a (GFile         *src,
                GFile         *dest,
                GCancellable  *cancellable,
                GError       **error)
{
  return cp_internal (src, dest, GS_CP_MODE_COPY_ALL,
                      cancellable, error);
}

static unsigned char
struct_stat_to_dt (struct stat *stbuf)
{
  if (S_ISDIR (stbuf->st_mode))
    return DT_DIR;
  if (S_ISREG (stbuf->st_mode))
    return DT_REG;
  if (S_ISCHR (stbuf->st_mode))
    return DT_CHR;
  if (S_ISBLK (stbuf->st_mode))
    return DT_BLK;
  if (S_ISFIFO (stbuf->st_mode))
    return DT_FIFO;
  if (S_ISLNK (stbuf->st_mode))
    return DT_LNK;
  if (S_ISSOCK (stbuf->st_mode))
    return DT_SOCK;
  return DT_UNKNOWN;
}

static gboolean
gs_shutil_rm_rf_children (DIR                *dir,
                          GCancellable       *cancellable,
                          GError            **error)
{
  gboolean ret = FALSE;
  int dfd;
  DIR *child_dir = NULL;
  struct dirent *dent;
  union dirent_storage buf;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    goto out;

  dfd = dirfd (dir);

  while (readdir_r (dir, &buf.dent, &dent) == 0)
    {
      if (dent == NULL)
        break;
      if (dent->d_type == DT_UNKNOWN)
        {
          struct stat stbuf;
          if (fstatat (dfd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW) == -1)
            {
              int errsv = errno;
              if (errsv == ENOENT)
                continue;
              else
                {
                  g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                                       g_strerror (errsv));
                  goto out;
                }
            }
          dent->d_type = struct_stat_to_dt (&stbuf);
          /* Assume unknown types are just treated like regular files */
          if (dent->d_type == DT_UNKNOWN)
            dent->d_type = DT_REG;
        }

      if (strcmp (dent->d_name, ".") == 0 || strcmp (dent->d_name, "..") == 0)
        continue;
          
      if (dent->d_type == DT_DIR)
        {
          int child_dfd = openat (dfd, dent->d_name, O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);

          if (child_dfd == -1)
            {
              if (errno == ENOENT)
                continue;
              else
                {
                  int errsv = errno;
                  g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                                       g_strerror (errsv));
                  goto out;
                }
            }

          child_dir = fdopendir (child_dfd);
          if (!child_dir)
            {
              int errsv = errno;
              g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                                   g_strerror (errsv));
              goto out;
            }

          if (!gs_shutil_rm_rf_children (child_dir, cancellable, error))
            goto out;

          if (unlinkat (dfd, dent->d_name, AT_REMOVEDIR) == -1)
            {
              int errsv = errno;
              g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                                   g_strerror (errsv));
              goto out;
            }

          (void) closedir (child_dir);
          child_dir = NULL;
        }
      else
        {
          if (unlinkat (dfd, dent->d_name, 0) == -1)
            {
              int errsv = errno;
              if (errno != ENOENT)
                {
                  g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                                       g_strerror (errsv));
                  goto out;
                }
            }
        }
    }
  /* Ignore error result from readdir_r, that's what others
   * seem to do =(
   */

  ret = TRUE;
 out:
  if (child_dir) (void) closedir (child_dir);
  return ret;
}

/**
 * gs_shutil_rm_rf:
 * @path: A file or directory
 * @cancellable:
 * @error:
 *
 * Recursively delete the filename referenced by @path; it may be a
 * file or directory.  No error is thrown if @path does not exist.
 */
gboolean
gs_shutil_rm_rf (GFile        *path,
                 GCancellable *cancellable,
                 GError      **error)
{
  gboolean ret = FALSE;
  int dfd = -1;
  DIR *d = NULL;

  /* With O_NOFOLLOW first */
  dfd = openat (AT_FDCWD, gs_file_get_path_cached (path),
                O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);

  if (dfd == -1)
    {
      int errsv = errno;
      if (errsv == ENOENT)
        {
          ;
        }
      else if (errsv == ENOTDIR || errsv == ELOOP)
        {
          if (!gs_file_unlink (path, cancellable, error))
            goto out;
        }
      else
        {
          g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                               g_strerror (errsv));
          goto out;
        }
    }
  else
    {
      d = fdopendir (dfd);
      if (!d)
        {
          int errsv = errno;
          g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                               g_strerror (errsv));
          goto out;
        }

      if (!gs_shutil_rm_rf_children (d, cancellable, error))
        goto out;

      if (rmdir (gs_file_get_path_cached (path)) == -1)
        {
          int errsv = errno;
          if (errsv != ENOENT)
            {
              g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                                   g_strerror (errsv));
              goto out;
            }
        }
    }

  ret = TRUE;
 out:
  if (d) (void) closedir (d);
  return ret;
}

