/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8; c-indent-level: 8 -*- */
/* this file is part of evince, a gnome document viewer
 *
 *  Copyright (C) 2004 Red Hat, Inc
 *
 * Evince is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Evince is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
 */

#include <gtk/gtkalignment.h>
#include <glib/gi18n.h>
#include <gtk/gtkbindings.h>
#include <gtk/gtkselection.h>
#include <gtk/gtkclipboard.h>
#include <gdk/gdkkeysyms.h>
#include <libgnomevfs/gnome-vfs-utils.h>

#include "ev-marshal.h"
#include "ev-view.h"
#include "ev-document-find.h"
#include "ev-document-misc.h"
#include "ev-debug.h"
#include "ev-job-queue.h"
#include "ev-page-cache.h"
#include "ev-pixbuf-cache.h"

#define EV_VIEW_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), EV_TYPE_VIEW, EvViewClass))
#define EV_IS_VIEW_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), EV_TYPE_VIEW))
#define EV_VIEW_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), EV_TYPE_VIEW, EvViewClass))

enum {
	PROP_0,
	PROP_STATUS,
	PROP_FIND_STATUS
};

enum {
  TARGET_STRING,
  TARGET_TEXT,
  TARGET_COMPOUND_TEXT,
  TARGET_UTF8_STRING,
  TARGET_TEXT_BUFFER_CONTENTS
};

static const GtkTargetEntry targets[] = {
	{ "STRING", 0, TARGET_STRING },
	{ "TEXT",   0, TARGET_TEXT },
	{ "COMPOUND_TEXT", 0, TARGET_COMPOUND_TEXT },
	{ "UTF8_STRING", 0, TARGET_UTF8_STRING },
};

typedef enum {
	EV_VIEW_CURSOR_NORMAL,
	EV_VIEW_CURSOR_LINK,
	EV_VIEW_CURSOR_WAIT,
	EV_VIEW_CURSOR_HIDDEN
} EvViewCursor;

#define ZOOM_IN_FACTOR  1.2
#define ZOOM_OUT_FACTOR (1.0/ZOOM_IN_FACTOR)

#define MIN_SCALE 0.05409
#define MAX_SCALE 18.4884

struct _EvView {
	GtkWidget parent_instance;

	EvDocument *document;
	
	GdkWindow *bin_window;

	char *status;
	char *find_status;
	
	int scroll_x;
	int scroll_y;

	gboolean pressed_button;
	gboolean has_selection;
	GdkPoint selection_start;
	GdkRectangle selection;
	EvViewCursor cursor;

	GtkAdjustment *hadjustment;
	GtkAdjustment *vadjustment;

	EvPageCache *page_cache;
	EvPixbufCache *pixbuf_cache;

	gint current_page;
	EvJobRender *current_job;

	int find_page;
	int find_result;
	int spacing;

	double scale;
	int width;
	int height;
};

struct _EvViewClass {
	GtkWidgetClass parent_class;

	void	(*set_scroll_adjustments) (EvView         *view,
					   GtkAdjustment  *hadjustment,
					   GtkAdjustment  *vadjustment);
	void    (*scroll_view)		  (EvView         *view,
					   GtkScrollType   scroll,
					   gboolean        horizontal);
	
	/* Should this be notify::page? */
	void	(*page_changed)           (EvView         *view);
};

static guint page_changed_signal = 0;

static void ev_view_set_scroll_adjustments (EvView         *view,
					    GtkAdjustment  *hadjustment,
					    GtkAdjustment  *vadjustment);
    
G_DEFINE_TYPE (EvView, ev_view, GTK_TYPE_WIDGET)

/*** Helper functions ***/       
     
static void
view_update_adjustments (EvView *view)
{
	int old_x = view->scroll_x;
	int old_y = view->scroll_y;
  
	if (view->hadjustment)
		view->scroll_x = view->hadjustment->value;
	else
		view->scroll_x = 0;

	if (view->vadjustment)
		view->scroll_y = view->vadjustment->value;
	else
		view->scroll_y = 0;
  
	if (GTK_WIDGET_REALIZED (view) &&
	    (view->scroll_x != old_x || view->scroll_y != old_y)) {
		gdk_window_move (view->bin_window, - view->scroll_x, - view->scroll_y);
		gdk_window_process_updates (view->bin_window, TRUE);
	}
}

static void
view_set_adjustment_values (EvView         *view,
			    GtkOrientation  orientation)
{
	GtkWidget *widget = GTK_WIDGET (view);
	GtkAdjustment *adjustment;
	gboolean value_changed = FALSE;
	int requisition;
	int allocation;

	if (orientation == GTK_ORIENTATION_HORIZONTAL)  {
		requisition = widget->requisition.width;
		allocation = widget->allocation.width;
		adjustment = view->hadjustment;
	} else {
		requisition = widget->requisition.height;
		allocation = widget->allocation.height;
		adjustment = view->vadjustment;
	}

	if (!adjustment)
		return;
  
	adjustment->page_size = allocation;
	adjustment->step_increment = allocation * 0.1;
	adjustment->page_increment = allocation * 0.9;
	adjustment->lower = 0;
	adjustment->upper = MAX (allocation, requisition);

	if (adjustment->value > adjustment->upper - adjustment->page_size) {
		adjustment->value = adjustment->upper - adjustment->page_size;
		value_changed = TRUE;
	}

	gtk_adjustment_changed (adjustment);
	if (value_changed)
		gtk_adjustment_value_changed (adjustment);
}

/*** Virtual function implementations ***/       
     
static void
ev_view_finalize (GObject *object)
{
	EvView *view = EV_VIEW (object);

	LOG ("Finalize");


	ev_view_set_scroll_adjustments (view, NULL, NULL);

	G_OBJECT_CLASS (ev_view_parent_class)->finalize (object);
}

static void
ev_view_destroy (GtkObject *object)
{
	EvView *view = EV_VIEW (object);

	if (view->document) {
		g_object_unref (view->document);
		view->document = NULL;
	}
	if (view->pixbuf_cache) {
		g_object_unref (view->pixbuf_cache);
		view->pixbuf_cache = NULL;
	}
	ev_view_set_scroll_adjustments (view, NULL, NULL);
  
	GTK_OBJECT_CLASS (ev_view_parent_class)->destroy (object);
}

static void
ev_view_get_offsets (EvView *view, int *x_offset, int *y_offset)
{
	EvDocument *document = view->document;
	GtkWidget *widget = GTK_WIDGET (view);
	int width, height, target_width, target_height;
	GtkBorder border;

	g_return_if_fail (EV_IS_DOCUMENT (document));

	ev_page_cache_get_size (view->page_cache,
				view->current_page,
				view->scale,
				&width, &height);

	ev_document_misc_get_page_border_size (width, height, &border);
	
	*x_offset = view->spacing;
	*y_offset = view->spacing;
	target_width = width + border.left + border.right + view->spacing * 2;
	target_height = height + border.top + border.bottom + view->spacing * 2;
	*x_offset += MAX (0, (widget->allocation.width - target_width) / 2);
	*y_offset += MAX (0, (widget->allocation.height - target_height) / 2);
}

static void
view_rect_to_doc_rect (EvView *view, GdkRectangle *view_rect, GdkRectangle *doc_rect)
{
	int x_offset, y_offset;

	ev_view_get_offsets (view, &x_offset, &y_offset); 
	doc_rect->x = (view_rect->x - x_offset) / view->scale;
	doc_rect->y = (view_rect->y - y_offset) / view->scale;
	doc_rect->width = view_rect->width / view->scale;
	doc_rect->height = view_rect->height / view->scale;
}

static void
doc_rect_to_view_rect (EvView *view, GdkRectangle *doc_rect, GdkRectangle *view_rect)
{
	int x_offset, y_offset;

	ev_view_get_offsets (view, &x_offset, &y_offset); 
	view_rect->x = doc_rect->x * view->scale + x_offset;
	view_rect->y = doc_rect->y * view->scale + y_offset;
	view_rect->width = doc_rect->width * view->scale;
	view_rect->height = doc_rect->height * view->scale;
}


/* Called by size_request to make sure we have appropriate jobs running.
 */
static void
ev_view_size_request (GtkWidget      *widget,
		      GtkRequisition *requisition)
{
	EvView *view = EV_VIEW (widget);
	GtkBorder border;
	gint width, height;

	if (!GTK_WIDGET_REALIZED (widget))
		return;

	if (!view->document) {
		requisition->width = 1;
		requisition->height = 1;
		return;
	}

	ev_page_cache_get_size (view->page_cache,
				view->current_page,
				view->scale,
				&width, &height);

	ev_pixbuf_cache_set_page_range (view->pixbuf_cache,
					view->current_page,
					view->current_page,
					view->scale);
	ev_document_misc_get_page_border_size (width, height, &border);

	if (view->width >= 0) {
		requisition->width = 0;
	} else {
		requisition->width = width + border.left + border.right +
				     view->spacing * 2;
	}
	
	if (view->height >= 0) {
		requisition->height = 0;
	} else {
		requisition->height = height + border.top + border.bottom +
				      view->spacing * 2;
	}
}

static void
ev_view_size_allocate (GtkWidget      *widget,
		       GtkAllocation  *allocation)
{
	EvView *view = EV_VIEW (widget);

	GTK_WIDGET_CLASS (ev_view_parent_class)->size_allocate (widget, allocation);

	view_set_adjustment_values (view, GTK_ORIENTATION_HORIZONTAL);
	view_set_adjustment_values (view, GTK_ORIENTATION_VERTICAL);

	if (GTK_WIDGET_REALIZED (widget)) {
		gdk_window_resize (view->bin_window,
				   MAX (widget->allocation.width, widget->requisition.width),
				   MAX (widget->allocation.height, widget->requisition.height));
	}
}

static void
ev_view_realize (GtkWidget *widget)
{
	EvView *view = EV_VIEW (widget);
	GdkWindowAttr attributes;

	GTK_WIDGET_SET_FLAGS (widget, GTK_REALIZED);
  

	attributes.window_type = GDK_WINDOW_CHILD;
	attributes.wclass = GDK_INPUT_OUTPUT;
	attributes.visual = gtk_widget_get_visual (widget);
	attributes.colormap = gtk_widget_get_colormap (widget);
  
	attributes.x = widget->allocation.x;
	attributes.y = widget->allocation.y;
	attributes.width = widget->allocation.width;
	attributes.height = widget->allocation.height;
	attributes.event_mask = 0;
  
	widget->window = gdk_window_new (gtk_widget_get_parent_window (widget),
					 &attributes,
					 GDK_WA_X | GDK_WA_Y |
					 GDK_WA_COLORMAP |
					 GDK_WA_VISUAL);
	gdk_window_set_user_data (widget->window, widget);
	widget->style = gtk_style_attach (widget->style, widget->window);
	gdk_window_set_background (widget->window, &widget->style->mid[widget->state]);
  
	attributes.x = 0;
	attributes.y = 0;
	attributes.width = MAX (widget->allocation.width, widget->requisition.width);
	attributes.height = MAX (widget->allocation.height, widget->requisition.height);
	attributes.event_mask = GDK_EXPOSURE_MASK |
				GDK_BUTTON_PRESS_MASK |
				GDK_BUTTON_RELEASE_MASK |
				GDK_SCROLL_MASK |
				GDK_KEY_PRESS_MASK |
				GDK_POINTER_MOTION_MASK |
		                GDK_LEAVE_NOTIFY_MASK;
  
	view->bin_window = gdk_window_new (widget->window,
					   &attributes,
					   GDK_WA_X | GDK_WA_Y |
					   GDK_WA_COLORMAP |
					   GDK_WA_VISUAL);
	gdk_window_set_user_data (view->bin_window, widget);
	gdk_window_show (view->bin_window);

	widget->style = gtk_style_attach (widget->style, view->bin_window);
	gdk_window_set_background (view->bin_window, &widget->style->mid[widget->state]);

	if (view->document) {
		/* We can't get page size without a target, so we have to
		 * queue a size request at realization. Could be fixed
		 * with EvDocument changes to allow setting a GdkScreen
		 * without setting a target.
		 */
		gtk_widget_queue_resize (widget);
	}
}

static void
ev_view_unrealize (GtkWidget *widget)
{
	EvView *view = EV_VIEW (widget);

	gdk_window_set_user_data (view->bin_window, NULL);
	gdk_window_destroy (view->bin_window);
	view->bin_window = NULL;

	GTK_WIDGET_CLASS (ev_view_parent_class)->unrealize (widget);
}

static guint32
ev_gdk_color_to_rgb (const GdkColor *color)
{
  guint32 result;
  result = (0xff0000 | (color->red & 0xff00));
  result <<= 8;
  result |= ((color->green & 0xff00) | (color->blue >> 8));
  return result;
}

static void
draw_rubberband (GtkWidget *widget, GdkWindow *window,
		 const GdkRectangle *rect, guchar alpha)
{
	GdkGC *gc;
	GdkPixbuf *pixbuf;
	GdkColor *fill_color_gdk;
	guint fill_color;

	fill_color_gdk = gdk_color_copy (&GTK_WIDGET (widget)->style->base[GTK_STATE_SELECTED]);
	fill_color = ev_gdk_color_to_rgb (fill_color_gdk) << 8 | alpha;

	pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8,
				 rect->width, rect->height);
	gdk_pixbuf_fill (pixbuf, fill_color);

	gdk_draw_pixbuf (window, NULL, pixbuf,
			 0, 0,
			 rect->x,rect->y,
			 rect->width, rect->height,
			 GDK_RGB_DITHER_NONE,
			 0, 0);

	g_object_unref (pixbuf);

	gc = gdk_gc_new (window);
	gdk_gc_set_rgb_fg_color (gc, fill_color_gdk);
	gdk_draw_rectangle (window, gc, FALSE,
			    rect->x, rect->y,
			    rect->width - 1,
			    rect->height - 1);
	g_object_unref (gc);

	gdk_color_free (fill_color_gdk);
}

static void
highlight_find_results (EvView *view)
{
	EvDocumentFind *find;
	int i, results;

	g_return_if_fail (EV_IS_DOCUMENT_FIND (view->document));

	find = EV_DOCUMENT_FIND (view->document);

	results = ev_document_find_get_n_results (find);

	for (i = 0; i < results; i++) {
		GdkRectangle rectangle;
		guchar alpha;

		alpha = (i == view->find_result) ? 0x90 : 0x20;
		ev_document_find_get_result (find, i, &rectangle);
		draw_rubberband (GTK_WIDGET (view), view->bin_window,
				 &rectangle, alpha);
        }
}


static void
expose_bin_window (GtkWidget      *widget,
		   GdkEventExpose *event)
{
	EvView *view = EV_VIEW (widget);
	GtkBorder border;
	gint width, height;
	GdkRectangle area;
	int x_offset, y_offset;
	GdkPixbuf *scaled_image;
	GdkPixbuf *current_pixbuf;

	if (view->document == NULL)
		return;

	ev_view_get_offsets (view, &x_offset, &y_offset); 
	ev_page_cache_get_size (view->page_cache,
				view->current_page,
				view->scale,
				&width, &height);

	ev_document_misc_get_page_border_size (width, height, &border);
	
	/* Paint the frame */
	area.x = x_offset;
	area.y = y_offset;
	area.width = width + border.left + border.right;
	area.height = height + border.top + border.bottom;
	ev_document_misc_paint_one_page (view->bin_window, widget, &area, &border);

	/* Render the document itself */
	LOG ("Render area %d %d %d %d - Offset %d %d",
	     event->area.x, event->area.y,
             event->area.width, event->area.height,
	     x_offset, y_offset);

	current_pixbuf = ev_pixbuf_cache_get_pixbuf (view->pixbuf_cache, view->current_page);

	if (current_pixbuf == NULL)
		scaled_image = NULL;
	else if (width == gdk_pixbuf_get_width (current_pixbuf) &&
		 height == gdk_pixbuf_get_height (current_pixbuf))
		scaled_image = g_object_ref (current_pixbuf);
	else
		scaled_image = gdk_pixbuf_scale_simple (current_pixbuf,
							width, height,
							GDK_INTERP_NEAREST);
	if (scaled_image) {
		gdk_draw_pixbuf (view->bin_window,
				 GTK_WIDGET (view)->style->fg_gc[GTK_STATE_NORMAL],
				 scaled_image,
				 0, 0,
				 area.x + border.left,
				 area.y + border.top,
				 width, height,
				 GDK_RGB_DITHER_NORMAL,
				 0, 0);
		g_object_unref (scaled_image);
	}

	if (EV_IS_DOCUMENT_FIND (view->document)) {
		highlight_find_results (view);
	}

	if (view->has_selection) {
		GdkRectangle rubberband;

		doc_rect_to_view_rect (view, &view->selection, &rubberband);
		if (rubberband.width > 0 && rubberband.height > 0) {
			draw_rubberband (widget, view->bin_window,
					 &rubberband, 0x40);
		}
	}
}

static gboolean
ev_view_expose_event (GtkWidget      *widget,
		      GdkEventExpose *event)
{
	EvView *view = EV_VIEW (widget);

	if (event->window == view->bin_window)
		expose_bin_window (widget, event);
	else
		return GTK_WIDGET_CLASS (ev_view_parent_class)->expose_event (widget, event);

	return FALSE;
}

void
ev_view_select_all (EvView *ev_view)
{
	GtkWidget *widget = GTK_WIDGET (ev_view);
	GdkRectangle selection;
	int width, height;
	int x_offset, y_offset;
	GtkBorder border;

	g_return_if_fail (EV_IS_VIEW (ev_view));

	ev_view_get_offsets (ev_view, &x_offset, &y_offset);
	ev_document_get_page_size (ev_view->document, -1, &width, &height);
	ev_document_misc_get_page_border_size (width, height, &border);

	ev_view->has_selection = TRUE;
	selection.x = x_offset + border.left;
        selection.y = y_offset + border.top;
	selection.width = width;
	selection.height = height;
	view_rect_to_doc_rect (ev_view, &selection, &ev_view->selection);

	gtk_widget_queue_draw (widget);
}

void
ev_view_copy (EvView *ev_view)
{
	GtkClipboard *clipboard;
	GdkRectangle selection;
	char *text;

	doc_rect_to_view_rect (ev_view, &ev_view->selection, &selection);
	text = ev_document_get_text (ev_view->document, &selection);
	clipboard = gtk_widget_get_clipboard (GTK_WIDGET (ev_view),
					      GDK_SELECTION_CLIPBOARD);
	gtk_clipboard_set_text (clipboard, text, -1);
	g_free (text);
}

static void
ev_view_primary_get_cb (GtkClipboard     *clipboard,
			GtkSelectionData *selection_data,
			guint             info,
			gpointer          data)
{
	EvView *ev_view = EV_VIEW (data);
	GdkRectangle selection;
	char *text;

	doc_rect_to_view_rect (ev_view, &ev_view->selection, &selection);
	text = ev_document_get_text (ev_view->document, &selection);
	gtk_selection_data_set_text (selection_data, text, -1);
}

static void
ev_view_primary_clear_cb (GtkClipboard *clipboard,
			  gpointer      data)
{
	EvView *ev_view = EV_VIEW (data);

	ev_view->has_selection = FALSE;
}

static void
ev_view_update_primary_selection (EvView *ev_view)
{
	GtkClipboard *clipboard;

	clipboard = gtk_widget_get_clipboard (GTK_WIDGET (ev_view),
                                              GDK_SELECTION_PRIMARY);

	if (ev_view->has_selection) {
		if (!gtk_clipboard_set_with_owner (clipboard,
						   targets,
						   G_N_ELEMENTS (targets),
						   ev_view_primary_get_cb,
						   ev_view_primary_clear_cb,
						   G_OBJECT (ev_view)))
			ev_view_primary_clear_cb (clipboard, ev_view);
	} else {
		if (gtk_clipboard_get_owner (clipboard) == G_OBJECT (ev_view))
			gtk_clipboard_clear (clipboard);
	}
}

static gboolean
ev_view_button_press_event (GtkWidget      *widget,
			    GdkEventButton *event)
{
	EvView *view = EV_VIEW (widget);

	if (!GTK_WIDGET_HAS_FOCUS (widget)) {
		gtk_widget_grab_focus (widget);
	}

	view->pressed_button = event->button;

	switch (event->button) {
		case 1:
			if (view->has_selection) {
				view->has_selection = FALSE;
				gtk_widget_queue_draw (widget);
			}

			view->selection_start.x = event->x;
			view->selection_start.y = event->y;
			break;
	}

	return TRUE;
}

static char *
status_message_from_link (EvLink *link)
{
	EvLinkType type;
	char *msg;
	int page;

	type = ev_link_get_link_type (link);
	
	switch (type) {
		case EV_LINK_TYPE_TITLE:
			msg = g_strdup (ev_link_get_title (link));
			break;
		case EV_LINK_TYPE_PAGE:
			page = ev_link_get_page (link);
			msg = g_strdup_printf (_("Go to page %d"), page);
			break;
		case EV_LINK_TYPE_EXTERNAL_URI:
			msg = g_strdup (ev_link_get_uri (link));
			break;
		default:
			msg = NULL;
	}

	return msg;
}

static void
ev_view_set_status (EvView *view, const char *message)
{
	g_return_if_fail (EV_IS_VIEW (view));

	if (message != view->status) {
		g_free (view->status);
		view->status = g_strdup (message);
		g_object_notify (G_OBJECT (view), "status");
	}
}

static void
ev_view_set_find_status (EvView *view, const char *message)
{
	g_return_if_fail (EV_IS_VIEW (view));
	
	g_free (view->find_status);
	view->find_status = g_strdup (message);
	g_object_notify (G_OBJECT (view), "find-status");
}

static GdkCursor *
ev_view_create_invisible_cursor(void)
{
       GdkBitmap *empty;
       GdkColor black = { 0, 0, 0, 0 };
       static unsigned char bits[] = { 0x00 };

       empty = gdk_bitmap_create_from_data (NULL, bits, 1, 1);

       return gdk_cursor_new_from_pixmap (empty, empty, &black, &black, 0, 0);
}

static void
ev_view_set_cursor (EvView *view, EvViewCursor new_cursor)
{
	GdkCursor *cursor = NULL;
	GdkDisplay *display;
	GtkWidget *widget;

	if (view->cursor == new_cursor) {
		return;
	}

	widget = gtk_widget_get_toplevel (GTK_WIDGET (view));
	display = gtk_widget_get_display (widget);
	view->cursor = new_cursor;

	switch (new_cursor) {
		case EV_VIEW_CURSOR_NORMAL:
			gdk_window_set_cursor (widget->window, NULL);
			break;
		case EV_VIEW_CURSOR_LINK:
			cursor = gdk_cursor_new_for_display (display, GDK_HAND2);
			break;
		case EV_VIEW_CURSOR_WAIT:
			cursor = gdk_cursor_new_for_display (display, GDK_WATCH);
			break;
                case EV_VIEW_CURSOR_HIDDEN:
                        cursor = ev_view_create_invisible_cursor ();
                        break;

	}

	if (cursor) {
		gdk_window_set_cursor (widget->window, cursor);
		gdk_cursor_unref (cursor);
		gdk_flush();
	}
}

static gboolean
ev_view_motion_notify_event (GtkWidget      *widget,
			     GdkEventMotion *event)
{
	EvView *view = EV_VIEW (widget);

	if (view->pressed_button > 0) {
		GdkRectangle selection;

		view->has_selection = TRUE;
		selection.x = MIN (view->selection_start.x, event->x);
		selection.y = MIN (view->selection_start.y, event->y);
		selection.width = ABS (view->selection_start.x - event->x) + 1;
		selection.height = ABS (view->selection_start.y - event->y) + 1;
		view_rect_to_doc_rect (view, &selection, &view->selection);

		gtk_widget_queue_draw (widget);
	} else if (FALSE && view->document) {
		EvLink *link;

		link = ev_document_get_link (view->document, event->x, event->y);
                if (link) {
			char *msg;

			msg = status_message_from_link (link);
			ev_view_set_status (view, msg);
			ev_view_set_cursor (view, EV_VIEW_CURSOR_LINK);
			g_free (msg);

                        g_object_unref (link);
		} else {
			ev_view_set_status (view, NULL);
			if (view->cursor == EV_VIEW_CURSOR_LINK) {
				ev_view_set_cursor (view, EV_VIEW_CURSOR_NORMAL);
			}
		}
	}

	return TRUE;
}

static gboolean
ev_view_button_release_event (GtkWidget      *widget,
			      GdkEventButton *event)
{
	EvView *view = EV_VIEW (widget);

	view->pressed_button = -1;

	if (view->has_selection) {
		ev_view_update_primary_selection (view);
	} else if (view->document) {
		EvLink *link;

		link = ev_document_get_link (view->document,
					     event->x,
					     event->y);
		if (link) {
			ev_view_go_to_link (view, link);
			g_object_unref (link);
		}
	}

	return FALSE;
}

static void
on_adjustment_value_changed (GtkAdjustment  *adjustment,
			     EvView *view)
{
	view_update_adjustments (view);
}

static void
set_scroll_adjustment (EvView *view,
		       GtkOrientation  orientation,
		       GtkAdjustment  *adjustment)
{
	GtkAdjustment **to_set;

	if (orientation == GTK_ORIENTATION_HORIZONTAL)
		to_set = &view->hadjustment;
	else
		to_set = &view->vadjustment;
  
	if (*to_set != adjustment) {
		if (*to_set) {
			g_signal_handlers_disconnect_by_func (*to_set,
							      (gpointer) on_adjustment_value_changed,
							      view);
			g_object_unref (*to_set);
		}

		*to_set = adjustment;
		view_set_adjustment_values (view, orientation);

		if (*to_set) {
			g_object_ref (*to_set);
			g_signal_connect (*to_set, "value_changed",
					  G_CALLBACK (on_adjustment_value_changed), view);
		}
	}
}

static void
ev_view_set_scroll_adjustments (EvView *view,
				GtkAdjustment  *hadjustment,
				GtkAdjustment  *vadjustment)
{
	set_scroll_adjustment (view, GTK_ORIENTATION_HORIZONTAL, hadjustment);
	set_scroll_adjustment (view, GTK_ORIENTATION_VERTICAL, vadjustment);

	view_update_adjustments (view);
}

static void
add_scroll_binding (GtkBindingSet  *binding_set,
		    guint           keyval,
		    GtkScrollType   scroll,
		    gboolean        horizontal)
{
  guint keypad_keyval = keyval - GDK_Left + GDK_KP_Left;
  
  gtk_binding_entry_add_signal (binding_set, keyval, 0,
                                "scroll_view", 2,
                                GTK_TYPE_SCROLL_TYPE, scroll,
				G_TYPE_BOOLEAN, horizontal);
  gtk_binding_entry_add_signal (binding_set, keypad_keyval, 0,
                                "scroll_view", 2,
                                GTK_TYPE_SCROLL_TYPE, scroll,
				G_TYPE_BOOLEAN, horizontal);
}

static void
ev_view_scroll_view (EvView *view,
		     GtkScrollType scroll,
		     gboolean horizontal)
{
	if (scroll == GTK_SCROLL_PAGE_BACKWARD) {
		ev_view_set_page (view, ev_view_get_page (view) - 1);
	} else if (scroll == GTK_SCROLL_PAGE_FORWARD) {
		ev_view_set_page (view, ev_view_get_page (view) + 1);
	} else {
		GtkAdjustment *adjustment;
		double value;

		if (horizontal) {
			adjustment = view->hadjustment;	
		} else {
			adjustment = view->vadjustment;
		}

		value = adjustment->value;

		switch (scroll) {
			case GTK_SCROLL_STEP_BACKWARD:	
				value -= adjustment->step_increment; 
				break;
			case GTK_SCROLL_STEP_FORWARD:
				value += adjustment->step_increment; 
				break;
			default:
				break;
		}

		value = CLAMP (value, adjustment->lower,
			       adjustment->upper - adjustment->page_size);

		gtk_adjustment_set_value (adjustment, value);
	}
}

static void
ev_view_set_property (GObject *object,
		      guint prop_id,
		      const GValue *value,
		      GParamSpec *pspec)
{
	switch (prop_id)
	{
		/* Read only */
		case PROP_STATUS:
		case PROP_FIND_STATUS:
			break;
	}
}

static void
ev_view_get_property (GObject *object,
		      guint prop_id,
		      GValue *value,
		      GParamSpec *pspec)
{
	EvView *view = EV_VIEW (object);

	switch (prop_id)
	{
		case PROP_STATUS:
			g_value_set_string (value, view->status);
			break;
		case PROP_FIND_STATUS:
			g_value_set_string (value, view->status);
			break;
	}
}

static void
ev_view_class_init (EvViewClass *class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (class);
	GtkObjectClass *gtk_object_class = GTK_OBJECT_CLASS (class);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (class);
	GtkBindingSet *binding_set;

	object_class->finalize = ev_view_finalize;
	object_class->set_property = ev_view_set_property;
	object_class->get_property = ev_view_get_property;

	widget_class->expose_event = ev_view_expose_event;
	widget_class->button_press_event = ev_view_button_press_event;
	widget_class->motion_notify_event = ev_view_motion_notify_event;
	widget_class->button_release_event = ev_view_button_release_event;
	widget_class->size_request = ev_view_size_request;
	widget_class->size_allocate = ev_view_size_allocate;
	widget_class->realize = ev_view_realize;
	widget_class->unrealize = ev_view_unrealize;
	gtk_object_class->destroy = ev_view_destroy;

	class->set_scroll_adjustments = ev_view_set_scroll_adjustments;
	class->scroll_view = ev_view_scroll_view;

	widget_class->set_scroll_adjustments_signal =  g_signal_new ("set-scroll-adjustments",
								     G_OBJECT_CLASS_TYPE (object_class),
								     G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
								     G_STRUCT_OFFSET (EvViewClass, set_scroll_adjustments),
								     NULL, NULL,
								     ev_marshal_VOID__OBJECT_OBJECT,
								     G_TYPE_NONE, 2,
								     GTK_TYPE_ADJUSTMENT,
								     GTK_TYPE_ADJUSTMENT);
	page_changed_signal = g_signal_new ("page-changed",
					    G_OBJECT_CLASS_TYPE (object_class),
					    G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
					    G_STRUCT_OFFSET (EvViewClass, page_changed),
					    NULL, NULL,
					    ev_marshal_VOID__NONE,
					    G_TYPE_NONE, 0);

	g_signal_new ("scroll_view",
		      G_TYPE_FROM_CLASS (object_class),
		      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
		      G_STRUCT_OFFSET (EvViewClass, scroll_view),
		      NULL, NULL,
		      ev_marshal_VOID__ENUM_BOOLEAN,
		      G_TYPE_NONE, 2,
		      GTK_TYPE_SCROLL_TYPE,
		      G_TYPE_BOOLEAN);

	g_object_class_install_property (object_class,
					 PROP_STATUS,
					 g_param_spec_string ("status",
							      "Status Message",
							      "The status message",
							      NULL,
							      G_PARAM_READABLE));

	g_object_class_install_property (object_class,
					 PROP_STATUS,
					 g_param_spec_string ("find-status",
							      "Find Status Message",
							      "The find status message",
							      NULL,
							      G_PARAM_READABLE));

	binding_set = gtk_binding_set_by_class (class);

	add_scroll_binding (binding_set, GDK_Left,  GTK_SCROLL_STEP_BACKWARD, TRUE);
	add_scroll_binding (binding_set, GDK_Right, GTK_SCROLL_STEP_FORWARD,  TRUE);
	add_scroll_binding (binding_set, GDK_Up,    GTK_SCROLL_STEP_BACKWARD, FALSE);
	add_scroll_binding (binding_set, GDK_Down,  GTK_SCROLL_STEP_FORWARD,  FALSE);

	add_scroll_binding (binding_set, GDK_Page_Up,   GTK_SCROLL_PAGE_BACKWARD, FALSE);
	add_scroll_binding (binding_set, GDK_Page_Down, GTK_SCROLL_PAGE_FORWARD,  FALSE);
}

static void
ev_view_init (EvView *view)
{
	GTK_WIDGET_SET_FLAGS (view, GTK_CAN_FOCUS);

	view->spacing = 10;
	view->scale = 1.0;
	view->current_page = 1;
	view->pressed_button = -1;
	view->cursor = EV_VIEW_CURSOR_NORMAL;
}

static void
update_find_status_message (EvView *view)
{
	char *message;

	if (ev_document_get_page (view->document) == view->find_page) {
		int results;

		results = ev_document_find_get_n_results
				(EV_DOCUMENT_FIND (view->document));

		/* TRANS: Sometimes this could be better translated as
		   "%d hit(s) on this page".  Therefore this string
		   contains plural cases. */
		message = g_strdup_printf (ngettext ("%d found on this page",
						     "%d found on this page",
						     results),
					   results);
	} else {
		double percent;
		
		percent = ev_document_find_get_progress
				(EV_DOCUMENT_FIND (view->document));

		if (percent >= (1.0 - 1e-10)) {
			message = g_strdup (_("Not found"));
		} else {
			message = g_strdup_printf (_("%3d%% remaining to search"),
						   (int) ((1.0 - percent) * 100));
		}
		
	}

	ev_view_set_find_status (view, message);
	g_free (message);
}

static void
set_document_page (EvView *view, int new_page)
{
	int page;
	int n_pages;

	n_pages = ev_page_cache_get_n_pages (view->page_cache);
	page = CLAMP (new_page, 1, n_pages);

	if (view->document) {
		int old_page = view->current_page;
		int old_width, old_height;

		ev_document_get_page_size (view->document,
					   -1, 
					   &old_width, &old_height);

		if (old_page != page) {
			if (view->cursor != EV_VIEW_CURSOR_HIDDEN) {
				//ev_view_set_cursor (view, EV_VIEW_CURSOR_WAIT);
			}
			view->current_page = page;
			ev_pixbuf_cache_set_page_range (view->pixbuf_cache,
							view->current_page,
							view->current_page,
							view->scale);
		}

		if (old_page != view->current_page) {
//			int width, height;
			
			g_signal_emit (view, page_changed_signal, 0);

			view->has_selection = FALSE;
#if 0
			ev_document_get_page_size (view->document,
						   page, 
						   &width, &height);
			if (width != old_width || height != old_height)
#endif
				gtk_widget_queue_resize (GTK_WIDGET (view));

			gtk_adjustment_set_value (view->vadjustment,
						  view->vadjustment->lower);
		}

		if (EV_IS_DOCUMENT_FIND (view->document)) {
			view->find_page = page;
			view->find_result = 0;
			update_find_status_message (view);
		}
	}
}

#define MARGIN 5

static void
ensure_rectangle_is_visible (EvView *view, GdkRectangle *rect)
{
	GtkWidget *widget = GTK_WIDGET (view);
	GtkAdjustment *adjustment;
	int value;

	adjustment = view->vadjustment;

	if (rect->y < adjustment->value) {
		value = MAX (adjustment->lower, rect->y - MARGIN);
		gtk_adjustment_set_value (view->vadjustment, value);
	} else if (rect->y + rect->height >
		   adjustment->value + widget->allocation.height) {
		value = MIN (adjustment->upper, rect->y + rect->height -
			     widget->allocation.height + MARGIN);
		gtk_adjustment_set_value (view->vadjustment, value);
	}

	adjustment = view->hadjustment;

	if (rect->x < adjustment->value) {
		value = MAX (adjustment->lower, rect->x - MARGIN);
		gtk_adjustment_set_value (view->hadjustment, value);
	} else if (rect->x + rect->height >
		   adjustment->value + widget->allocation.width) {
		value = MIN (adjustment->upper, rect->x + rect->width -
			     widget->allocation.width + MARGIN);
		gtk_adjustment_set_value (view->hadjustment, value);
	}
}

static void
jump_to_find_result (EvView *view)
{
	EvDocumentFind *find = EV_DOCUMENT_FIND (view->document);
	GdkRectangle rect;
	int n_results;

	n_results = ev_document_find_get_n_results (find);

	if (n_results > view->find_result) {
		ev_document_find_get_result
			(find, view->find_result, &rect);
		ensure_rectangle_is_visible (view, &rect);
	}
}

static void
jump_to_find_page (EvView *view)
{
	int n_pages, i;

	n_pages = ev_document_get_n_pages (view->document);

	for (i = 0; i <= n_pages; i++) {
		int has_results;
		int page;

		page = i + view->find_page;
		if (page > n_pages) {
			page = page - n_pages;
		}

		has_results = ev_document_find_page_has_results
				(EV_DOCUMENT_FIND (view->document), page);
		if (has_results == -1) {
			view->find_page = page;
			break;
		} else if (has_results == 1) {
			set_document_page (view, page);
			jump_to_find_result (view);
			break;
		}
	}
}

static void
find_changed_cb (EvDocument *document, int page, EvView *view)
{
	jump_to_find_page (view);
	jump_to_find_result (view);
	update_find_status_message (view);

	if (ev_document_get_page (document) == page) {
		gtk_widget_queue_draw (GTK_WIDGET (view));
	}
}
/*** Public API ***/       
     
GtkWidget*
ev_view_new (void)
{
	return g_object_new (EV_TYPE_VIEW, NULL);
}

static void
job_finished_cb (EvPixbufCache *pixbuf_cache,
		 EvView        *view)
{
	gtk_widget_queue_draw (GTK_WIDGET (view));
}

void
ev_view_set_document (EvView     *view,
		      EvDocument *document)
{
	g_return_if_fail (EV_IS_VIEW (view));

	if (document != view->document) {
		if (view->document) {
                        g_signal_handlers_disconnect_by_func (view->document,
                                                              find_changed_cb,
                                                              view);
			g_object_unref (view->document);
			g_object_unref (view->page_cache);
                }

		view->document = document;
		view->find_page = 1;
		view->find_result = 0;

		if (view->document) {
			g_object_ref (view->document);
			if (EV_IS_DOCUMENT_FIND (view->document)) {
				g_signal_connect (view->document,
						  "find_changed",
						  G_CALLBACK (find_changed_cb),
						  view);
			}
			view->page_cache = ev_document_get_page_cache (view->document);
			view->pixbuf_cache = ev_pixbuf_cache_new (view->document);
			g_signal_connect (view->pixbuf_cache, "job-finished", G_CALLBACK (job_finished_cb), view);
                }
		
		gtk_widget_queue_resize (GTK_WIDGET (view));
		
		g_signal_emit (view, page_changed_signal, 0);
	}
}

static void
go_to_link (EvView *view, EvLink *link)
{
	EvLinkType type;
	const char *uri;
	int page;

	type = ev_link_get_link_type (link);
	
	switch (type) {
		case EV_LINK_TYPE_TITLE:
			break;
		case EV_LINK_TYPE_PAGE:
			page = ev_link_get_page (link);
			set_document_page (view, page);
			break;
		case EV_LINK_TYPE_EXTERNAL_URI:
			uri = ev_link_get_uri (link);
			gnome_vfs_url_show (uri);
			break;
	}
}

void
ev_view_go_to_link (EvView *view, EvLink *link)
{
	go_to_link (view, link);
}

void
ev_view_set_page (EvView *view,
		  int     page)
{
	g_return_if_fail (EV_IS_VIEW (view));

	set_document_page (view, page);
}

int
ev_view_get_page (EvView *view)
{
	if (view->document)
		return ev_document_get_page (view->document);
	else
		return 1;
}

static void
ev_view_zoom (EvView   *view,
	      double    factor,
	      gboolean  relative)
{
	double scale;

	if (relative)
		scale = view->scale * factor;
	else
		scale = factor;

	scale = CLAMP (scale, MIN_SCALE, MAX_SCALE);

	view->scale = scale;
#if 0
	gtk_widget_queue_resize (GTK_WIDGET (view));
	ev_document_set_scale (view->document, view->scale);
#endif
}

void
ev_view_zoom_in (EvView *view)
{
	view->width = view->height = -1;
	ev_view_zoom (view, ZOOM_IN_FACTOR, TRUE);
}

void
ev_view_zoom_out (EvView *view)
{
	view->width = view->height = -1;
	ev_view_zoom (view, ZOOM_OUT_FACTOR, TRUE);
}

static double
size_to_zoom_factor (EvView *view, int width, int height)
{
	int doc_width, doc_height;
	double scale, scale_w, scale_h;
	GtkBorder border;

	doc_width = doc_height = 0;
	scale = scale_w = scale_h = 1.0;
	ev_page_cache_get_size (view->page_cache,
				view->current_page,
				view->scale,
				&doc_width,
				&doc_height);

	/* FIXME: The border size isn't constant.  Ugh.  Still, if we have extra
	 * space, we just cut it from the border */
	ev_document_misc_get_page_border_size (doc_width, doc_height, &border);

	if (doc_width == 0 && doc_height == 0) {
		return 0;
	}

	if (width >= 0) {
		int target_width;

		target_width = width - (view->spacing * 2 + border.left + border.right);
		scale = scale_w = (double)target_width * view->scale / doc_width;
	}

	if (height >= 0) {
		int target_height;

		target_height = height - (view->spacing * 2 + border.top + border.bottom);
		scale = scale_h = (double)target_height * view->scale / doc_height;
	}

	if (width >= 0 && height >= 0) {
		scale = (scale_w < scale_h) ? scale_w : scale_h;
	}

	return scale;
}

void
ev_view_set_size (EvView     *view,
		  int         width,
		  int         height)
{
	double factor;

	if (!view->document)
		return;

	if (view->width != width ||
	    view->height != height) {
		view->width = width;
		view->height = height;
		factor = size_to_zoom_factor (view, width, height);
		ev_view_zoom (view, factor, FALSE);
		gtk_widget_queue_resize (GTK_WIDGET (view));
	}
}

const char *
ev_view_get_status (EvView *view)
{
	g_return_val_if_fail (EV_IS_VIEW (view), NULL);

	return view->status;
}

const char *
ev_view_get_find_status (EvView *view)
{
	g_return_val_if_fail (EV_IS_VIEW (view), NULL);

	return view->find_status;
}

void
ev_view_find_next (EvView *view)
{
	int n_results, n_pages;
	EvDocumentFind *find = EV_DOCUMENT_FIND (view->document);

	n_results = ev_document_find_get_n_results (find);
	n_pages = ev_document_get_n_pages (view->document);

	view->find_result++;

	if (view->find_result >= n_results) {
		view->find_result = 0;
		view->find_page++;

		if (view->find_page > n_pages) {
			view->find_page = 1;
		}

		jump_to_find_page (view);
	} else {
		jump_to_find_result (view);
		gtk_widget_queue_draw (GTK_WIDGET (view));
	}
}

void
ev_view_find_previous (EvView *view)
{
	int n_results, n_pages;
	EvDocumentFind *find = EV_DOCUMENT_FIND (view->document);

	n_results = ev_document_find_get_n_results (find);
	n_pages = ev_document_get_n_pages (view->document);

	view->find_result--;

	if (view->find_result < 0) {
		view->find_result = 0;
		view->find_page--;

		if (view->find_page < 1) {
			view->find_page = n_pages;
		}

		jump_to_find_page (view);
	} else {
		jump_to_find_result (view);
		gtk_widget_queue_draw (GTK_WIDGET (view));
	}
}
void
ev_view_hide_cursor (EvView *view)
{
       ev_view_set_cursor (view, EV_VIEW_CURSOR_HIDDEN);
}

void
ev_view_show_cursor (EvView *view)
{
       ev_view_set_cursor (view, EV_VIEW_CURSOR_LINK);
}
