/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012,2013 Colin Walters <walters@verbum.org>.
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

#ifndef __LIBGSYSTEM__
#define __LIBGSYSTEM__

#include <gio/gio.h>

G_BEGIN_DECLS

#define gs_transfer_out_value(outp, srcp) G_STMT_START {   \
  if (outp)                                                \
    {                                                      \
      *(outp) = *(srcp);                                   \
      *(srcp) = NULL;                                      \
    }                                                      \
  } G_STMT_END;

#include <gsystem-console.h>
#include <gsystem-file-utils.h>
#include <gsystem-shutil.h>
#if GLIB_CHECK_VERSION(2,34,0)
#include <gsystem-subprocess.h>
#endif
#include <gsystem-log.h>
#ifndef _GSYSTEM_NO_LOCAL_ALLOC
#include <gsystem-local-alloc.h>
#endif

G_END_DECLS

#endif
