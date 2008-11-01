/*
 *  Copyright (C) 2002 Jorn Baayen
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 *  $Id$
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <errno.h>

#if WITH_GNOME
#include <libgnome/gnome-init.h>
#endif

#include "ev-file-helpers.h"

static gchar *dot_dir = NULL;
static gchar *tmp_dir = NULL;
static gint   count = 0;

static gboolean
ensure_dir_exists (const char *dir)
{
	if (g_file_test (dir, G_FILE_TEST_IS_DIR))
		return TRUE;
	
	if (g_mkdir_with_parents (dir, 488) == 0)
		return TRUE;

	if (errno == EEXIST)
		return g_file_test (dir, G_FILE_TEST_IS_DIR);
	
	g_warning ("Failed to create directory %s: %s", dir, strerror (errno));
	return FALSE;
}

const gchar *
ev_dot_dir (void)
{
	if (dot_dir == NULL) {
		gboolean exists;

		dot_dir = g_build_filename (g_get_home_dir (),
					    ".gnome2",
					    "evince",
					    NULL);

		exists = ensure_dir_exists (dot_dir);
		if (!exists)
			exit (1);
	}

	return dot_dir;
}

const gchar *
ev_tmp_dir (void)
{
	if (tmp_dir == NULL) {
		gboolean exists;
		gchar   *dirname;

		dirname = g_strdup_printf ("evince-%u", getpid ());
		tmp_dir = g_build_filename (g_get_tmp_dir (),
					    dirname,
					    NULL);
		g_free (dirname);

		exists = ensure_dir_exists (tmp_dir);
		g_assert (exists);
	}

	return tmp_dir;
}

void
ev_file_helpers_init (void)
{
}

void
ev_file_helpers_shutdown (void)
{	
	if (tmp_dir != NULL)	
		g_rmdir (tmp_dir);

	g_free (tmp_dir);
	g_free (dot_dir);

	dot_dir = NULL;
	tmp_dir = NULL;
}

GFile *
ev_tmp_file_get (const gchar *prefix)
{
	gchar *path;
	GFile *file;

	path = ev_tmp_filename (prefix);
	file = g_file_new_for_path (path);
	
	g_free (path);
	
	return file;
}

gchar * 
ev_tmp_filename (const gchar *prefix)
{
	gchar *basename;
	gchar *filename = NULL;

	do {
		if (filename != NULL)
			g_free (filename);
			
		basename = g_strdup_printf ("%s-%d",
					    prefix ? prefix : "document",
					    count ++);
		
		filename = g_build_filename (ev_tmp_dir (),
					     basename, NULL);
		
		g_free (basename);
	} while (g_file_test (filename, G_FILE_TEST_EXISTS));
			
	return filename;
}

/* Remove a local temp file created by evince */
void
ev_tmp_filename_unlink (const gchar *filename)
{
	const gchar *tempdir;
	
	if (!filename)
		return;

	tempdir = g_get_tmp_dir ();
	if (g_ascii_strncasecmp (filename, tempdir, strlen (tempdir)) == 0) {
		g_unlink (filename);
	}
}

void
ev_tmp_file_unlink (GFile *file)
{
	gboolean res;

	if (!file)
		return;
	
	res = g_file_delete (file, NULL, NULL);
	if (!res) {
		char *uri;
		
		uri = g_file_get_uri (file);
		g_warning ("Unable to delete temp file %s\n", uri);
		g_free (uri);
	}
}

void
ev_tmp_uri_unlink (const gchar *uri)
{
	GFile *file;
	
	if (!uri)
		return;
	
	file = g_file_new_for_uri (uri);
	if (!g_file_is_native (file)) {
		g_warning ("Attempting to delete non native uri: %s\n", uri);
		g_object_unref (file);
		return;
	}
	
	ev_tmp_file_unlink (file);
	g_object_unref (file);
}

gboolean
ev_xfer_uri_simple (const char *from,
		    const char *to,
		    GError     **error)
{
	GFile *source_file;
	GFile *target_file;
	GError *ioerror = NULL;
	gboolean result;
	
	if (!from)
		return FALSE;
	
	source_file = g_file_new_for_uri (from);
	target_file = g_file_new_for_uri (to);
	
	result = g_file_copy (source_file, target_file,
			      G_FILE_COPY_OVERWRITE,
			      NULL, NULL, NULL, &ioerror);

	g_object_unref (target_file);
	g_object_unref (source_file);
    
	if (!result) {
		g_propagate_error (error, ioerror);
	}
	return result;

}

/* Compressed files support */
#define BZIPCOMMAND "bzip2"
#define GZIPCOMMAND "gzip"
#define N_ARGS      4
#define BUFFER_SIZE 1024

static gchar *
compression_run (const gchar       *uri,
		 EvCompressionType  type,
		 gboolean           compress, 
		 GError           **error)
{
	gchar *argv[N_ARGS];
	gchar *uri_dst = NULL;
	gchar *filename, *filename_dst;
	gchar *cmd;
	gint   fd, pout;

	if (type == EV_COMPRESSION_NONE)
		return NULL;

	cmd = g_find_program_in_path ((type == EV_COMPRESSION_BZIP2) ? BZIPCOMMAND : GZIPCOMMAND);
	if (!cmd)
		return NULL;

	filename = g_filename_from_uri (uri, NULL, NULL);
	if (!filename) {
		g_free (cmd);
		return NULL;
	}
	
	filename_dst = g_build_filename (ev_tmp_dir (), "evinceXXXXXX", NULL);
	fd = g_mkstemp (filename_dst);
	if (fd < 0) {
		g_free (cmd);
		g_free (filename);
		g_free (filename_dst);
		return NULL;
	}

	argv[0] = cmd;
	argv[1] = compress ? "-c" : "-cd";
	argv[2] = filename;
	argv[3] = NULL;

	if (g_spawn_async_with_pipes (NULL, argv, NULL,
				      G_SPAWN_STDERR_TO_DEV_NULL,
				      NULL, NULL, NULL,
				      NULL, &pout, NULL, error)) {
		GIOChannel *in, *out;
		gchar buf[BUFFER_SIZE];
		GIOStatus read_st, write_st;
		gsize bytes_read, bytes_written;

		in = g_io_channel_unix_new (pout);
		g_io_channel_set_encoding (in, NULL, NULL);
		out = g_io_channel_unix_new (fd);
		g_io_channel_set_encoding (out, NULL, NULL);

		do {
			read_st = g_io_channel_read_chars (in, buf,
							   BUFFER_SIZE,
							   &bytes_read,
							   error);
			if (read_st == G_IO_STATUS_NORMAL) {
				write_st = g_io_channel_write_chars (out, buf,
								     bytes_read,
								     &bytes_written,
								     error);
				if (write_st == G_IO_STATUS_ERROR)
					break;
			} else if (read_st == G_IO_STATUS_ERROR) {
				break;
			}
		} while (bytes_read > 0);

		g_io_channel_unref (in);
		g_io_channel_unref (out);
	}

	close (fd);

	if (*error == NULL) {
		uri_dst = g_filename_to_uri (filename_dst, NULL, NULL);
	}

	g_free (cmd);
	g_free (filename);
	g_free (filename_dst);

	return uri_dst;
}

gchar *
ev_file_uncompress (const gchar       *uri,
		    EvCompressionType  type,
		    GError           **error)
{
	g_return_val_if_fail (uri != NULL, NULL);

	return compression_run (uri, type, FALSE, error);
}

gchar *
ev_file_compress (const gchar       *uri,
		  EvCompressionType  type,
		  GError           **error)
{
	g_return_val_if_fail (uri != NULL, NULL);

	return compression_run (uri, type, TRUE, error);
}
