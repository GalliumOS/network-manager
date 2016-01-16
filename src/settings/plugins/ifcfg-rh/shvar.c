/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/*
 * shvar.c
 *
 * Implementation of non-destructively reading/writing files containing
 * only shell variable declarations and full-line comments.
 *
 * Copyright 1999,2000 Red Hat, Inc.
 *
 * This is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "shvar.h"

/* Open the file <name>, returning a shvarFile on success and NULL on failure.
 * Add a wrinkle to let the caller specify whether or not to create the file
 * (actually, return a structure anyway) if it doesn't exist.
 */
static shvarFile *
svOpenFileInternal (const char *name, gboolean create, GError **error)
{
	shvarFile *s = NULL;
	gboolean closefd = FALSE;
	int errsv = 0;

	s = g_slice_new0 (shvarFile);

	s->fd = -1;
	if (create)
		s->fd = open (name, O_RDWR); /* NOT O_CREAT */

	if (!create || s->fd == -1) {
		/* try read-only */
		s->fd = open (name, O_RDONLY); /* NOT O_CREAT */
		if (s->fd == -1)
			errsv = errno;
		else
			closefd = TRUE;
	}
	s->fileName = g_strdup (name);

	if (s->fd != -1) {
		struct stat buf;
		char *arena, *p, *q;
		ssize_t nread, total = 0;

		if (fstat (s->fd, &buf) < 0) {
			errsv = errno;
			goto bail;
		}
		arena = g_malloc (buf.st_size + 1);
		arena[buf.st_size] = '\0';

		while (total < buf.st_size) {
			nread = read (s->fd, arena + total, buf.st_size - total);
			if (nread == -1 && errno == EINTR)
				continue;
			if (nread <= 0) {
				errsv = errno;
				g_free (arena);
				goto bail;
			}
			total += nread;
		}

		/* we'd use g_strsplit() here, but we want a list, not an array */
		for (p = arena; (q = strchr (p, '\n')) != NULL; p = q + 1)
			s->lineList = g_list_append (s->lineList, g_strndup (p, q - p));
		g_free (arena);

		/* closefd is set if we opened the file read-only, so go ahead and
		 * close it, because we can't write to it anyway
		 */
		if (closefd) {
			close (s->fd);
			s->fd = -1;
		}

		return s;
	}

	if (create)
		return s;

 bail:
	if (s->fd != -1)
		close (s->fd);
	g_free (s->fileName);
	g_slice_free (shvarFile, s);

	g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errsv),
	             "Could not read file '%s': %s",
	             name, errsv ? strerror (errsv) : "Unknown error");
	return NULL;
}

/* Open the file <name>, return shvarFile on success, NULL on failure */
shvarFile *
svOpenFile (const char *name, GError **error)
{
	return svOpenFileInternal (name, FALSE, error);
}

/* Create a new file structure, returning actual data if the file exists,
 * and a suitable starting point if it doesn't.
 */
shvarFile *
svCreateFile (const char *name)
{
	return svOpenFileInternal (name, TRUE, NULL);
}

/* remove escaped characters in place */
void
svUnescape (char *s)
{
	size_t len, idx_rd = 0, idx_wr = 0;
	char c;

	len = strlen (s);
	if (len < 2) {
		if (s[0] == '\\')
			s[0] = '\0';
		return;
	}

	if ((s[0] == '"' || s[0] == '\'') && s[0] == s[len-1]) {
		if (len == 2) {
			s[0] = '\0';
			return;
		}
		if (len == 3) {
			if (s[1] == '\\') {
				s[0] = '\0';
			} else {
				s[0] = s[1];
				s[1] = '\0';
			}
			return;
		}
		s[--len] = '\0';
		idx_rd = 1;
	} else {
		/* seek for the first escape... */
		char *p = strchr (s, '\\');

		if (!p)
			return;
		if (p[1] == '\0') {
			p[0] = '\0';
			return;
		}
		idx_wr = idx_rd = (p - s);
	}

	/* idx_rd points to the first escape. Walk the string and shift the
	 * characters from idx_rd to idx_wr.
	 */
	while ((c = s[idx_rd++])) {
		if (c == '\\') {
			if (s[idx_rd] == '\0') {
				s[idx_wr] = '\0';
				return;
			}
			s[idx_wr++] = s[idx_rd++];
			continue;
		}
		s[idx_wr++] = c;
	}
	s[idx_wr] = '\0';
}

/* create a new string with all necessary characters escaped.
 * caller must free returned string
 */
static const char escapees[] = "\"'\\$~`";		/* must be escaped */
static const char spaces[] = " \t|&;()<>";		/* only require "" */
static const char newlines[] = "\n\r";			/* will be removed */

char *
svEscape (const char *s)
{
	char *new;
	int i, j, mangle = 0, space = 0, newline = 0;
	int newlen, slen;

	slen = strlen (s);

	for (i = 0; i < slen; i++) {
		if (strchr (escapees, s[i]))
			mangle++;
		if (strchr (spaces, s[i]))
			space++;
		if (strchr (newlines, s[i]))
			newline++;
	}
	if (!mangle && !space && !newline)
		return strdup (s);

	newlen = slen + mangle - newline + 3;	/* 3 is extra ""\0 */
	new = g_malloc (newlen);

	j = 0;
	new[j++] = '"';
	for (i = 0; i < slen; i++) {
		if (strchr (newlines, s[i]))
			continue;
		if (strchr (escapees, s[i])) {
			new[j++] = '\\';
		}
		new[j++] = s[i];
	}
	new[j++] = '"';
	new[j++] = '\0'
;
	g_assert (j == slen + mangle - newline + 3);

	return new;
}

/* Get the value associated with the key, and leave the current pointer
 * pointing at the line containing the value.  The char* returned MUST
 * be freed by the caller.
 */
char *
svGetValue (shvarFile *s, const char *key, gboolean verbatim)
{
	char *value = NULL;
	char *line;
	char *keyString;
	int len;

	g_return_val_if_fail (s != NULL, NULL);
	g_return_val_if_fail (key != NULL, NULL);

	keyString = g_strdup_printf ("%s=", key);
	len = strlen (keyString);

	for (s->current = s->lineList; s->current; s->current = s->current->next) {
		line = s->current->data;
		if (!strncmp (keyString, line, len)) {
			value = g_strdup (line + len);
			if (!verbatim)
				svUnescape (value);
			break;
		}
	}
	g_free (keyString);

	if (value && value[0]) {
		return value;
	} else {
		g_free (value);
		return NULL;
	}
}

/* return TRUE if <key> resolves to any truth value (e.g. "yes", "y", "true")
 * return FALSE if <key> resolves to any non-truth value (e.g. "no", "n", "false")
 * return <default> otherwise
 */
gboolean
svTrueValue (shvarFile *s, const char *key, gboolean def)
{
	char *tmp;
	gboolean returnValue = def;

	tmp = svGetValue (s, key, FALSE);
	if (!tmp)
		return returnValue;

	if (   !g_ascii_strcasecmp ("yes", tmp)
	    || !g_ascii_strcasecmp ("true", tmp)
	    || !g_ascii_strcasecmp ("t", tmp)
	    || !g_ascii_strcasecmp ("y", tmp))
		returnValue = TRUE;
	else if (   !g_ascii_strcasecmp ("no", tmp)
	         || !g_ascii_strcasecmp ("false", tmp)
	         || !g_ascii_strcasecmp ("f", tmp)
	         || !g_ascii_strcasecmp ("n", tmp))
		returnValue = FALSE;

	g_free (tmp);
	return returnValue;
}


/* Set the variable <key> equal to the value <value>.
 * If <key> does not exist, and the <current> pointer is set, append
 * the key=value pair after that line.  Otherwise, append the pair
 * to the bottom of the file.
 */
void
svSetValue (shvarFile *s, const char *key, const char *value, gboolean verbatim)
{
	char *newval = NULL, *oldval = NULL;
	char *keyValue;

	g_return_if_fail (s != NULL);
	g_return_if_fail (key != NULL);
	/* value may be NULL */

	if (value)
		newval = verbatim ? g_strdup (value) : svEscape (value);
	keyValue = g_strdup_printf ("%s=%s", key, newval ? newval : "");

	oldval = svGetValue (s, key, FALSE);

	if (!newval || !newval[0]) {
		/* delete value */
		if (oldval) {
			/* delete line */
			s->lineList = g_list_remove_link (s->lineList, s->current);
			g_list_free_1 (s->current);
			s->modified = TRUE;
		}
		goto bail; /* do not need keyValue */
	}

	if (!oldval) {
		/* append line */
		s->lineList = g_list_append (s->lineList, keyValue);
		s->modified = TRUE;
		goto end;
	}

	if (strcmp (oldval, newval) != 0) {
		/* change line */
		if (s->current)
			s->current->data = keyValue;
		else
			s->lineList = g_list_append (s->lineList, keyValue);
		s->modified = TRUE;
	}

 end:
	g_free (newval);
	g_free (oldval);
	return;

 bail:
	g_free (keyValue);
	goto end;
}

/* Write the current contents iff modified.  Returns FALSE on error
 * and TRUE on success.  Do not write if no values have been modified.
 * The mode argument is only used if creating the file, not if
 * re-writing an existing file, and is passed unchanged to the
 * open() syscall.
 */
gboolean
svWriteFile (shvarFile *s, int mode, GError **error)
{
	FILE *f;
	int tmpfd;

	if (s->modified) {
		if (s->fd == -1)
			s->fd = open (s->fileName, O_WRONLY | O_CREAT, mode);
		if (s->fd == -1) {
			int errsv = errno;

			g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errsv),
			             "Could not open file '%s' for writing: %s",
			             s->fileName, strerror (errsv));
			return FALSE;
		}
		if (ftruncate (s->fd, 0) < 0) {
			int errsv = errno;

			g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errsv),
			             "Could not overwrite file '%s': %s",
			             s->fileName, strerror (errsv));
			return FALSE;
		}

		tmpfd = dup (s->fd);
		if (tmpfd == -1) {
			int errsv = errno;

			g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errsv),
			             "Internal error writing file '%s': %s",
			             s->fileName, strerror (errsv));
			return FALSE;
		}
		f = fdopen (tmpfd, "w");
		fseek (f, 0, SEEK_SET);
		for (s->current = s->lineList; s->current; s->current = s->current->next) {
			char *line = s->current->data;
			fprintf (f, "%s\n", line);
		}
		fclose (f);
	}

	return TRUE;
}


/* Close the file descriptor (if open) and free the shvarFile. */
void
svCloseFile (shvarFile *s)
{
	g_return_if_fail (s != NULL);

	if (s->fd != -1)
		close (s->fd);

	g_free (s->fileName);
	g_list_free_full (s->lineList, g_free); /* implicitly frees s->current */
	g_slice_free (shvarFile, s);
}
