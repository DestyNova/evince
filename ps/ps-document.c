/* Ghostscript widget for GTK/GNOME
 * 
 * Copyright (C) 1998 - 2005 the Free Software Foundation
 * 
 * Authors: Jonathan Blandford, Jaka Mocnik
 * 
 * Based on code by: Federico Mena (Quartic), Szekeres Istvan (Pista)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */
 
#include "config.h"
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <gtk/gtk.h>
#include <gtk/gtkobject.h>
#include <gdk/gdkprivate.h>
#include <gdk/gdkx.h>
#include <gdk/gdk.h>
#include <glib/gi18n.h>
#include <X11/Intrinsic.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdio.h>
#include <math.h>

#include "ps-document.h"
#include "ev-debug.h"
#include "gsdefaults.h"

#ifdef HAVE_LOCALE_H
#   include <locale.h>
#endif

/* if POSIX O_NONBLOCK is not available, use O_NDELAY */
#if !defined(O_NONBLOCK) && defined(O_NDELAY)
#   define O_NONBLOCK O_NDELAY
#endif

#define MAX_BUFSIZE 1024

#define PS_DOCUMENT_IS_COMPRESSED(gs)       (PS_DOCUMENT(gs)->gs_filename_unc != NULL)
#define PS_DOCUMENT_GET_PS_FILE(gs)         (PS_DOCUMENT_IS_COMPRESSED(gs) ? \
                                        PS_DOCUMENT(gs)->gs_filename_unc : \
                                        PS_DOCUMENT(gs)->gs_filename)

GCond* pixbuf_cond = NULL;
GMutex* pixbuf_mutex = NULL;
GdkPixbuf *current_pixbuf = NULL;

enum {
	PROP_0,
	PROP_TITLE
};

/* structure to describe section of file to send to ghostscript */
struct record_list {
  FILE *fp;
  long begin;
  guint len;
  gboolean seek_needed;
  gboolean close;
  struct record_list *next;
};

static gboolean broken_pipe = FALSE;

/* Forward declarations */
static void ps_document_init(PSDocument * gs);
static void ps_document_class_init(PSDocumentClass * klass);
static void ps_document_finalize(GObject * object);
static void send_ps(PSDocument * gs, long begin, unsigned int len, gboolean close);
static void close_pipe(int p[2]);
static void output(gpointer data, gint source, GdkInputCondition condition);
static void input(gpointer data, gint source, GdkInputCondition condition);
static void stop_interpreter(PSDocument * gs);
static gint start_interpreter(PSDocument * gs);
static void ps_document_document_iface_init (EvDocumentIface *iface);
static gboolean ps_document_widget_event (GtkWidget *widget, GdkEvent *event, gpointer data);

static GObjectClass *parent_class = NULL;

static PSDocumentClass *gs_class = NULL;

static void
ps_document_init (PSDocument *gs)
{
	gs->bpixmap = NULL;

	gs->current_page = 0;
	gs->interpreter_pid = -1;

	gs->width = -1;
	gs->height = -1;
	gs->busy = FALSE;
	gs->gs_filename = 0;
	gs->gs_filename_unc = 0;

	broken_pipe = FALSE;

	gs->structured_doc = FALSE;
	gs->reading_from_pipe = FALSE;
	gs->send_filename_to_gs = FALSE;

	gs->doc = NULL;
	gs->loaded = FALSE;

	gs->interpreter_input = -1;
	gs->interpreter_output = -1;
	gs->interpreter_err = -1;
	gs->interpreter_input_id = 0;
	gs->interpreter_output_id = 0;
	gs->interpreter_error_id = 0;

	gs->ps_input = NULL;
	gs->input_buffer = NULL;
	gs->input_buffer_ptr = NULL;
	gs->bytes_left = 0;
	gs->buffer_bytes_left = 0;

	gs->page_x_offset = 0;
	gs->page_y_offset = 0;
	gs->zoom_factor = 1.0;

	gs->gs_status = _("No document loaded.");

	pixbuf_cond = g_cond_new ();
	pixbuf_mutex = g_mutex_new ();
}

static void
ps_document_set_property (GObject *object,
		          guint prop_id,
		          const GValue *value,
		          GParamSpec *pspec)
{
	switch (prop_id)

	{
		case PROP_TITLE:
			/* read only */
			break;
	}
}

static void
ps_document_get_property (GObject *object,
		          guint prop_id,
		          GValue *value,
		          GParamSpec *pspec)
{
	PSDocument *ps = PS_DOCUMENT (object);

	switch (prop_id)
	{
		case PROP_TITLE:
			if (ps->doc) {
				g_value_set_string (value, ps->doc->title);
			} else {
				g_value_set_string (value, NULL);
			}
			break;
	}
}

static void
ps_document_class_init(PSDocumentClass *klass)
{
	GObjectClass *object_class;

	object_class = (GObjectClass *) klass;
	parent_class = g_type_class_peek_parent (klass);
	gs_class = klass;

	object_class->finalize = ps_document_finalize;
	object_class->get_property = ps_document_get_property;
	object_class->set_property = ps_document_set_property;

	klass->gs_atom = gdk_atom_intern ("GHOSTVIEW", FALSE);
	klass->next_atom = gdk_atom_intern ("NEXT", FALSE);
	klass->page_atom = gdk_atom_intern ("PAGE", FALSE);
	klass->string_atom = gdk_atom_intern ("STRING", FALSE);

	g_object_class_override_property (object_class, PROP_TITLE, "title");
}

static void
push_pixbuf (PSDocument *gs)
{
	GdkColormap *cmap;
	GdkPixbuf *pixbuf;

	cmap = gdk_window_get_colormap (gs->pstarget);
	
	pixbuf =  gdk_pixbuf_get_from_drawable (NULL, gs->bpixmap, cmap,
				      	        0, 0, 0, 0,
					        gs->width, gs->height);
	g_mutex_lock (pixbuf_mutex);
	current_pixbuf = pixbuf;
	g_cond_signal (pixbuf_cond);
	g_mutex_unlock (pixbuf_mutex);

}

static void
interpreter_failed (PSDocument *gs, char *msg)
{
	LOG ("Interpreter failed %s", msg);

	push_pixbuf (gs);

	stop_interpreter (gs);
}

static void
ps_document_cleanup (PSDocument *gs)
{
	g_return_if_fail (gs != NULL);
	g_return_if_fail (PS_IS_DOCUMENT (gs));

	LOG ("Cleanup\n");

	stop_interpreter (gs);

	if (gs->gs_psfile) {
		fclose (gs->gs_psfile);
		gs->gs_psfile = NULL;
	}

	if (gs->gs_filename) {
		g_free (gs->gs_filename);
		gs->gs_filename = NULL;
	}

	if (gs->doc) {
		psfree (gs->doc);
		gs->doc = NULL;
	}

	if (gs->gs_filename_unc) {
		unlink(gs->gs_filename_unc);
		g_free(gs->gs_filename_unc);
		gs->gs_filename_unc = NULL;
	}

	gs->current_page = 0;
	gs->loaded = FALSE;
}

static gboolean
ps_document_widget_event (GtkWidget *widget, GdkEvent *event, gpointer data)
{
	PSDocument *gs = (PSDocument *) data;

	if(event->type != GDK_CLIENT_EVENT)
		return FALSE;

	gs->message_window = event->client.data.l[0];

	if (event->client.message_type == gs_class->page_atom) {
		LOG ("GS rendered the document");
		gs->busy = FALSE;

		push_pixbuf (gs);
	}

	return TRUE;
}

static void
ps_document_finalize (GObject * object)
{
	PSDocument *gs;

	g_return_if_fail (object != NULL);
	g_return_if_fail (PS_IS_DOCUMENT (object));

	LOG ("Finalize");

	gs = PS_DOCUMENT (object);

	ps_document_cleanup (gs);
	stop_interpreter (gs);

	if(gs->input_buffer) {
		g_free(gs->input_buffer);
		gs->input_buffer = NULL;
	}

	(*G_OBJECT_CLASS (parent_class)->finalize) (object);
}

static void
send_ps(PSDocument * gs, long begin, unsigned int len, gboolean close)
{
  struct record_list *ps_new;

  if(gs->interpreter_input < 0) {
    g_critical("No pipe to gs: error in send_ps().");
    return;
  }

  ps_new = (struct record_list *) g_malloc(sizeof(struct record_list));
  ps_new->fp = gs->gs_psfile;
  ps_new->begin = begin;
  ps_new->len = len;
  ps_new->seek_needed = TRUE;
  ps_new->close = close;
  ps_new->next = NULL;

  if(gs->input_buffer == NULL) {
    gs->input_buffer = g_malloc(MAX_BUFSIZE);
  }

  if(gs->ps_input == NULL) {
    gs->input_buffer_ptr = gs->input_buffer;
    gs->bytes_left = len;
    gs->buffer_bytes_left = 0;
    gs->ps_input = ps_new;
    gs->interpreter_input_id =
      gdk_input_add(gs->interpreter_input, GDK_INPUT_WRITE, input, gs);
  }
  else {
    struct record_list *p = gs->ps_input;
    while(p->next != NULL) {
      p = p->next;
    }
    p->next = ps_new;
  }
}

static float
get_xdpi (PSDocument *gs)
{
	return 25.4 * gdk_screen_width() / gdk_screen_width_mm();
}

static float
get_ydpi (PSDocument *gs)
{
	return 25.4 * gdk_screen_height() / gdk_screen_height_mm();
}

static void
setup_pixmap (PSDocument *gs)
{
	GdkGC *fill;
	GdkColor white = { 0, 0xFFFF, 0xFFFF, 0xFFFF };   /* pixel, r, g, b */
	GdkColormap *colormap;

        LOG ("Create our internal pixmap");

	if(gs->bpixmap) {
		gdk_drawable_unref(gs->bpixmap);
	}

	fill = gdk_gc_new (gs->pstarget);
	colormap = gdk_drawable_get_colormap (gs->pstarget);
	gdk_color_alloc (colormap, &white);
	gdk_gc_set_foreground (fill, &white);
	gs->bpixmap = gdk_pixmap_new (gs->pstarget, gs->width, gs->height, -1);
	gdk_draw_rectangle (gs->bpixmap, fill, TRUE,
                            0, 0, gs->width, gs->height);
}

static void
setup_page (PSDocument *gs)
{
	char buf[1024];
#ifdef HAVE_LOCALE_H
	char *savelocale;
#endif

	LOG ("Setup the page");

#ifdef HAVE_LOCALE_H
	/* gs needs floating point parameters with '.' as decimal point
	 * while some (european) locales use ',' instead, so we set the 
	 * locale for this snprintf to "C".
	 */
	savelocale = setlocale (LC_NUMERIC, "C");
#endif

	g_snprintf (buf, 1024, "%ld %d %d %d %d %d %f %f %d %d %d %d",
		    0L, gs->orientation * 90, gs->llx, gs->lly, gs->urx, gs->ury,
		    get_xdpi (gs) * gs->zoom_factor,
		    get_ydpi (gs) * gs->zoom_factor,
		    0, 0, 0, 0);
	LOG ("GS property %s", buf);

#ifdef HAVE_LOCALE_H
	setlocale(LC_NUMERIC, savelocale);
#endif
	gdk_property_change (gs->pstarget, gs_class->gs_atom, gs_class->string_atom,
			     8, GDK_PROP_MODE_REPLACE, (guchar *)buf, strlen(buf));
	gdk_flush ();
}

static void
close_pipe (int p[2])
{
	if (p[0] != -1) {
		close (p[0]);
	}
	if (p[1] != -1) {
		close (p[1]);
	}
}

static gboolean
is_interpreter_ready (PSDocument *gs)
{
	return (gs->interpreter_pid != -1 && !gs->busy && gs->ps_input == NULL);
}

static void
output(gpointer data, gint source, GdkInputCondition condition)
{
  char buf[MAX_BUFSIZE + 1], *msg;
  guint bytes = 0;
  PSDocument *gs = PS_DOCUMENT(data);

  if(source == gs->interpreter_output) {
    bytes = read(gs->interpreter_output, buf, MAX_BUFSIZE);
    if(bytes == 0) {            /* EOF occurred */
      close(gs->interpreter_output);
      gs->interpreter_output = -1;
      gdk_input_remove(gs->interpreter_output_id);
      return;
    }
    else if(bytes == -1) {
      /* trouble... */
      interpreter_failed(gs, NULL);
      return;
    }
    if(gs->interpreter_err == -1) {
      interpreter_failed(gs, NULL);
    }
  }
  else if(source == gs->interpreter_err) {
    bytes = read(gs->interpreter_err, buf, MAX_BUFSIZE);
    if(bytes == 0) {            /* EOF occurred */
      close(gs->interpreter_err);
      gs->interpreter_err = -1;
      gdk_input_remove(gs->interpreter_error_id);
      return;
    }
    else if(bytes == -1) {
      /* trouble... */
      interpreter_failed(gs, NULL);
      return;
    }
    if(gs->interpreter_output == -1) {
      interpreter_failed(gs, NULL);
    }
  }
  if(bytes > 0) {
    buf[bytes] = '\0';
    msg = g_strdup(buf);
    interpreter_failed (gs, msg);   
  }
}

static void
catchPipe(int i)
{
  broken_pipe = True;
}

static void
input(gpointer data, gint source, GdkInputCondition condition)
{
  PSDocument *gs = PS_DOCUMENT(data);
  int bytes_written;
  void (*oldsig) (int);
  oldsig = signal(SIGPIPE, catchPipe);

  LOG ("Input");

  do {
    if(gs->buffer_bytes_left == 0) {
      /* Get a new section if required */
      if(gs->ps_input && gs->bytes_left == 0) {
        struct record_list *ps_old = gs->ps_input;
        gs->ps_input = ps_old->next;
        if(ps_old->close && NULL != ps_old->fp)
          fclose(ps_old->fp);
        g_free((char *) ps_old);
      }
      /* Have to seek at the beginning of each section */
      if(gs->ps_input && gs->ps_input->seek_needed) {
        fseek(gs->ps_input->fp, gs->ps_input->begin, SEEK_SET);
        gs->ps_input->seek_needed = FALSE;
        gs->bytes_left = gs->ps_input->len;
      }

      if(gs->bytes_left > MAX_BUFSIZE) {
        gs->buffer_bytes_left =
          fread(gs->input_buffer, sizeof(char), MAX_BUFSIZE, gs->ps_input->fp);
      }
      else if(gs->bytes_left > 0) {
        gs->buffer_bytes_left =
          fread(gs->input_buffer,
                sizeof(char), gs->bytes_left, gs->ps_input->fp);
      }
      else {
        gs->buffer_bytes_left = 0;
      }
      if(gs->bytes_left > 0 && gs->buffer_bytes_left == 0) {
        interpreter_failed (gs, NULL); /* Error occurred */
      }
      gs->input_buffer_ptr = gs->input_buffer;
      gs->bytes_left -= gs->buffer_bytes_left;
    }

    if(gs->buffer_bytes_left > 0) {
      bytes_written = write(gs->interpreter_input,
                            gs->input_buffer_ptr, gs->buffer_bytes_left);

      if(broken_pipe) {
        interpreter_failed (gs, g_strdup(_("Broken pipe.")));
        broken_pipe = FALSE;
        interpreter_failed (gs, NULL);
      }
      else if(bytes_written == -1) {
        if((errno != EWOULDBLOCK) && (errno != EAGAIN)) {
          interpreter_failed (gs, NULL);   /* Something bad happened */
        }
      }
      else {
        gs->buffer_bytes_left -= bytes_written;
        gs->input_buffer_ptr += bytes_written;
      }
    }
  }
  while(gs->ps_input && gs->buffer_bytes_left == 0);

  signal(SIGPIPE, oldsig);

  if(gs->ps_input == NULL && gs->buffer_bytes_left == 0) {
    if(gs->interpreter_input_id != 0) {
      gdk_input_remove(gs->interpreter_input_id);
      gs->interpreter_input_id = 0;
    }
  }
}

static int
start_interpreter (PSDocument *gs)
{
	int std_in[2] = { -1, -1 };   /* pipe to interp stdin */
	int std_out[2];               /* pipe from interp stdout */
	int std_err[2];               /* pipe from interp stderr */

#define NUM_ARGS    100
#define NUM_GS_ARGS (NUM_ARGS - 20)
#define NUM_ALPHA_ARGS 10

	char *argv[NUM_ARGS], *dir, *gv_env;
	char **gs_args, **alpha_args = NULL;
	int argc = 0, i;

	LOG ("Start the interpreter");

	if(!gs->gs_filename)
		return 0;

	stop_interpreter(gs);

	/* set up the args... */
	gs_args = g_strsplit (gtk_gs_defaults_get_interpreter_cmd (), " ", NUM_GS_ARGS);
	for(i = 0; i < NUM_GS_ARGS && gs_args[i]; i++, argc++) {
		argv[argc] = gs_args[i];
	}

	alpha_args = g_strsplit (ALPHA_PARAMS, " ", NUM_ALPHA_ARGS);

	argv[argc++] = "-dNOPAUSE";
	argv[argc++] = "-dQUIET";
	argv[argc++] = "-dSAFER";

	/* set up the pipes */
	if (gs->send_filename_to_gs) {
		argv[argc++] = PS_DOCUMENT_GET_PS_FILE (gs);
		argv[argc++] = "-c";
		argv[argc++] = "quit";
	} else {
		argv[argc++] = "-";
	}

	argv[argc++] = NULL;

	if (!gs->reading_from_pipe && !gs->send_filename_to_gs) {
		if (pipe (std_in) == -1) {
			g_critical ("Unable to open pipe to Ghostscript.");
			return -1;
		}
	}

	if (pipe (std_out) == -1) {
		close_pipe (std_in);
		return -1;
	}

	if (pipe(std_err) == -1) {
		close_pipe (std_in);
		close_pipe (std_out);
		return -1;
	}

	gv_env = g_strdup_printf ("GHOSTVIEW=%ld %ld",
                           	  gdk_x11_drawable_get_xid (gs->pstarget),
				  gdk_x11_drawable_get_xid (gs->bpixmap));
	LOG ("Launching ghostview with env %s", gv_env);

	gs->busy = TRUE;
	gs->interpreter_pid = fork ();
	switch (gs->interpreter_pid) {
		case -1:                     /* error */
			close_pipe (std_in);
			close_pipe (std_out);
 			close_pipe (std_err);
			return -2;
			break;
		case 0:                      /* child */
			close (std_out[0]);
			dup2 (std_out[1], 1);
			close (std_out[1]);

			close (std_err[0]);
			dup2 (std_err[1], 2);
			close (std_err[1]);

			if (!gs->reading_from_pipe) {
				if (gs->send_filename_to_gs) {
					int stdinfd;
					/* just in case gs tries to read from stdin */
					stdinfd = open("/dev/null", O_RDONLY);
					if (stdinfd != 0) {
						dup2(stdinfd, 0);
						close(stdinfd);
					}
				} else {
					close (std_in[1]);
					dup2 (std_in[0], 0);
					close (std_in[0]);
				}
			}

			putenv(gv_env);

			/* change to directory where the input file is. This helps
			 * with postscript-files which include other files using
			 * a relative path */
			dir = g_path_get_dirname (gs->gs_filename);
			chdir (dir);
			g_free (dir);

			execvp (argv[0], argv);

			/* Notify error */
			g_critical ("Unable to execute [%s]\n", argv[0]);
			g_strfreev (gs_args);
			g_free (gv_env);
			g_strfreev (alpha_args);
			_exit (1);
			break;
		default:                     /* parent */
			if (!gs->send_filename_to_gs && !gs->reading_from_pipe) {
				int result;
				close (std_in[0]);
				/* use non-blocking IO for pipe to ghostscript */
				result = fcntl (std_in[1], F_GETFL, 0);
				fcntl (std_in[1], F_SETFL, result | O_NONBLOCK);
				gs->interpreter_input = std_in[1];
			} else {
				gs->interpreter_input = -1;
			}
			close (std_out[1]);

 			gs->interpreter_output = std_out[0];
			close (std_err[1]);
			gs->interpreter_err = std_err[0];
			gs->interpreter_output_id =
				gdk_input_add (std_out[0], GDK_INPUT_READ, output, gs);
			gs->interpreter_error_id =
				gdk_input_add (std_err[0], GDK_INPUT_READ, output, gs);
			break;
	}

	return TRUE;
}

static void
stop_interpreter(PSDocument * gs)
{
  if(gs->interpreter_pid > 0) {
    int status = 0;
    LOG ("Stop the interpreter");
    kill(gs->interpreter_pid, SIGTERM);
    while((wait(&status) == -1) && (errno == EINTR)) ;
    gs->interpreter_pid = -1;
    if(status == 1) {
      ps_document_cleanup(gs);
      gs->gs_status = _("Interpreter failed.");
    }
  }

  if(gs->interpreter_input >= 0) {
    close(gs->interpreter_input);
    gs->interpreter_input = -1;
    if(gs->interpreter_input_id != 0) {
      gdk_input_remove(gs->interpreter_input_id);
      gs->interpreter_input_id = 0;
    }
    while(gs->ps_input) {
      struct record_list *ps_old = gs->ps_input;
      gs->ps_input = gs->ps_input->next;
      if(ps_old->close && NULL != ps_old->fp)
        fclose(ps_old->fp);
      g_free((char *) ps_old);
    }
  }

  if(gs->interpreter_output >= 0) {
    close(gs->interpreter_output);
    gs->interpreter_output = -1;
    if(gs->interpreter_output_id) {
      gdk_input_remove(gs->interpreter_output_id);
      gs->interpreter_output_id = 0;
    }
  }

  if(gs->interpreter_err >= 0) {
    close(gs->interpreter_err);
    gs->interpreter_err = -1;
    if(gs->interpreter_error_id) {
      gdk_input_remove(gs->interpreter_error_id);
      gs->interpreter_error_id = 0;
    }
  }

  gs->busy = FALSE;
}

/* If file exists and is a regular file then return its length, else -1 */
static gint
file_length(const gchar * filename)
{
  struct stat stat_rec;

  if(filename && (stat(filename, &stat_rec) == 0)
     && S_ISREG(stat_rec.st_mode))
    return stat_rec.st_size;
  else
    return -1;
}

/* Test if file exists, is a regular file and its length is > 0 */
static gboolean
file_readable(const char *filename)
{
  return (file_length(filename) > 0);
}

/*
 * Decompress gs->gs_filename if necessary
 * Set gs->filename_unc to the name of the uncompressed file or NULL.
 * Error reporting via signal 'interpreter_message'
 * Return name of input file to use or NULL on error..
 */
static gchar *
check_filecompressed(PSDocument * gs)
{
  FILE *file;
  gchar buf[1024];
  gchar *filename, *filename_unc, *filename_err, *cmdline;
  const gchar *cmd;
  int fd;

  cmd = NULL;

  if((file = fopen(gs->gs_filename, "r"))
     && (fread(buf, sizeof(gchar), 3, file) == 3)) {
    if((buf[0] == '\037') && ((buf[1] == '\235') || (buf[1] == '\213'))) {
      /* file is gzipped or compressed */
      cmd = gtk_gs_defaults_get_ungzip_cmd();
    }
    else if(strncmp(buf, "BZh", 3) == 0) {
      /* file is compressed with bzip2 */
      cmd = gtk_gs_defaults_get_unbzip2_cmd();
    }
  }
  if(NULL != file)
    fclose(file);

  if(!cmd)
    return gs->gs_filename;

  /* do the decompression */
  filename = g_shell_quote(gs->gs_filename);
  filename_unc = g_strconcat(g_get_tmp_dir(), "/ggvXXXXXX", NULL);
  if((fd = mkstemp(filename_unc)) < 0) {
    g_free(filename_unc);
    g_free(filename);
    return NULL;
  }
  close(fd);
  filename_err = g_strconcat(g_get_tmp_dir(), "/ggvXXXXXX", NULL);
  if((fd = mkstemp(filename_err)) < 0) {
    g_free(filename_err);
    g_free(filename_unc);
    g_free(filename);
    return NULL;
  }
  close(fd);
  cmdline = g_strdup_printf("%s %s >%s 2>%s", cmd,
                            filename, filename_unc, filename_err);
  if((system(cmdline) == 0)
     && file_readable(filename_unc)
     && (file_length(filename_err) == 0)) {
    /* sucessfully uncompressed file */
    gs->gs_filename_unc = filename_unc;
  }
  else {
    /* report error */
    g_snprintf(buf, 1024, _("Error while decompressing file %s:\n"),
               gs->gs_filename);
    interpreter_failed (gs, buf);
    unlink(filename_unc);
    g_free(filename_unc);
    filename_unc = NULL;
  }
  unlink(filename_err);
  g_free(filename_err);
  g_free(cmdline);
  g_free(filename);
  return filename_unc;
}

static void
compute_dimensions (PSDocument *gs, int page)
{
	GtkGSPaperSize *paper_sizes = gtk_gs_defaults_get_paper_sizes ();
	int urx, ury, llx, lly;
	int width, height;
	int orientation;

	g_return_if_fail (PS_IS_DOCUMENT (gs));
	g_return_if_fail (gs->doc != NULL);
	g_return_if_fail (page >= 0);
	g_return_if_fail (gs->doc->numpages > page);

	orientation = GTK_GS_ORIENTATION_NONE;
	if (gs->structured_doc) {
		orientation = gs->doc->pages[gs->current_page].orientation;
	}
	if (orientation == GTK_GS_ORIENTATION_NONE) {
		orientation = GTK_GS_ORIENTATION_PORTRAIT;
	}

	if (gs->doc->pages && gs->doc->pages[page].size) {
		int page_size;

		page_size = gs->doc->pages[page].size - gs->doc->size;
		llx = lly = 0;
		urx = gs->doc->size[page_size].width;
		ury = gs->doc->size[page_size].height;
	} else if (gs->doc->pages &&
                   (gs->doc->pages[page].boundingbox[URX] >
                    gs->doc->pages[page].boundingbox[LLX]) &&
                   (gs->doc->pages[page].boundingbox[URY] >
                    gs->doc->pages[page].boundingbox[LLY])) {
		llx = gs->doc->pages[page].boundingbox[LLX];
		lly = gs->doc->pages[page].boundingbox[LLY];
		urx = gs->doc->pages[page].boundingbox[URX];
		ury = gs->doc->pages[page].boundingbox[URY];
	} else if ((gs->doc->boundingbox[URX] > gs->doc->boundingbox[LLX]) &&
                   (gs->doc->boundingbox[URY] > gs->doc->boundingbox[LLY])) {
		llx = gs->doc->boundingbox[LLX];
		lly = gs->doc->boundingbox[LLY];
		urx = gs->doc->boundingbox[URX];
		ury = gs->doc->boundingbox[URY];
	} else {
		/* Fallback to A4 */
		llx = lly = 0;
		urx = paper_sizes[12].width;
    		ury = paper_sizes[12].height;
	}

	switch (orientation) {
		case GTK_GS_ORIENTATION_PORTRAIT:
		case GTK_GS_ORIENTATION_UPSIDEDOWN:
			width = (urx - llx) / 72.0 * get_xdpi (gs) + 0.5;
			height = (ury - lly) / 72.0 * get_ydpi (gs) + 0.5;
			break;
		case GTK_GS_ORIENTATION_LANDSCAPE:
		case GTK_GS_ORIENTATION_SEASCAPE:
			width = (ury - lly) / 72.0 * get_xdpi (gs) + 0.5;
			height = (urx - llx) / 72.0 * get_ydpi (gs) + 0.5;
			break;
		default:
			width = height = 0;
			g_assert_not_reached ();
			break;
	}

	width = width * gs->zoom_factor;
	height = height * gs->zoom_factor;

	if (llx != gs->llx || lly != gs->lly ||
	    urx != gs->urx || ury != gs->ury ||
	    gs->width != width || gs->height != height ||
	    orientation != gs->orientation) {
		gs->llx = llx;
		gs->lly = lly;
		gs->urx = urx;
		gs->ury = ury;
		gs->width = width;
		gs->height = height;
		gs->orientation = orientation;
		gs->changed = TRUE;
	}
}

static gint
ps_document_enable_interpreter(PSDocument * gs)
{
  g_return_val_if_fail(gs != NULL, FALSE);
  g_return_val_if_fail(PS_IS_DOCUMENT(gs), FALSE);

  if(!gs->gs_filename)
    return 0;

  return start_interpreter(gs);
}

/* publicly accessible functions */

GType
ps_document_get_type(void)
{
  static GType gs_type = 0;
  if(!gs_type) {
    GTypeInfo gs_info = {
      sizeof(PSDocumentClass),
      (GBaseInitFunc) NULL,
      (GBaseFinalizeFunc) NULL,
      (GClassInitFunc) ps_document_class_init,
      (GClassFinalizeFunc) NULL,
      NULL,                     /* class_data */
      sizeof(PSDocument),
      0,                        /* n_preallocs */
      (GInstanceInitFunc) ps_document_init
    };

    static const GInterfaceInfo document_info =
    {
        (GInterfaceInitFunc) ps_document_document_iface_init,
        NULL,
        NULL
    };

    gs_type = g_type_register_static(G_TYPE_OBJECT,
                                     "PSDocument", &gs_info, 0);

    g_type_add_interface_static (gs_type,
                                 EV_TYPE_DOCUMENT,
                                 &document_info);
  }
  return gs_type;


}

static gboolean
document_load(PSDocument * gs, const gchar * fname)
{
  g_return_val_if_fail(gs != NULL, FALSE);
  g_return_val_if_fail(PS_IS_DOCUMENT(gs), FALSE);

  LOG ("Load the document");

  /* clean up previous document */
  ps_document_cleanup(gs);

  if(fname == NULL) {
    gs->gs_status = "";
    return FALSE;
  }

  /* prepare this document */
  gs->structured_doc = FALSE;
  gs->send_filename_to_gs = TRUE;
  gs->current_page = 0;
  gs->loaded = FALSE;
  if(*fname == '/') {
    /* an absolute path */
    gs->gs_filename = g_strdup(fname);
  }
  else {
    /* path relative to our cwd: make it absolute */
    gchar *cwd = g_get_current_dir();
    gs->gs_filename = g_strconcat(cwd, "/", fname, NULL);
    g_free(cwd);
  }

  if((gs->reading_from_pipe = (strcmp(fname, "-") == 0))) {
    gs->send_filename_to_gs = FALSE;
  }
  else {
    /*
     * We need to make sure that the file is loadable/exists!
     * otherwise we want to exit without loading new stuff...
     */
    gchar *filename = NULL;

    if(!file_readable(fname)) {
      gchar buf[1024];
      g_snprintf(buf, 1024, _("Cannot open file %s.\n"), fname);
      interpreter_failed (gs, buf);
      gs->gs_status = _("File is not readable.");
    }
    else {
      filename = check_filecompressed(gs);
    }

    if(!filename || (gs->gs_psfile = fopen(filename, "r")) == NULL) {
      interpreter_failed (gs, NULL);
      ps_document_cleanup(gs);
      return FALSE;
    }

    /* we grab the vital statistics!!! */
    gs->doc = psscan(gs->gs_psfile, TRUE, filename);

    g_object_notify (G_OBJECT (gs), "title");

    if(gs->doc == NULL) {
      /* File does not seem to be a Postscript one */
      gchar buf[1024];
      g_snprintf(buf, 1024, _("Error while scanning file %s\n"), fname);
      interpreter_failed (gs, buf);
      ps_document_cleanup(gs);
      gs->gs_status = _("The file is not a PostScript document.");
      return FALSE;
    }

    if((!gs->doc->epsf && gs->doc->numpages > 0) ||
       (gs->doc->epsf && gs->doc->numpages > 1)) {
      gs->structured_doc = TRUE;
      gs->send_filename_to_gs = FALSE;
    }
  }
  gs->loaded = TRUE;
  compute_dimensions (gs, gs->current_page);

  gs->gs_status = _("Document loaded.");

  return gs->loaded;
}


static gboolean
ps_document_next_page (PSDocument *gs)
{
	XEvent event;

	LOG ("Make ghostscript render next page");

	g_return_val_if_fail (PS_IS_DOCUMENT(gs), FALSE);
	g_return_val_if_fail (gs->interpreter_pid != 0, FALSE);
	g_return_val_if_fail (gs->busy != TRUE, FALSE);

	gs->busy = TRUE;

	event.xclient.type = ClientMessage;
	event.xclient.display = gdk_display;
	event.xclient.window = gs->message_window;
	event.xclient.message_type = gdk_x11_atom_to_xatom(gs_class->next_atom);
	event.xclient.format = 32;

	gdk_error_trap_push ();
	XSendEvent (gdk_display, gs->message_window, FALSE, 0, &event);
	gdk_flush ();
	gdk_error_trap_pop ();

	return TRUE;
}

static gboolean
render_page (PSDocument *gs)
{
	g_return_val_if_fail(gs != NULL, FALSE);
	g_return_val_if_fail(PS_IS_DOCUMENT(gs), FALSE);

	if(!gs->gs_filename) {
		return FALSE;
	}

	if (gs->structured_doc && gs->doc) {
		LOG ("It's a structured document, let's send one page to gs");

		if (is_interpreter_ready (gs)) {
			ps_document_next_page (gs);
		} else {
			ps_document_enable_interpreter (gs);
			send_ps (gs, gs->doc->beginprolog, gs->doc->lenprolog, FALSE);
			send_ps (gs, gs->doc->beginsetup, gs->doc->lensetup, FALSE);
		}

		send_ps (gs, gs->doc->pages[gs->current_page].begin,
			 gs->doc->pages[gs->current_page].len, FALSE);
	} else {
		/* Unstructured document
		 *
		 * In the case of non structured documents,
		 * GS read the PS from the  actual file (via command
		 * line. Hence, ggv only send a signal next page.
		 * If ghostview is not running it is usually because
		 * the last page of the file was displayed. In that
		 * case, ggv restarts GS again and the first page is displayed.
		 */

		LOG ("It's an unstructured document, gs will just read the file");

		if (!is_interpreter_ready (gs)) {
			ps_document_enable_interpreter(gs);
		}
		ps_document_next_page(gs);
	}

	return TRUE;
}

static gboolean
ps_document_load (EvDocument  *document,
		  const char  *uri,
		  GError     **error)
{
	gboolean result;
	char *filename;

	filename = g_filename_from_uri (uri, NULL, error);
	if (!filename)
		return FALSE;

	result = document_load (PS_DOCUMENT (document), filename);
	if (!result) {
		g_set_error (error, G_FILE_ERROR,
			     G_FILE_ERROR_FAILED,
			     "Failed to load document '%s'\n",
			     uri);
	}

	g_free (filename);

	return result;
}

static gboolean
ps_document_save (EvDocument  *document,
		  const char  *uri,
		  GError     **error)
{
	g_warning ("ps_document_save not implemented"); /* FIXME */
	return TRUE;
}

static int
ps_document_get_n_pages (EvDocument  *document)
{
	PSDocument *ps = PS_DOCUMENT (document);

	g_return_val_if_fail (ps != NULL, -1);

	if (!ps->gs_filename || !ps->doc) {
		return -1;
	}

	return ps->structured_doc ? ps->doc->numpages : 1;
}

static void
ps_document_set_page (EvDocument  *document,
		       int          page)
{
	PSDocument *gs = PS_DOCUMENT (document);

	LOG ("Set document page %d\n", page);

	gs->current_page = page - 1;
	compute_dimensions (gs, page);
}

static int
ps_document_get_page (EvDocument  *document)
{
	PSDocument *ps = PS_DOCUMENT (document);

	g_return_val_if_fail (ps != NULL, -1);

	return ps->current_page + 1;
}

static void
ps_document_set_scale (EvDocument  *document,
			double       scale)
{
	PSDocument *gs = PS_DOCUMENT (document);

	gs->zoom_factor = scale;
	compute_dimensions (gs, gs->current_page);
}

static void
ps_document_set_page_offset (EvDocument  *document,
			      int          x,
			      int          y)
{
	PSDocument *gs = PS_DOCUMENT (document);

	gs->page_x_offset = x;
	gs->page_y_offset = y;
}

static void
ps_document_get_page_size (EvDocument   *document,
			   int           page,
			   int          *width,
			   int          *height)
{
	/* Post script documents never vary in size */

	PSDocument *gs = PS_DOCUMENT (document);

	if (width) {
		*width = gs->width;
	}

	if (height) {
		*height = gs->height;
	}
}

static char *
ps_document_get_text (EvDocument *document, GdkRectangle *rect)
{
	g_warning ("ps_document_get_text not implemented"); /* FIXME ? */
	return NULL;
}

static EvLink *
ps_document_get_link (EvDocument *document,
		      int         x,
		      int	  y)
{
	return NULL;
}

static gboolean
render_pixbuf_idle (EvDocument *document)
{
	PSDocument *gs = PS_DOCUMENT (document);

	if (gs->pstarget == NULL) {
		GtkWidget *widget;

		widget = gtk_window_new (GTK_WINDOW_POPUP);
	        gtk_widget_realize (widget);
		gs->pstarget = widget->window;

	        g_assert (gs->pstarget != NULL);

		g_signal_connect (widget, "event",
			    	  G_CALLBACK (ps_document_widget_event),
			          gs);
	}

	if (gs->changed) {
		stop_interpreter (gs);
		setup_pixmap (gs);
		setup_page (gs);
		gs->changed = FALSE;
	}

	render_page (PS_DOCUMENT (document));

	return FALSE;
}

static GdkPixbuf *
ps_document_render_pixbuf (EvDocument *document)
{
	GdkPixbuf *pixbuf;

	g_idle_add ((GSourceFunc)render_pixbuf_idle, document);

	g_mutex_lock (pixbuf_mutex);
	while (!current_pixbuf)
		g_cond_wait (pixbuf_cond, pixbuf_mutex);
	pixbuf = current_pixbuf;
	current_pixbuf = NULL;
	g_mutex_unlock (pixbuf_mutex);

	LOG ("Pixbuf rendered %p\n", pixbuf);

	return pixbuf;
}

static void
ps_document_document_iface_init (EvDocumentIface *iface)
{
	iface->load = ps_document_load;
	iface->save = ps_document_save;
	iface->get_text = ps_document_get_text;
	iface->get_link = ps_document_get_link;
	iface->get_n_pages = ps_document_get_n_pages;
	iface->set_page = ps_document_set_page;
	iface->get_page = ps_document_get_page;
	iface->set_scale = ps_document_set_scale;
	iface->set_page_offset = ps_document_set_page_offset;
	iface->get_page_size = ps_document_get_page_size;
	iface->render_pixbuf = ps_document_render_pixbuf;
}
