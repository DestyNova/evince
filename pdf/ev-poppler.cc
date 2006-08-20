/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8; c-indent-level: 8 -*- */
/* pdfdocument.h: Implementation of EvDocument for PDF
 * Copyright (C) 2004, Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <math.h>
#include <string.h>
#include <gtk/gtk.h>
#include <poppler.h>
#include <poppler-document.h>
#include <poppler-page.h>
#include <glib/gi18n.h>

#include "ev-poppler.h"
#include "ev-ps-exporter.h"
#include "ev-document-find.h"
#include "ev-document-misc.h"
#include "ev-document-links.h"
#include "ev-document-fonts.h"
#include "ev-document-security.h"
#include "ev-document-thumbnails.h"
#include "ev-selection.h"
#include "ev-attachment.h"

typedef struct {
	PdfDocument *document;
	char *text;
	GList **pages;
	guint idle;
	int start_page;
	int search_page;
} PdfDocumentSearch;

struct _PdfDocumentClass
{
	GObjectClass parent_class;
};

struct _PdfDocument
{
	GObject parent_instance;

	PopplerDocument *document;
	PopplerPSFile *ps_file;
	gchar *password;

	PopplerFontInfo *font_info;
	PopplerFontsIter *fonts_iter;
	int fonts_scanned_pages;

	PdfDocumentSearch *search;
};

static void pdf_document_document_iface_init            (EvDocumentIface           *iface);
static void pdf_document_security_iface_init            (EvDocumentSecurityIface   *iface);
static void pdf_document_document_thumbnails_iface_init (EvDocumentThumbnailsIface *iface);
static void pdf_document_document_links_iface_init      (EvDocumentLinksIface      *iface);
static void pdf_document_document_fonts_iface_init      (EvDocumentFontsIface      *iface);
static void pdf_document_find_iface_init                (EvDocumentFindIface       *iface);
static void pdf_document_ps_exporter_iface_init         (EvPSExporterIface         *iface);
static void pdf_selection_iface_init                    (EvSelectionIface          *iface);
static void pdf_document_thumbnails_get_dimensions      (EvDocumentThumbnails      *document_thumbnails,
							 gint                       page,
							 gint                       size,
							 gint                      *width,
							 gint                      *height);
static int  pdf_document_get_n_pages			(EvDocument                *document);

static EvLinkDest *ev_link_dest_from_dest (PopplerDest *dest);
static EvLink *ev_link_from_action (PopplerAction *action);
static void pdf_document_search_free (PdfDocumentSearch   *search);

static void pdf_document_get_crop_box (EvDocument *document, int page, EvRectangle *rect);
static GList *pdf_document_get_form_field_mapping (EvDocument *document, int page);
static gchar* pdf_document_get_text_field_content (EvDocument *document, int field_id);
static gboolean pdf_document_set_text_field_content (EvDocument *document, int field_id, gchar* content);
static void pdf_document_set_form_field_button_state (EvDocument *document, int field_id, int index, gboolean state);





G_DEFINE_TYPE_WITH_CODE (PdfDocument, pdf_document, G_TYPE_OBJECT,
                         {
				 G_IMPLEMENT_INTERFACE (EV_TYPE_DOCUMENT,
							pdf_document_document_iface_init);
				 G_IMPLEMENT_INTERFACE (EV_TYPE_DOCUMENT_SECURITY,
							pdf_document_security_iface_init);
				 G_IMPLEMENT_INTERFACE (EV_TYPE_DOCUMENT_THUMBNAILS,
							pdf_document_document_thumbnails_iface_init);
				 G_IMPLEMENT_INTERFACE (EV_TYPE_DOCUMENT_LINKS,
							pdf_document_document_links_iface_init);
				 G_IMPLEMENT_INTERFACE (EV_TYPE_DOCUMENT_FONTS,
							pdf_document_document_fonts_iface_init);
				 G_IMPLEMENT_INTERFACE (EV_TYPE_DOCUMENT_FIND,
							pdf_document_find_iface_init);
				 G_IMPLEMENT_INTERFACE (EV_TYPE_PS_EXPORTER,
							pdf_document_ps_exporter_iface_init);
				 G_IMPLEMENT_INTERFACE (EV_TYPE_SELECTION,
							pdf_selection_iface_init);
			 });


static void
set_rc_data (PdfDocument     *pdf_document,
	     EvRenderContext *rc)
{
	if (rc->data == NULL) {
		rc->data = poppler_document_get_page (pdf_document->document,
						      rc->page);
		rc->destroy = g_object_unref;
	} else {
		g_assert (rc->page == poppler_page_get_index (POPPLER_PAGE (rc->data)));
	}
}

static void
pdf_document_search_free (PdfDocumentSearch   *search)
{
        PdfDocument *pdf_document = search->document;
	int n_pages;
	int i;

        if (search->idle != 0)
                g_source_remove (search->idle);

	n_pages = pdf_document_get_n_pages (EV_DOCUMENT (pdf_document));
	for (i = 0; i < n_pages; i++) {
		g_list_foreach (search->pages[i], (GFunc) g_free, NULL);
		g_list_free (search->pages[i]);
	}
	
        g_free (search->text);
}

static void
pdf_document_dispose (GObject *object)
{
	PdfDocument *pdf_document = PDF_DOCUMENT(object);

	if (pdf_document->search) {
		pdf_document_search_free (pdf_document->search);
		pdf_document->search = NULL;
	}

	if (pdf_document->document) {
		g_object_unref (pdf_document->document);
	}

	if (pdf_document->font_info) { 
		poppler_font_info_free (pdf_document->font_info);
	}

	if (pdf_document->fonts_iter) {
		poppler_fonts_iter_free (pdf_document->fonts_iter);
	}
}

static void
pdf_document_class_init (PdfDocumentClass *klass)
{
	GObjectClass *g_object_class = G_OBJECT_CLASS (klass);

	g_object_class->dispose = pdf_document_dispose;
}

static void
pdf_document_init (PdfDocument *pdf_document)
{
	pdf_document->password = NULL;
}

static void
convert_error (GError  *poppler_error,
	       GError **error)
{
	if (poppler_error == NULL)
		return;

	if (poppler_error->domain == POPPLER_ERROR) {
		/* convert poppler errors into EvDocument errors */
		gint code = EV_DOCUMENT_ERROR_INVALID;
		if (poppler_error->code == POPPLER_ERROR_INVALID)
			code = EV_DOCUMENT_ERROR_INVALID;
		else if (poppler_error->code == POPPLER_ERROR_ENCRYPTED)
			code = EV_DOCUMENT_ERROR_ENCRYPTED;
			

		g_set_error (error,
			     EV_DOCUMENT_ERROR,
			     code,
			     poppler_error->message,
			     NULL);
	} else {
		g_propagate_error (error, poppler_error);
	}
}


/* EvDocument */
static gboolean
pdf_document_save (EvDocument  *document,
		   const char  *uri,
		   GError     **error)
{
	gboolean retval;
	GError *poppler_error = NULL;

	retval = poppler_document_save (PDF_DOCUMENT (document)->document,
					uri,
					&poppler_error);
	if (! retval)
		convert_error (poppler_error, error);

	return retval;
}

static gboolean
pdf_document_load (EvDocument   *document,
		   const char   *uri,
		   GError      **error)
{
	GError *poppler_error = NULL;
	PdfDocument *pdf_document = PDF_DOCUMENT (document);

	pdf_document->document =
		poppler_document_new_from_file (uri, pdf_document->password, &poppler_error);

	if (pdf_document->document == NULL) {
		convert_error (poppler_error, error);
		return FALSE;
	}

	return TRUE;
}

static int
pdf_document_get_n_pages (EvDocument *document)
{
	return poppler_document_get_n_pages (PDF_DOCUMENT (document)->document);
}

static void
pdf_document_get_page_size (EvDocument   *document,
			    int           page,
			    double       *width,
			    double       *height)
{
	PdfDocument *pdf_document = PDF_DOCUMENT (document);
	PopplerPage *poppler_page;

	poppler_page = poppler_document_get_page (pdf_document->document, page);
	poppler_page_get_size (poppler_page, width, height);
	g_object_unref (poppler_page);
}

static char *
pdf_document_get_page_label (EvDocument *document,
			     int         page)
{
	PopplerPage *poppler_page;
	char *label = NULL;

	poppler_page = poppler_document_get_page (PDF_DOCUMENT (document)->document,
						  page);

	g_object_get (G_OBJECT (poppler_page),
		      "label", &label,
		      NULL);
	g_object_unref (poppler_page);

	return label;
}

static gboolean
pdf_document_has_attachments (EvDocument *document)
{
	PdfDocument *pdf_document;

	pdf_document = PDF_DOCUMENT (document);

	return poppler_document_has_attachments (pdf_document->document);
}

struct SaveToBufferData {
	gchar *buffer;
	gsize len, max;
};

static gboolean
attachment_save_to_buffer_callback (const gchar  *buf,
				    gsize         count,
				    gpointer      user_data,
				    GError      **error)
{
	struct SaveToBufferData *sdata = (SaveToBufferData *)user_data;
	gchar *new_buffer;
	gsize new_max;

	if (sdata->len + count > sdata->max) {
		new_max = MAX (sdata->max * 2, sdata->len + count);
		new_buffer = (gchar *)g_realloc (sdata->buffer, new_max);

		sdata->buffer = new_buffer;
		sdata->max = new_max;
	}
	
	memcpy (sdata->buffer + sdata->len, buf, count);
	sdata->len += count;
	
	return TRUE;
}

static gboolean
attachment_save_to_buffer (PopplerAttachment  *attachment,
			   gchar             **buffer,
			   gsize              *buffer_size,
			   GError            **error)
{
	static const gint initial_max = 1024;
	struct SaveToBufferData sdata;

	*buffer = NULL;
	*buffer_size = 0;

	sdata.buffer = (gchar *) g_malloc (initial_max);
	sdata.max = initial_max;
	sdata.len = 0;

	if (! poppler_attachment_save_to_callback (attachment,
						   attachment_save_to_buffer_callback,
						   &sdata,
						   error)) {
		g_free (sdata.buffer);
		return FALSE;
	}

	*buffer = sdata.buffer;
	*buffer_size = sdata.len;
	
	return TRUE;
}

static GList *
pdf_document_get_attachments (EvDocument *document)
{
	PdfDocument *pdf_document;
	GList *attachments;
	GList *list;
	GList *retval = NULL;

	pdf_document = PDF_DOCUMENT (document);

	if (!pdf_document_has_attachments (document))
		return NULL;

	attachments = poppler_document_get_attachments (pdf_document->document);
	
	for (list = attachments; list; list = list->next) {
		PopplerAttachment *attachment;
		EvAttachment *ev_attachment;
		gchar *data = NULL;
		gsize size;
		GError *error = NULL;

		attachment = (PopplerAttachment *) list->data;

		if (attachment_save_to_buffer (attachment, &data, &size, &error)) {
			ev_attachment = ev_attachment_new (attachment->name,
							   attachment->description,
							   attachment->mtime,
							   attachment->ctime,
							   size, data);
			
			retval = g_list_prepend (retval, ev_attachment);
		} else {
			if (error) {
				g_warning ("%s", error->message);
				g_error_free (error);

				g_free (data);
			}
		}

		g_object_unref (attachment);
	}

	return g_list_reverse (retval);
}

static GdkPixbuf *
pdf_document_render_pixbuf (EvDocument   *document,
			    EvRenderContext *rc)
{
	PdfDocument *pdf_document;
	GdkPixbuf *pixbuf;
	double width_points, height_points;
	gint width, height;

	pdf_document = PDF_DOCUMENT (document);

	set_rc_data (pdf_document, rc);

	poppler_page_get_size (POPPLER_PAGE (rc->data), &width_points, &height_points);

	if (rc->rotation == 90 || rc->rotation == 270) {
		width = (int) ((height_points * rc->scale) + 0.5);
		height = (int) ((width_points * rc->scale) + 0.5);
	} else {
		width = (int) ((width_points * rc->scale) + 0.5);
		height = (int) ((height_points * rc->scale) + 0.5);
	}

	pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB,
				 FALSE, 8,
				 width, height);

	poppler_page_render_to_pixbuf (POPPLER_PAGE (rc->data),
				       0, 0,
				       width, height,
				       rc->scale,
				       rc->rotation,
				       pixbuf);
	
	
	return pixbuf;
}

/* EvDocumentSecurity */

static gboolean
pdf_document_has_document_security (EvDocumentSecurity *document_security)
{
	/* FIXME: do we really need to have this? */
	return FALSE;
}

static void
pdf_document_set_password (EvDocumentSecurity *document_security,
			   const char         *password)
{
	PdfDocument *document = PDF_DOCUMENT (document_security);

	if (document->password)
		g_free (document->password);

	document->password = g_strdup (password);
}

static gboolean
pdf_document_can_get_text (EvDocument *document)
{
	return TRUE;
}

static EvDocumentInfo *
pdf_document_get_info (EvDocument *document)
{
	EvDocumentInfo *info;
	PopplerPageLayout layout;
	PopplerPageMode mode;
	PopplerViewerPreferences view_prefs;
	PopplerPermissions permissions;

	info = g_new0 (EvDocumentInfo, 1);

	info->fields_mask = EV_DOCUMENT_INFO_TITLE |
			    EV_DOCUMENT_INFO_FORMAT |
			    EV_DOCUMENT_INFO_AUTHOR |
			    EV_DOCUMENT_INFO_SUBJECT |
			    EV_DOCUMENT_INFO_KEYWORDS |
			    EV_DOCUMENT_INFO_LAYOUT |
			    EV_DOCUMENT_INFO_START_MODE |
		            EV_DOCUMENT_INFO_PERMISSIONS |
			    EV_DOCUMENT_INFO_UI_HINTS |
			    EV_DOCUMENT_INFO_CREATOR |
			    EV_DOCUMENT_INFO_PRODUCER |
			    EV_DOCUMENT_INFO_CREATION_DATE |
			    EV_DOCUMENT_INFO_MOD_DATE |
			    EV_DOCUMENT_INFO_LINEARIZED |
			    EV_DOCUMENT_INFO_N_PAGES |
			    EV_DOCUMENT_INFO_SECURITY;


	g_object_get (PDF_DOCUMENT (document)->document,
		      "title", &(info->title),
		      "format", &(info->format),
		      "author", &(info->author),
		      "subject", &(info->subject),
		      "keywords", &(info->keywords),
		      "page-mode", &mode,
		      "page-layout", &layout,
		      "viewer-preferences", &view_prefs,
		      "permissions", &permissions,
		      "creator", &(info->creator),
		      "producer", &(info->producer),
		      "creation-date", &(info->creation_date),
		      "mod-date", &(info->modified_date),
		      "linearized", &(info->linearized),
		      NULL);

	switch (layout) {
		case POPPLER_PAGE_LAYOUT_SINGLE_PAGE:
			info->layout = EV_DOCUMENT_LAYOUT_SINGLE_PAGE;
			break;
		case POPPLER_PAGE_LAYOUT_ONE_COLUMN:
			info->layout = EV_DOCUMENT_LAYOUT_ONE_COLUMN;
			break;
		case POPPLER_PAGE_LAYOUT_TWO_COLUMN_LEFT:
			info->layout = EV_DOCUMENT_LAYOUT_TWO_COLUMN_LEFT;
			break;
		case POPPLER_PAGE_LAYOUT_TWO_COLUMN_RIGHT:
			info->layout = EV_DOCUMENT_LAYOUT_TWO_COLUMN_RIGHT;
		case POPPLER_PAGE_LAYOUT_TWO_PAGE_LEFT:
			info->layout = EV_DOCUMENT_LAYOUT_TWO_PAGE_LEFT;
			break;
		case POPPLER_PAGE_LAYOUT_TWO_PAGE_RIGHT:
			info->layout = EV_DOCUMENT_LAYOUT_TWO_PAGE_RIGHT;
			break;
	        default:
			break;
	}

	switch (mode) {
		case POPPLER_PAGE_MODE_NONE:
			info->mode = EV_DOCUMENT_MODE_NONE;
			break;
		case POPPLER_PAGE_MODE_USE_THUMBS:
			info->mode = EV_DOCUMENT_MODE_USE_THUMBS;
			break;
		case POPPLER_PAGE_MODE_USE_OC:
			info->mode = EV_DOCUMENT_MODE_USE_OC;
			break;
		case POPPLER_PAGE_MODE_FULL_SCREEN:
			info->mode = EV_DOCUMENT_MODE_FULL_SCREEN;
			break;
		case POPPLER_PAGE_MODE_USE_ATTACHMENTS:
			info->mode = EV_DOCUMENT_MODE_USE_ATTACHMENTS;
	        default:
			break;
	}

	info->ui_hints = 0;
	if (view_prefs & POPPLER_VIEWER_PREFERENCES_HIDE_TOOLBAR) {
		info->ui_hints |= EV_DOCUMENT_UI_HINT_HIDE_TOOLBAR;
	}
	if (view_prefs & POPPLER_VIEWER_PREFERENCES_HIDE_MENUBAR) {
		info->ui_hints |= EV_DOCUMENT_UI_HINT_HIDE_MENUBAR;
	}
	if (view_prefs & POPPLER_VIEWER_PREFERENCES_HIDE_WINDOWUI) {
		info->ui_hints |= EV_DOCUMENT_UI_HINT_HIDE_WINDOWUI;
	}
	if (view_prefs & POPPLER_VIEWER_PREFERENCES_FIT_WINDOW) {
		info->ui_hints |= EV_DOCUMENT_UI_HINT_FIT_WINDOW;
	}
	if (view_prefs & POPPLER_VIEWER_PREFERENCES_CENTER_WINDOW) {
		info->ui_hints |= EV_DOCUMENT_UI_HINT_CENTER_WINDOW;
	}
	if (view_prefs & POPPLER_VIEWER_PREFERENCES_DISPLAY_DOC_TITLE) {
		info->ui_hints |= EV_DOCUMENT_UI_HINT_DISPLAY_DOC_TITLE;
	}
	if (view_prefs & POPPLER_VIEWER_PREFERENCES_DIRECTION_RTL) {
		info->ui_hints |=  EV_DOCUMENT_UI_HINT_DIRECTION_RTL;
	}

	info->permissions = 0;
	if (permissions & POPPLER_PERMISSIONS_OK_TO_PRINT) {
		info->permissions |= EV_DOCUMENT_PERMISSIONS_OK_TO_PRINT;
	}
	if (permissions & POPPLER_PERMISSIONS_OK_TO_MODIFY) {
		info->permissions |= EV_DOCUMENT_PERMISSIONS_OK_TO_MODIFY;
	}
	if (permissions & POPPLER_PERMISSIONS_OK_TO_COPY) {
		info->permissions |= EV_DOCUMENT_PERMISSIONS_OK_TO_COPY;
	}
	if (permissions & POPPLER_PERMISSIONS_OK_TO_ADD_NOTES) {
		info->permissions |= EV_DOCUMENT_PERMISSIONS_OK_TO_ADD_NOTES;
	}

	info->n_pages = ev_document_get_n_pages (document);

	if (ev_document_security_has_document_security (EV_DOCUMENT_SECURITY (document))) {
		/* translators: this is the document security state */
		info->security = g_strdup (_("Yes"));
	} else {
		/* translators: this is the document security state */
		info->security = g_strdup (_("No"));
	}

	return info;
}

static char *
pdf_document_get_text (EvDocument *document, int page, EvRectangle *rect)
{
	PdfDocument *pdf_document = PDF_DOCUMENT (document);
	PopplerPage *poppler_page;
	PopplerRectangle r;
	double height;
	char *text;
	
	poppler_page = poppler_document_get_page (pdf_document->document, page);
	g_return_val_if_fail (poppler_page != NULL, NULL);

	poppler_page_get_size (poppler_page, NULL, &height);
	r.x1 = rect->x1;
	r.y1 = height - rect->y2;
	r.x2 = rect->x2;
	r.y2 = height - rect->y1;

	text = poppler_page_get_text (poppler_page, &r);

	g_object_unref (poppler_page);

	return text;
}

static void
pdf_document_document_iface_init (EvDocumentIface *iface)
{
	iface->save = pdf_document_save;
	iface->load = pdf_document_load;
	iface->get_n_pages = pdf_document_get_n_pages;
	iface->get_page_size = pdf_document_get_page_size;
	iface->get_page_label = pdf_document_get_page_label;
	iface->has_attachments = pdf_document_has_attachments;
	iface->get_attachments = pdf_document_get_attachments;
	iface->render_pixbuf = pdf_document_render_pixbuf;
	iface->get_text = pdf_document_get_text;
	iface->get_form_field_mapping = pdf_document_get_form_field_mapping;
	iface->get_crop_box = pdf_document_get_crop_box;
	iface->can_get_text = pdf_document_can_get_text;
	iface->get_info = pdf_document_get_info;
	iface->set_text_field_content = pdf_document_set_text_field_content;
	iface->get_text_field_content = pdf_document_get_text_field_content;
	iface->set_button_state = pdf_document_set_form_field_button_state;
};

static void
pdf_document_security_iface_init (EvDocumentSecurityIface *iface)
{
	iface->has_document_security = pdf_document_has_document_security;
	iface->set_password = pdf_document_set_password;
}

static gdouble
pdf_document_fonts_get_progress (EvDocumentFonts *document_fonts)
{
	PdfDocument *pdf_document = PDF_DOCUMENT (document_fonts);
	int n_pages;

        n_pages = pdf_document_get_n_pages (EV_DOCUMENT (pdf_document));

	return (double)pdf_document->fonts_scanned_pages / (double)n_pages;
}

static gboolean
pdf_document_fonts_scan (EvDocumentFonts *document_fonts,
			 int              n_pages)
{
	PdfDocument *pdf_document = PDF_DOCUMENT (document_fonts);
	gboolean result;

	g_return_val_if_fail (PDF_IS_DOCUMENT (document_fonts), FALSE);

	if (pdf_document->font_info == NULL) { 
		pdf_document->font_info = poppler_font_info_new (pdf_document->document);
	}

	if (pdf_document->fonts_iter) {
		poppler_fonts_iter_free (pdf_document->fonts_iter);
	}

	pdf_document->fonts_scanned_pages += n_pages;

	result = poppler_font_info_scan (pdf_document->font_info, n_pages,
				         &pdf_document->fonts_iter);
	if (!result) {
		pdf_document->fonts_scanned_pages = 0;
		poppler_font_info_free (pdf_document->font_info);
		pdf_document->font_info = NULL;	
	}

	return result;
}

static const char *
font_type_to_string (PopplerFontType type)
{
	switch (type) {
	        case POPPLER_FONT_TYPE_TYPE1:
			return _("Type 1");
	        case POPPLER_FONT_TYPE_TYPE1C:
			return _("Type 1C");
	        case POPPLER_FONT_TYPE_TYPE3:
			return _("Type 3");
	        case POPPLER_FONT_TYPE_TRUETYPE:
			return _("TrueType");
	        case POPPLER_FONT_TYPE_CID_TYPE0:
			return _("Type 1 (CID)");
	        case POPPLER_FONT_TYPE_CID_TYPE0C:
			return _("Type 1C (CID)");
	        case POPPLER_FONT_TYPE_CID_TYPE2:
			return _("TrueType (CID)");
	        default:
			return _("Unknown font type");
	}
}

static void
pdf_document_fonts_fill_model (EvDocumentFonts *document_fonts,
			       GtkTreeModel    *model)
{
	PdfDocument *pdf_document = PDF_DOCUMENT (document_fonts);
	PopplerFontsIter *iter = pdf_document->fonts_iter;

	g_return_if_fail (PDF_IS_DOCUMENT (document_fonts));

	if (!iter)
		return;

	do {
		GtkTreeIter list_iter;
		const char *name;
		const char *type;
		const char *embedded;
		char *details;
		
		name = poppler_fonts_iter_get_name (iter);

		if (name == NULL) {
			name = _("No name");
		}

		type = font_type_to_string (
			poppler_fonts_iter_get_font_type (iter));

		if (poppler_fonts_iter_is_embedded (iter)) {
			if (poppler_fonts_iter_is_subset (iter))
				embedded = _("Embedded subset");
			else
				embedded = _("Embedded");
		} else {
			embedded = _("Not embedded");
		}

		details = g_markup_printf_escaped ("%s\n%s", type, embedded);

		gtk_list_store_append (GTK_LIST_STORE (model), &list_iter);
		gtk_list_store_set (GTK_LIST_STORE (model), &list_iter,
				    EV_DOCUMENT_FONTS_COLUMN_NAME, name,
				    EV_DOCUMENT_FONTS_COLUMN_DETAILS, details,
				    -1);

		g_free (details);
	} while (poppler_fonts_iter_next (iter));
}

static void
pdf_document_document_fonts_iface_init (EvDocumentFontsIface *iface)
{
	iface->fill_model = pdf_document_fonts_fill_model;
	iface->scan = pdf_document_fonts_scan;
	iface->get_progress = pdf_document_fonts_get_progress;
}

static gboolean
pdf_document_links_has_document_links (EvDocumentLinks *document_links)
{
	PdfDocument *pdf_document = PDF_DOCUMENT (document_links);
	PopplerIndexIter *iter;

	g_return_val_if_fail (PDF_IS_DOCUMENT (document_links), FALSE);

	iter = poppler_index_iter_new (pdf_document->document);
	if (iter == NULL)
		return FALSE;
	poppler_index_iter_free (iter);

	return TRUE;
}

static EvLinkDest *
ev_link_dest_from_dest (PopplerDest *dest)
{
	EvLinkDest *ev_dest = NULL;
	const char *unimplemented_dest = NULL;

	g_assert (dest != NULL);
	
	switch (dest->type) {
	        case POPPLER_DEST_XYZ:
			ev_dest = ev_link_dest_new_xyz (dest->page_num - 1,
							dest->left,
							dest->top,
							dest->zoom);
			break;
	        case POPPLER_DEST_FIT:
			ev_dest = ev_link_dest_new_fit (dest->page_num - 1);
			break;
	        case POPPLER_DEST_FITH:
			ev_dest = ev_link_dest_new_fith (dest->page_num - 1,
							 dest->top);
			break;
	        case POPPLER_DEST_FITV:
			ev_dest = ev_link_dest_new_fitv (dest->page_num - 1,
							 dest->left);
			break;
	        case POPPLER_DEST_FITR:
			ev_dest = ev_link_dest_new_fitr (dest->page_num - 1,
							 dest->left,
							 dest->bottom,
							 dest->right,
							 dest->top);
			break;
	        case POPPLER_DEST_FITB:
			unimplemented_dest = "POPPLER_DEST_FITB";
			break;
	        case POPPLER_DEST_FITBH:
			unimplemented_dest = "POPPLER_DEST_FITBH";
			break;
	        case POPPLER_DEST_FITBV:
			unimplemented_dest = "POPPLER_DEST_FITBV";
			break;
	        case POPPLER_DEST_NAMED:
			ev_dest = ev_link_dest_new_named (dest->named_dest);
			break;
	        case POPPLER_DEST_UNKNOWN:
			unimplemented_dest = "POPPLER_DEST_UNKNOWN";
			break;
	}
	
	if (unimplemented_dest) {
		g_warning ("Unimplemented destination: %s, please post a bug report with a testcase.",
			   unimplemented_dest);
	}

	if (!ev_dest)
		ev_dest = ev_link_dest_new_page (dest->page_num - 1);
	
	return ev_dest;
}

static EvLink *
ev_link_from_action (PopplerAction *action)
{
	EvLink       *link = NULL;
	EvLinkAction *ev_action = NULL;
	const char   *unimplemented_action = NULL;

	switch (action->type) {
	        case POPPLER_ACTION_GOTO_DEST: {
			EvLinkDest *dest;
			
			dest = ev_link_dest_from_dest (action->goto_dest.dest);
			ev_action = ev_link_action_new_dest (dest);
		}
			break;
	        case POPPLER_ACTION_GOTO_REMOTE: {
			EvLinkDest *dest;
			
			dest = ev_link_dest_from_dest (action->goto_remote.dest);
			ev_action = ev_link_action_new_remote (dest, 
							       action->goto_remote.file_name);
			
		}
			break;
	        case POPPLER_ACTION_LAUNCH:
			ev_action = ev_link_action_new_launch (action->launch.file_name,
							       action->launch.params);
			break;
	        case POPPLER_ACTION_URI:
			ev_action = ev_link_action_new_external_uri (action->uri.uri);
			break;
	        case POPPLER_ACTION_NAMED:
			ev_action = ev_link_action_new_named (action->named.named_dest);
			break;
	        case POPPLER_ACTION_MOVIE:
			unimplemented_action = "POPPLER_ACTION_MOVIE";
			break;
	        case POPPLER_ACTION_UNKNOWN:
			unimplemented_action = "POPPLER_ACTION_UNKNOWN";
	}
	
	if (unimplemented_action) {
		g_warning ("Unimplemented action: %s, please post a bug report with a testcase.",
			   unimplemented_action);
	}
	
	link = ev_link_new (action->any.title, ev_action);
	
	return link;	
}

static void
build_tree (PdfDocument      *pdf_document,
	    GtkTreeModel     *model,
	    GtkTreeIter      *parent,
	    PopplerIndexIter *iter)
{
	
	do {
		GtkTreeIter tree_iter;
		PopplerIndexIter *child;
		PopplerAction *action;
		EvLink *link = NULL;
		gboolean expand;
		char *title_markup;
		
		action = poppler_index_iter_get_action (iter);
		expand = poppler_index_iter_is_open (iter);

		if (!action)
			continue;

		switch (action->type) {
		        case POPPLER_ACTION_GOTO_DEST: {
				/* For bookmarks, solve named destinations */
				if (action->goto_dest.dest->type == POPPLER_DEST_NAMED) {
					PopplerDest *dest;
					EvLinkDest *ev_dest = NULL;
					EvLinkAction *ev_action;
					
					dest = poppler_document_find_dest (pdf_document->document,
									   action->goto_dest.dest->named_dest);
					if (!dest) {
						link = ev_link_from_action (action);
						break;
					}
					
					ev_dest = ev_link_dest_from_dest (dest);
					poppler_dest_free (dest);
					
					ev_action = ev_link_action_new_dest (ev_dest);
					link = ev_link_new (action->any.title, ev_action);
				} else {
					link = ev_link_from_action (action);
				}
			}
				break;
		        default:
				link = ev_link_from_action (action);
				break;
		}
		
		if (!link) {
			poppler_action_free (action);
			continue;
		}

		gtk_tree_store_append (GTK_TREE_STORE (model), &tree_iter, parent);
		title_markup = g_markup_escape_text (ev_link_get_title (link), -1);
		
		gtk_tree_store_set (GTK_TREE_STORE (model), &tree_iter,
				    EV_DOCUMENT_LINKS_COLUMN_MARKUP, title_markup,
				    EV_DOCUMENT_LINKS_COLUMN_LINK, link,
				    EV_DOCUMENT_LINKS_COLUMN_EXPAND, expand,
				    -1);
		
		g_free (title_markup);
		g_object_unref (link);
		
		child = poppler_index_iter_get_child (iter);
		if (child)
			build_tree (pdf_document, model, &tree_iter, child);
		poppler_index_iter_free (child);
		poppler_action_free (action);
		
	} while (poppler_index_iter_next (iter));
}

static GtkTreeModel *
pdf_document_links_get_links_model (EvDocumentLinks *document_links)
{
	PdfDocument *pdf_document = PDF_DOCUMENT (document_links);
	GtkTreeModel *model = NULL;
	PopplerIndexIter *iter;
	
	g_return_val_if_fail (PDF_IS_DOCUMENT (document_links), NULL);

	iter = poppler_index_iter_new (pdf_document->document);
	/* Create the model if we have items*/
	if (iter != NULL) {
		model = (GtkTreeModel *) gtk_tree_store_new (EV_DOCUMENT_LINKS_COLUMN_NUM_COLUMNS,
							     G_TYPE_STRING,
							     G_TYPE_OBJECT,
							     G_TYPE_BOOLEAN);
		build_tree (pdf_document, model, NULL, iter);
		poppler_index_iter_free (iter);
	}
	
	return model;
}

static GList *
pdf_document_links_get_links (EvDocumentLinks *document_links,
			      gint             page)
{
	PdfDocument *pdf_document;
	PopplerPage *poppler_page;
	GList *retval = NULL;
	GList *mapping_list;
	GList *list;
	double height;

	pdf_document = PDF_DOCUMENT (document_links);
	poppler_page = poppler_document_get_page (pdf_document->document,
						  page);
	mapping_list = poppler_page_get_link_mapping (poppler_page);
	poppler_page_get_size (poppler_page, NULL, &height);

	for (list = mapping_list; list; list = list->next) {
		PopplerLinkMapping *link_mapping;
		EvLinkMapping *ev_link_mapping;

		link_mapping = (PopplerLinkMapping *)list->data;
		ev_link_mapping = g_new (EvLinkMapping, 1);
		ev_link_mapping->link = ev_link_from_action (link_mapping->action);
		ev_link_mapping->x1 = link_mapping->area.x1;
		ev_link_mapping->x2 = link_mapping->area.x2;
		/* Invert this for X-style coordinates */
		ev_link_mapping->y1 = height - link_mapping->area.y2;
		ev_link_mapping->y2 = height - link_mapping->area.y1;

		retval = g_list_prepend (retval, ev_link_mapping);
	}

	poppler_page_free_link_mapping (mapping_list);
	g_object_unref (poppler_page);

	return g_list_reverse (retval);
}

static EvLinkDest *
pdf_document_links_find_link_dest (EvDocumentLinks  *document_links,
				   const gchar      *link_name)
{
	PdfDocument *pdf_document;
	PopplerDest *dest;
	EvLinkDest *ev_dest = NULL;

	pdf_document = PDF_DOCUMENT (document_links);
	dest = poppler_document_find_dest (pdf_document->document,
					   link_name);
	if (dest) {
		ev_dest = ev_link_dest_from_dest (dest);
		poppler_dest_free (dest);
	}

	return ev_dest;
}

static void
pdf_document_document_links_iface_init (EvDocumentLinksIface *iface)
{
	iface->has_document_links = pdf_document_links_has_document_links;
	iface->get_links_model = pdf_document_links_get_links_model;
	iface->get_links = pdf_document_links_get_links;
	iface->find_link_dest = pdf_document_links_find_link_dest;
}

static GdkPixbuf *
make_thumbnail_for_size (PdfDocument   *pdf_document,
			 gint           page,
			 int            rotation,
			 gint           size)
{
	PopplerPage *poppler_page;
	GdkPixbuf *pixbuf;
	int width, height;
	double scale;
	gdouble unscaled_width, unscaled_height;

	poppler_page = poppler_document_get_page (pdf_document->document, page);
	g_return_val_if_fail (poppler_page != NULL, NULL);

	pdf_document_thumbnails_get_dimensions (EV_DOCUMENT_THUMBNAILS (pdf_document), page,
						size, &width, &height);
	poppler_page_get_size (poppler_page, &unscaled_width, &unscaled_height);
	scale = width / unscaled_width;

	/* rotate */
	if (rotation == 90 || rotation == 270) {
		int temp;
		temp = width;
		width = height;
		height = temp;
	}

	pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8,
				 width, height);
	gdk_pixbuf_fill (pixbuf, 0xffffffff);

	poppler_page_render_to_pixbuf (poppler_page, 0, 0,
				       width, height,
				       scale, rotation, pixbuf);
       

	g_object_unref (poppler_page);

	return pixbuf;
}

static GdkPixbuf *
pdf_document_thumbnails_get_thumbnail (EvDocumentThumbnails *document_thumbnails,
	 			       gint 		     page,
				       gint                  rotation,
 				       gint                  size,
				       gboolean              border)
{
	PdfDocument *pdf_document;
	PopplerPage *poppler_page;
	GdkPixbuf *pixbuf;
	GdkPixbuf *border_pixbuf;

	pdf_document = PDF_DOCUMENT (document_thumbnails);

	poppler_page = poppler_document_get_page (pdf_document->document, page);
	g_return_val_if_fail (poppler_page != NULL, NULL);

	pixbuf = poppler_page_get_thumbnail (poppler_page);
	
	if (pixbuf == NULL) {
		/* There is no provided thumbnail.  We need to make one. */
		pixbuf = make_thumbnail_for_size (pdf_document, page, rotation, size);
	}

        if (border) {		
		border_pixbuf = ev_document_misc_get_thumbnail_frame (-1, -1, rotation, pixbuf);
		g_object_unref (pixbuf);
		pixbuf = border_pixbuf;
	}		

	g_object_unref (poppler_page);
	
	return pixbuf;
}

static void
pdf_document_thumbnails_get_dimensions (EvDocumentThumbnails *document_thumbnails,
					gint                  page,
					gint                  size,
					gint                 *width,
					gint                 *height)
{
	PdfDocument *pdf_document;
	PopplerPage *poppler_page;
	gint has_thumb;
	
	pdf_document = PDF_DOCUMENT (document_thumbnails);
	poppler_page = poppler_document_get_page (pdf_document->document, page);

	g_return_if_fail (width != NULL);
	g_return_if_fail (height != NULL);
	g_return_if_fail (poppler_page != NULL);

	has_thumb = poppler_page_get_thumbnail_size (poppler_page, width, height);

	if (!has_thumb) {
		double page_width, page_height;

		poppler_page_get_size (poppler_page, &page_width, &page_height);
		*width = size;
		*height = (int) (size * page_height / page_width);
	}
	g_object_unref (poppler_page);
}

static void
pdf_document_document_thumbnails_iface_init (EvDocumentThumbnailsIface *iface)
{
	iface->get_thumbnail = pdf_document_thumbnails_get_thumbnail;
	iface->get_dimensions = pdf_document_thumbnails_get_dimensions;
}


static gboolean
pdf_document_search_idle_callback (void *data)
{
        PdfDocumentSearch *search = (PdfDocumentSearch*) data;
        PdfDocument *pdf_document = search->document;
        int n_pages;
	GList *matches;
	PopplerPage *page;

	page = poppler_document_get_page (search->document->document,
					  search->search_page);

	ev_document_doc_mutex_lock ();
	matches = poppler_page_find_text (page, search->text);
	ev_document_doc_mutex_unlock ();

	g_object_unref (page);

	search->pages[search->search_page] = matches;
	ev_document_find_changed (EV_DOCUMENT_FIND (pdf_document),
				  search->search_page);

        n_pages = pdf_document_get_n_pages (EV_DOCUMENT (search->document));
        search->search_page += 1;
        if (search->search_page == n_pages) {
                /* wrap around */
                search->search_page = 0;
        }

        if (search->search_page != search->start_page) {
	        return TRUE;
	}

        /* We're done. */
        search->idle = 0; /* will return FALSE to remove */
        return FALSE;
}


static PdfDocumentSearch *
pdf_document_search_new (PdfDocument *pdf_document,
			 int          start_page,
			 const char  *text)
{
	PdfDocumentSearch *search;
	int n_pages;
	int i;

	n_pages = pdf_document_get_n_pages (EV_DOCUMENT (pdf_document));

        search = g_new0 (PdfDocumentSearch, 1);

	search->text = g_strdup (text);
        search->pages = g_new0 (GList *, n_pages);
	for (i = 0; i < n_pages; i++) {
		search->pages[i] = NULL;
	}

        search->document = pdf_document;

        /* We add at low priority so the progress bar repaints */
        search->idle = g_idle_add_full (G_PRIORITY_LOW,
                                        pdf_document_search_idle_callback,
                                        search,
                                        NULL);

        search->start_page = start_page;
        search->search_page = start_page;

	return search;
}

static void
pdf_document_find_begin (EvDocumentFind   *document,
			 int               page,
                         const char       *search_string,
                         gboolean          case_sensitive)
{
        PdfDocument *pdf_document = PDF_DOCUMENT (document);

        /* FIXME handle case_sensitive (right now XPDF
         * code is always case insensitive for ASCII
         * and case sensitive for all other languaages)
         */

	if (pdf_document->search &&
	    strcmp (search_string, pdf_document->search->text) == 0)
                return;

        if (pdf_document->search)
                pdf_document_search_free (pdf_document->search);

        pdf_document->search = pdf_document_search_new (pdf_document,
							page,
							search_string);
}

int
pdf_document_find_get_n_results (EvDocumentFind *document_find, int page)
{
	PdfDocumentSearch *search = PDF_DOCUMENT (document_find)->search;

	if (search) {
		return g_list_length (search->pages[page]);
	} else {
		return 0;
	}
}

gboolean
pdf_document_find_get_result (EvDocumentFind *document_find,
			      int             page,
			      int             n_result,
			      EvRectangle    *rectangle)
{
	PdfDocument *pdf_document = PDF_DOCUMENT (document_find);
	PdfDocumentSearch *search = pdf_document->search;
	PopplerPage *poppler_page;
	PopplerRectangle *r;
	double height;

	if (search == NULL)
		return FALSE;

	r = (PopplerRectangle *) g_list_nth_data (search->pages[page],
						  n_result);
	if (r == NULL)
		return FALSE;

	poppler_page = poppler_document_get_page (pdf_document->document, page);
	poppler_page_get_size (poppler_page, NULL, &height);
	rectangle->x1 = r->x1;
	rectangle->y1 = height - r->y2;
	rectangle->x2 = r->x2;
	rectangle->y2 = height - r->y1;
	g_object_unref (poppler_page);
		
	return TRUE;
}

int
pdf_document_find_page_has_results (EvDocumentFind *document_find,
				    int             page)
{
	PdfDocumentSearch *search = PDF_DOCUMENT (document_find)->search;

	return search && search->pages[page] != NULL;
}

double
pdf_document_find_get_progress (EvDocumentFind *document_find)
{
	PdfDocumentSearch *search;
	int n_pages, pages_done;

	search = PDF_DOCUMENT (document_find)->search;

	if (search == NULL) {
		return 0;
	}

	n_pages = pdf_document_get_n_pages (EV_DOCUMENT (document_find));
	if (search->search_page > search->start_page) {
		pages_done = search->search_page - search->start_page + 1;
	} else if (search->search_page == search->start_page) {
		pages_done = n_pages;
	} else {
		pages_done = n_pages - search->start_page + search->search_page;
	}

	return pages_done / (double) n_pages;
}

static void
pdf_document_find_cancel (EvDocumentFind *document)
{
        PdfDocument *pdf_document = PDF_DOCUMENT (document);

	if (pdf_document->search) {
		pdf_document_search_free (pdf_document->search);
		pdf_document->search = NULL;
	}
}

static void
pdf_document_find_iface_init (EvDocumentFindIface *iface)
{
        iface->begin = pdf_document_find_begin;
	iface->get_n_results = pdf_document_find_get_n_results;
	iface->get_result = pdf_document_find_get_result;
	iface->page_has_results = pdf_document_find_page_has_results;
	iface->get_progress = pdf_document_find_get_progress;
        iface->cancel = pdf_document_find_cancel;
}

static void
pdf_document_ps_exporter_begin (EvPSExporter *exporter, const char *filename,
				int first_page, int last_page,
				double width, double height, gboolean duplex)
{
	PdfDocument *pdf_document = PDF_DOCUMENT (exporter);
	
	pdf_document->ps_file = poppler_ps_file_new (pdf_document->document, filename,
						     first_page,
						     last_page - first_page + 1);
	poppler_ps_file_set_paper_size (pdf_document->ps_file, width, height);
	poppler_ps_file_set_duplex (pdf_document->ps_file, duplex);
}

static void
pdf_document_ps_exporter_do_page (EvPSExporter *exporter, EvRenderContext *rc)
{
	PdfDocument *pdf_document = PDF_DOCUMENT (exporter);
	PopplerPage *poppler_page;

	g_return_if_fail (pdf_document->ps_file != NULL);

	poppler_page = poppler_document_get_page (pdf_document->document, rc->page);
	poppler_page_render_to_ps (poppler_page, pdf_document->ps_file);
	g_object_unref (poppler_page);
}

static void
pdf_document_ps_exporter_end (EvPSExporter *exporter)
{
	PdfDocument *pdf_document = PDF_DOCUMENT (exporter);

	poppler_ps_file_free (pdf_document->ps_file);
	pdf_document->ps_file = NULL;
}

static void
pdf_document_ps_exporter_iface_init (EvPSExporterIface *iface)
{
        iface->begin = pdf_document_ps_exporter_begin;
        iface->do_page = pdf_document_ps_exporter_do_page;
        iface->end = pdf_document_ps_exporter_end;
}


void
pdf_selection_render_selection (EvSelection      *selection,
				EvRenderContext  *rc,
				GdkPixbuf       **pixbuf,
				EvRectangle      *points,
				EvRectangle      *old_points,
				GdkColor        *text,
				GdkColor        *base)
{
	PdfDocument *pdf_document;
	double width_points, height_points;
	gint width, height;

	pdf_document = PDF_DOCUMENT (selection);
	set_rc_data (pdf_document, rc);

	poppler_page_get_size (POPPLER_PAGE (rc->data), &width_points, &height_points);
	width = (int) ((width_points * rc->scale) + 0.5);
	height = (int) ((height_points * rc->scale) + 0.5);

	if (*pixbuf == NULL) {
		* pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB,
					   TRUE, 8,
					   width, height);
	}
	
	poppler_page_render_selection (POPPLER_PAGE (rc->data),
				       rc->scale, rc->rotation, *pixbuf,
				       (PopplerRectangle *)points,
				       (PopplerRectangle *)old_points,
				       text,
				       base);
}


GdkRegion *
pdf_selection_get_selection_region (EvSelection     *selection,
				    EvRenderContext *rc,
				    EvRectangle     *points)
{
	PdfDocument *pdf_document;
	GdkRegion *retval;

	pdf_document = PDF_DOCUMENT (selection);

	set_rc_data (pdf_document, rc);

	retval = poppler_page_get_selection_region ((PopplerPage *)rc->data, rc->scale, (PopplerRectangle *) points);

	return retval;
}

GdkRegion *
pdf_selection_get_selection_map (EvSelection     *selection,
				 EvRenderContext *rc)
{
	PdfDocument *pdf_document;
	PopplerPage *poppler_page;
	PopplerRectangle points;
	GdkRegion *retval;

	pdf_document = PDF_DOCUMENT (selection);
	poppler_page = poppler_document_get_page (pdf_document->document,
						  rc->page);

	points.x1 = 0.0;
	points.y1 = 0.0;
	poppler_page_get_size (poppler_page, &(points.x2), &(points.y2));
	retval = poppler_page_get_selection_region (poppler_page, 1.0, &points);
	g_object_unref (poppler_page);

	return retval;
}

static void
pdf_selection_iface_init (EvSelectionIface *iface)
{
        iface->render_selection = pdf_selection_render_selection;
        iface->get_selection_region = pdf_selection_get_selection_region;
        iface->get_selection_map = pdf_selection_get_selection_map;
}

PdfDocument *
pdf_document_new (void)
{
	return PDF_DOCUMENT (g_object_new (PDF_TYPE_DOCUMENT, NULL));
}


void copy_form_poppler_evince (PopplerFormField *source, EvFormField *dest, double height, EvRectangle crop_box)
{
	dest->x1 = source->area.x1 - crop_box.x1;
	dest->x2 = source->area.x2 - crop_box.x1;
	dest->y1 = height - source->area.y2 + crop_box.y1;
	dest->y2 = height - source->area.y1 + crop_box.y1;
}

static GList *
pdf_document_get_form_field_mapping (EvDocument *document,
 			      int 	 page)
{
 	PdfDocument *pdf_document;
 	PopplerPage *poppler_page;
 	GList *retval = NULL;
 	GList *fields;
 	GList *list;
 	double height;
	EvRectangle crop_box;

 	pdf_document = PDF_DOCUMENT (document);
 	poppler_page = poppler_document_get_page (pdf_document->document,
 						  page);
 	fields = poppler_page_get_form_fields (poppler_page);
 	poppler_page_get_size (poppler_page, NULL, &height);

	pdf_document_get_crop_box(document, page, &crop_box);
 
 	for (list = fields; list; list = list->next) {
 		PopplerFormField *field;
 		EvFormField *field_mapping;
		double rect[4];	
		
 		field = (PopplerFormField *)list->data;

 		field_mapping = g_new (EvFormField, 1);
		field_mapping->content = NULL;
		field_mapping->type = (EvFormFieldType)field->type;
		field_mapping->id = field->id;
		
		copy_form_poppler_evince(field, field_mapping, height, crop_box);

		if(field_mapping->type == EV_FORM_FIELD_TYPE_TEXT)
			field_mapping->content = poppler_document_get_form_field_text_content(pdf_document->document, field->id);
		if(field_mapping->type == EV_FORM_FIELD_TYPE_BUTTON) {
			field_mapping->num_kids = field->button.num_kids;
			printf("num_kids: %i\n", field_mapping->num_kids);
			field_mapping->kids = g_new(EvFormField, field_mapping->num_kids);
			for (int i=0; i<field_mapping->num_kids; i++) {
				PopplerFormField *poppler_kid;
				EvFormField *kid = &field_mapping->kids[i];
				poppler_kid = poppler_document_get_form_field_button_kid(pdf_document->document, field->id, i);
				if (!poppler_kid) {
					g_warning("pdf_document_get_form_field_mapping : unable to get kid for field with id %i\n", field->id);
					break;
				}
				kid->id = poppler_kid->id;
				kid->type = (EvFormFieldType)poppler_kid->type;
				copy_form_poppler_evince(poppler_kid, kid, height, crop_box);
				g_free(poppler_kid);
			}

		}


 		retval = g_list_prepend(retval, field_mapping);
 	}
 	poppler_page_free_form_fields(fields);
 	g_object_unref (poppler_page);
 
	return g_list_reverse(retval);
}

static void
pdf_document_get_crop_box (EvDocument *document,
			   int page,
			   EvRectangle *rect)
{
	PdfDocument *pdf_document;
	PopplerPage *poppler_page;
	PopplerRectangle poppler_rect;

	pdf_document = PDF_DOCUMENT (document);
	poppler_page = poppler_document_get_page (pdf_document->document, page);
	poppler_page_get_crop_box (poppler_page, &poppler_rect);
	rect->x1 = poppler_rect.x1;
	rect->x2 = poppler_rect.x2;
	rect->y1 = poppler_rect.y1;
	rect->y2 = poppler_rect.y2;
}

static gchar * 
pdf_document_get_text_field_content (EvDocument *document, int field_id)
{
	PdfDocument *pdf_document = PDF_DOCUMENT(document);
	return poppler_document_get_form_field_text_content(pdf_document->document, field_id);
}

static gboolean pdf_document_set_text_field_content (EvDocument *document, int field_id, gchar* content)
{
	PdfDocument *pdf_document = PDF_DOCUMENT(document);
	poppler_document_set_form_field_text_content(pdf_document->document, field_id, content);
	return true;
}

static void 
pdf_document_set_form_field_button_state (EvDocument *document, int field_id, int index, gboolean state)
{
	PdfDocument *pdf_document = PDF_DOCUMENT(document);
	poppler_document_set_form_field_button_state(pdf_document->document, field_id, index, state);
}



