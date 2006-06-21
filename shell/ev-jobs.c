#include "ev-jobs.h"
#include "ev-job-queue.h"
#include "ev-document-thumbnails.h"
#include "ev-document-links.h"
#include "ev-document-factory.h"
#include "ev-file-helpers.h"
#include "ev-document-fonts.h"
#include "ev-selection.h"
#include "ev-async-renderer.h"

#include <libgnomevfs/gnome-vfs-uri.h>
#include <libgnomevfs/gnome-vfs-utils.h>
#include <libgnomevfs/gnome-vfs-ops.h>
#include <libgnomevfs/gnome-vfs-xfer.h>

static void ev_job_init                 (EvJob               *job);
static void ev_job_class_init           (EvJobClass          *class);
static void ev_job_links_init           (EvJobLinks          *job);
static void ev_job_links_class_init     (EvJobLinksClass     *class);
static void ev_job_render_init          (EvJobRender         *job);
static void ev_job_render_class_init    (EvJobRenderClass    *class);
static void ev_job_thumbnail_init       (EvJobThumbnail      *job);
static void ev_job_thumbnail_class_init (EvJobThumbnailClass *class);
static void ev_job_xfer_init    	(EvJobXfer	     *job);
static void ev_job_xfer_class_init 	(EvJobXferClass	     *class);

enum
{
	FINISHED,
	LAST_SIGNAL
};

static guint job_signals[LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (EvJob, ev_job, G_TYPE_OBJECT)
G_DEFINE_TYPE (EvJobLinks, ev_job_links, EV_TYPE_JOB)
G_DEFINE_TYPE (EvJobRender, ev_job_render, EV_TYPE_JOB)
G_DEFINE_TYPE (EvJobThumbnail, ev_job_thumbnail, EV_TYPE_JOB)
G_DEFINE_TYPE (EvJobFonts, ev_job_fonts, EV_TYPE_JOB)
G_DEFINE_TYPE (EvJobXfer, ev_job_xfer, EV_TYPE_JOB)

static void ev_job_init (EvJob *job) { /* Do Nothing */ }

static void
ev_job_dispose (GObject *object)
{
	EvJob *job;

	job = EV_JOB (object);

	if (job->document) {
		g_object_unref (job->document);
		job->document = NULL;
	}

	(* G_OBJECT_CLASS (ev_job_parent_class)->dispose) (object);
}

static void
ev_job_class_init (EvJobClass *class)
{
	GObjectClass *oclass;

	oclass = G_OBJECT_CLASS (class);

	oclass->dispose = ev_job_dispose;

	job_signals [FINISHED] =
		g_signal_new ("finished",
			      EV_TYPE_JOB,
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (EvJobClass, finished),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);
}


static void ev_job_links_init (EvJobLinks *job) { /* Do Nothing */ }

static void
ev_job_links_dispose (GObject *object)
{
	EvJobLinks *job;

	job = EV_JOB_LINKS (object);

	if (job->model) {
		g_object_unref (job->model);
		job->model = NULL;
	}

	(* G_OBJECT_CLASS (ev_job_links_parent_class)->dispose) (object);
}

static void
ev_job_links_class_init (EvJobLinksClass *class)
{
	GObjectClass *oclass;

	oclass = G_OBJECT_CLASS (class);

	oclass->dispose = ev_job_links_dispose;
}


static void ev_job_render_init (EvJobRender *job) { /* Do Nothing */ }

static void
ev_job_render_dispose (GObject *object)
{
	EvJobRender *job;

	job = EV_JOB_RENDER (object);

	if (job->pixbuf) {
		g_object_unref (job->pixbuf);
		job->pixbuf = NULL;
	}

	if (job->rc) {
		g_object_unref (job->rc);
		job->rc = NULL;
	}

	if (job->selection) {
		g_object_unref (job->selection);
		job->selection = NULL;
	}

	if (job->selection_region) {
		gdk_region_destroy (job->selection_region);
		job->selection_region = NULL;
	}

	(* G_OBJECT_CLASS (ev_job_render_parent_class)->dispose) (object);
}

static void
ev_job_render_class_init (EvJobRenderClass *class)
{
	GObjectClass *oclass;

	oclass = G_OBJECT_CLASS (class);

	oclass->dispose = ev_job_render_dispose;
}

static void ev_job_thumbnail_init (EvJobThumbnail *job) { /* Do Nothing */ }

static void
ev_job_thumbnail_dispose (GObject *object)
{
	EvJobThumbnail *job;

	job = EV_JOB_THUMBNAIL (object);

	if (job->thumbnail) {
		g_object_unref (job->thumbnail);
		job->thumbnail = NULL;
	}

	(* G_OBJECT_CLASS (ev_job_thumbnail_parent_class)->dispose) (object);
}

static void
ev_job_thumbnail_class_init (EvJobThumbnailClass *class)
{
	GObjectClass *oclass;

	oclass = G_OBJECT_CLASS (class);

	oclass->dispose = ev_job_thumbnail_dispose;
}

/* Public functions */
void
ev_job_finished (EvJob *job)
{
	g_return_if_fail (EV_IS_JOB (job));

	g_signal_emit (job, job_signals[FINISHED], 0);
}

EvJob *
ev_job_links_new (EvDocument *document)
{
	EvJob *job;

	job = g_object_new (EV_TYPE_JOB_LINKS, NULL);
	job->document = g_object_ref (document);

	return job;
}

void
ev_job_links_run (EvJobLinks *job)
{
	g_return_if_fail (EV_IS_JOB_LINKS (job));

	ev_document_doc_mutex_lock ();
	job->model = ev_document_links_get_links_model (EV_DOCUMENT_LINKS (EV_JOB (job)->document));
	EV_JOB (job)->finished = TRUE;
	ev_document_doc_mutex_unlock ();
}


EvJob *
ev_job_render_new (EvDocument      *document,
		   EvRenderContext *rc,
		   gint             width,
		   gint             height,
		   EvRectangle     *selection_points,
		   GdkColor        *text,
		   GdkColor        *base,
		   gboolean 	    include_form,
		   gboolean         include_links,
		   gboolean         include_text,
		   gboolean         include_selection)
{
	EvJobRender *job;

	g_return_val_if_fail (EV_IS_RENDER_CONTEXT (rc), NULL);
	if (include_selection)
		g_return_val_if_fail (selection_points != NULL, NULL);

	job = g_object_new (EV_TYPE_JOB_RENDER, NULL);

	EV_JOB (job)->document = g_object_ref (document);
	job->rc = g_object_ref (rc);
	job->target_width = width;
	job->target_height = height;
	job->text = *text;
	job->base = *base;
	job->include_form = include_form;
	job->include_links = include_links;
	job->include_text = include_text;
	job->include_selection = include_selection;

	if (include_selection)
		job->selection_points = *selection_points;

	if (EV_IS_ASYNC_RENDERER (document)) {	
		EV_JOB (job)->async = TRUE;
	}

	return EV_JOB (job);
}

static void
render_finished_cb (EvDocument *document, GdkPixbuf *pixbuf, EvJobRender *job)
{
	g_signal_handlers_disconnect_by_func (EV_JOB (job)->document,
					      render_finished_cb, job);

	EV_JOB (job)->finished = TRUE;
	job->pixbuf = g_object_ref (pixbuf);
	ev_job_finished (EV_JOB (job));
}

void
ev_job_render_run (EvJobRender *job)
{
	g_return_if_fail (EV_IS_JOB_RENDER (job));

	ev_document_doc_mutex_lock ();

	if (EV_JOB (job)->async) {
		EvAsyncRenderer *renderer = EV_ASYNC_RENDERER (EV_JOB (job)->document);
		ev_async_renderer_render_pixbuf (renderer, job->rc->page, job->rc->scale,
						 job->rc->rotation);
		g_signal_connect (EV_JOB (job)->document, "render_finished",
				  G_CALLBACK (render_finished_cb), job);
	} else {
		job->pixbuf = ev_document_render_pixbuf (EV_JOB (job)->document, job->rc);
		if (job->include_form)
			job->form_field_mapping = ev_document_get_form_field_mapping(EV_JOB(job)->document, job->rc->page);
		if (job->include_links && EV_IS_DOCUMENT_LINKS (EV_JOB (job)->document))
			job->link_mapping =
				ev_document_links_get_links (EV_DOCUMENT_LINKS (EV_JOB (job)->document),
							     job->rc->page);
		if (job->include_text && EV_IS_SELECTION (EV_JOB (job)->document))
			job->text_mapping =
				ev_selection_get_selection_map (EV_SELECTION (EV_JOB (job)->document),
								job->rc);
		if (job->include_selection && EV_IS_SELECTION (EV_JOB (job)->document)) {
			ev_selection_render_selection (EV_SELECTION (EV_JOB (job)->document),
						       job->rc,
						       &(job->selection),
						       &(job->selection_points),
						       NULL,
						       &(job->text), &(job->base));
			job->selection_region =
				ev_selection_get_selection_region (EV_SELECTION (EV_JOB (job)->document),
								   job->rc,
								   &(job->selection_points));
		}

		EV_JOB (job)->finished = TRUE;
	}

	ev_document_doc_mutex_unlock ();
}

EvJob *
ev_job_thumbnail_new (EvDocument   *document,
		      gint          page,
		      int           rotation,
		      gint          requested_width)
{
	EvJobThumbnail *job;

	job = g_object_new (EV_TYPE_JOB_THUMBNAIL, NULL);

	EV_JOB (job)->document = g_object_ref (document);
	job->page = page;
	job->rotation = rotation;
	job->requested_width = requested_width;

	return EV_JOB (job);
}

void
ev_job_thumbnail_run (EvJobThumbnail *job)
{
	g_return_if_fail (EV_IS_JOB_THUMBNAIL (job));

	ev_document_doc_mutex_lock ();

	job->thumbnail =
		ev_document_thumbnails_get_thumbnail (EV_DOCUMENT_THUMBNAILS (EV_JOB (job)->document),
						      job->page,
						      job->rotation,
						      job->requested_width,
						      TRUE);
	EV_JOB (job)->finished = TRUE;

	ev_document_doc_mutex_unlock ();
}

static void ev_job_fonts_init (EvJobFonts *job) { /* Do Nothing */ }

static void ev_job_fonts_class_init (EvJobFontsClass *class) { /* Do Nothing */ }

EvJob *
ev_job_fonts_new (EvDocument *document)
{
	EvJobFonts *job;

	job = g_object_new (EV_TYPE_JOB_FONTS, NULL);

	EV_JOB (job)->document = g_object_ref (document);

	return EV_JOB (job);
}

void
ev_job_fonts_run (EvJobFonts *job)
{
	EvDocumentFonts *fonts;

	g_return_if_fail (EV_IS_JOB_FONTS (job));

	ev_document_doc_mutex_lock ();
	
	fonts = EV_DOCUMENT_FONTS (EV_JOB (job)->document);
	job->scan_completed = !ev_document_fonts_scan (fonts, 20);
	
	EV_JOB (job)->finished = TRUE;

	ev_document_doc_mutex_unlock ();
}

static void ev_job_xfer_init (EvJobXfer *job) { /* Do Nothing */ }

static void
ev_job_xfer_dispose (GObject *object)
{
	EvJobXfer *job = EV_JOB_XFER (object);

	if (job->uri) {
		g_free (job->uri);
		job->uri = NULL;
	}

	if (job->local_uri) {
		g_free (job->local_uri);
		job->local_uri = NULL;
	}

	if (job->error) {
		g_error_free (job->error);
		job->error = NULL;
	}

	if (job->dest) {
		g_object_unref (job->dest);
		job->dest = NULL;
	}

	(* G_OBJECT_CLASS (ev_job_xfer_parent_class)->dispose) (object);
}

static void
ev_job_xfer_class_init (EvJobXferClass *class)
{
	GObjectClass *oclass;

	oclass = G_OBJECT_CLASS (class);

	oclass->dispose = ev_job_xfer_dispose;
}


EvJob *
ev_job_xfer_new (const gchar *uri, EvLinkDest *dest)
{
	EvJobXfer *job;

	job = g_object_new (EV_TYPE_JOB_XFER, NULL);

	job->uri = g_strdup (uri);
	if (dest)
		job->dest = g_object_ref (dest);

	return EV_JOB (job);
}

void
ev_job_xfer_run (EvJobXfer *job)
{
	GnomeVFSURI *source_uri;
	GnomeVFSURI *target_uri;

	g_return_if_fail (EV_IS_JOB_XFER (job));
	
	if (job->error) {
	        g_error_free (job->error);
		job->error = NULL;
	}
	
	/* This job may already have a document even if the job didn't complete
	   because, e.g., a password is required - if so, just reload rather than
	   creating a new instance */
	if (EV_JOB (job)->document) {
		ev_document_load (EV_JOB (job)->document,
				  job->local_uri ? job->local_uri : job->uri,
				  &job->error);
		EV_JOB (job)->finished = TRUE;
		return;
	}

	source_uri = gnome_vfs_uri_new (job->uri);
	if (!gnome_vfs_uri_is_local (source_uri) && !job->local_uri) {
		char *tmp_name;
		char *base_name;
		
		/* We'd like to keep extension of source uri since
		 * it helps to resolve some mime types, say cbz */
		
		tmp_name = ev_tmp_filename ();
		base_name = gnome_vfs_uri_extract_short_name (source_uri);
		job->local_uri = g_strconcat ("file:", tmp_name, "-", base_name, NULL);
		g_free (base_name);
		g_free (tmp_name);
		
		target_uri = gnome_vfs_uri_new (job->local_uri);

		gnome_vfs_xfer_uri (source_uri, target_uri, 
				    GNOME_VFS_XFER_DEFAULT | GNOME_VFS_XFER_FOLLOW_LINKS,
				    GNOME_VFS_XFER_ERROR_MODE_ABORT,
				    GNOME_VFS_XFER_OVERWRITE_MODE_REPLACE,
				    NULL,
				    job);
		gnome_vfs_uri_unref (target_uri);
	}
	gnome_vfs_uri_unref (source_uri);

	EV_JOB(job)->document = ev_document_factory_get_document (job->local_uri ? job->local_uri : job->uri, &job->error);
	EV_JOB (job)->finished = TRUE;

	return;
}

