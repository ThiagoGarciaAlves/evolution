/* Evolution calendar - Calendar properties dialogs
 *
 * Copyright (C) 2003 Novell, Inc.
 *
 * Author: Hans Petter Jansson <hpj@ximian.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <bonobo/bonobo-i18n.h>
#include <gtk/gtkdialog.h>
#include <gtk/gtkentry.h>
#include <gtk/gtkmenu.h>
#include <gtk/gtkmenuitem.h>
#include <gtk/gtkmessagedialog.h>
#include <gtk/gtkoptionmenu.h>
#include <libgnome/libgnome.h>
#include <libgnomeui/gnome-druid.h>
#include <libgnomeui/gnome-druid-page.h>
#include <libgnomeui/gnome-color-picker.h>
#include <glade/glade.h>
#include <libedataserver/e-source-list.h>
#include <libecal/e-cal.h>
#include <e-util/e-dialog-utils.h>
#include <e-util/e-url.h>
#include "calendar-setup.h"

#define GLADE_FILE_NAME "calendar-setup.glade"

typedef struct
{
	GladeXML     *gui_xml;

	/* Main widgets */
	GtkWidget    *window;
	GtkWidget    *druid;

	/* Source selection */
	ESourceList  *source_list;
	GtkWidget    *group_optionmenu;

	/* ESource we're currently editing (if any) */
	ESource      *source;

	/* Source group we're creating/editing source in */
	ESourceGroup *source_group;

	/* General page fields */
	GtkWidget    *name_entry;
	GtkWidget    *source_color;

	/* Location page fields */
	GtkWidget    *uri_entry;
	GtkWidget    *refresh_spin;

	/* sensitive blocks */
	GtkWidget    *uri_label;
	GtkWidget    *uri_hbox;
	GtkWidget    *refresh_label;
	GtkWidget    *refresh_hbox;
	GtkWidget    *refresh_optionmenu;
	GtkWidget    *add_button;
}
SourceDialog;

static gchar *
print_uri_noproto (EUri *uri)
{
	gchar *uri_noproto;

	if (uri->port != 0)
		uri_noproto = g_strdup_printf (
			"%s%s%s%s%s%s%s:%d%s%s%s",
			uri->user ? uri->user : "",
			uri->authmech ? ";auth=" : "",
			uri->authmech ? uri->authmech : "",
			uri->passwd ? ":" : "",
			uri->passwd ? uri->passwd : "",
			uri->user ? "@" : "",
			uri->host ? uri->host : "",
			uri->port,
			uri->path ? uri->path : "",
			uri->query ? "?" : "",
			uri->query ? uri->query : "");
	else
		uri_noproto = g_strdup_printf (
                        "%s%s%s%s%s%s%s%s%s%s",
                        uri->user ? uri->user : "",
                        uri->authmech ? ";auth=" : "",
                        uri->authmech ? uri->authmech : "",
                        uri->passwd ? ":" : "",
                        uri->passwd ? uri->passwd : "",
                        uri->user ? "@" : "",
                        uri->host ? uri->host : "",
                        uri->path ? uri->path : "",
                        uri->query ? "?" : "",
                        uri->query ? uri->query : "");

	return uri_noproto;
}

static gboolean
source_group_is_remote (ESourceGroup *group)
{
	EUri     *uri;
	gboolean  is_remote = FALSE;

	uri = e_uri_new (e_source_group_peek_base_uri (group));
	if (!uri)
		return FALSE;

	if (uri->protocol && uri->protocol [0] && strcmp (uri->protocol, "file"))
		is_remote = TRUE;

	e_uri_free (uri);
	return is_remote;
}

static gboolean
source_is_remote (ESource *source)
{
	gchar    *uri_str;
	EUri     *uri;
	gboolean  is_remote = FALSE;

	uri_str = e_source_get_uri (source);
	if (!uri_str)
		return FALSE;

	uri = e_uri_new (uri_str);
	g_free (uri_str);

	if (!uri)
		return FALSE;

	if (uri->protocol && uri->protocol [0] && strcmp (uri->protocol, "file"))
		is_remote = TRUE;

	e_uri_free (uri);
	return is_remote;
}

static gboolean
validate_remote_uri (const gchar *source_location, gboolean interactive, GtkWidget *parent)
{
	EUri *uri;

	if (!source_location || !strlen (source_location)) {
		if (interactive)
			e_notice (parent, GTK_MESSAGE_ERROR,
				  _("You must specify a location to get the calendar from."));
		return FALSE;
	}

	uri = e_uri_new (source_location);
	if (!uri) {
		if (interactive)
			e_notice (parent, GTK_MESSAGE_ERROR,
				  _("The source location '%s' is not well-formed."),
				  source_location);
		return FALSE;
	}

	/* Make sure we're in agreement with the protocol. Note that EUri sets it
	 * to 'file' if none was specified in the input URI. We don't want to
	 * silently translate an explicit file:// into http:// though. */
	if (uri->protocol &&
	    strcmp (uri->protocol, "http") &&
	    strcmp (uri->protocol, "webcal")) {
		e_uri_free (uri);

		if (interactive)
			e_notice (parent, GTK_MESSAGE_ERROR,
				  _("The source location '%s' is not a webcal source."),
				  source_location);
		return FALSE;
	}

	e_uri_free (uri);
	return TRUE;
}

static int
source_group_menu_add_groups (GtkMenuShell *menu_shell, ESourceList *source_list)
{
	GSList *groups, *sl;
	int index=-1, i=0;

	groups = e_source_list_peek_groups (source_list);
	for (sl = groups; sl; sl = g_slist_next (sl)) {
		GtkWidget    *menu_item;
		ESourceGroup *group = sl->data;

		menu_item = gtk_menu_item_new_with_label (e_source_group_peek_name (group));
		gtk_widget_show (menu_item);
		if (e_source_group_get_readonly(group))
			gtk_widget_set_sensitive(menu_item, FALSE);
		else if (i == -1)
			index = i;

		gtk_menu_shell_append (menu_shell, menu_item);
	}

	return index;
}

static ESource *
create_new_source_with_group (GtkWindow *parent,
			      ESourceGroup *group,
			      const char *source_name,
			      const char *source_location,
			      ECalSourceType source_type)
{
	ESource *source;
	ECal *cal;

	if (e_source_group_peek_source_by_name (group, source_name)) {
		e_notice (parent, GTK_MESSAGE_ERROR,
			  _("Source with name '%s' already exists in the selected group"),
			  source_name);
		return NULL;
	}

	if (source_group_is_remote (group)) {
		EUri  *uri;
		gchar *relative_uri;

		/* Remote source */

		if (!source_location || !strlen (source_location)) {
			e_notice (parent, GTK_MESSAGE_ERROR,
				  _("The group '%s' is remote. You must specify a location "
				    "to get the calendar from"),
				  e_source_group_peek_name (group));
			return NULL;
		}

		if (!validate_remote_uri (source_location, TRUE, GTK_WIDGET (parent)))
			return NULL;

		/* Our relative_uri is everything but protocol, which is supplied by parent group */
		uri = e_uri_new (source_location);
		relative_uri = print_uri_noproto (uri);
		e_uri_free (uri);

		/* Create source */
		source = e_source_new (source_name, relative_uri);

		g_free (relative_uri);
	} else {
		/* Local source */
		source = e_source_new (source_name, source_name);
		e_source_set_relative_uri (source, e_source_peek_uid (source));
	}

	e_source_group_add_source (group, source, -1);

	/* create the calendar in the backend */
	cal = e_cal_new (source, source_type);
	if (!e_cal_open (cal, FALSE, NULL)) {
		e_source_group_remove_source (group, source);
		g_object_unref (source);
		source = NULL;
	}

	g_object_unref (cal);

	return source;
}

static void
source_dialog_destroy (SourceDialog *source_dialog)
{
	g_object_unref (source_dialog->gui_xml);

	if (source_dialog->source)
		g_object_unref (source_dialog->source);

	g_free (source_dialog);
}

static gboolean
general_page_verify (SourceDialog *source_dialog)
{
	const gchar *name;

	if (!source_dialog->source_group && !source_dialog->source)
		return FALSE;

	name = gtk_entry_get_text (GTK_ENTRY (source_dialog->name_entry));
	if (!name || !name [0])
		return FALSE;

	return TRUE;
}

static gboolean
remote_page_verify (SourceDialog *source_dialog)
{
	const gchar *uri;

	uri = gtk_entry_get_text (GTK_ENTRY (source_dialog->uri_entry));
	if (!uri || !uri [0])
		return FALSE;

	if (!validate_remote_uri (uri, FALSE, NULL))
		return FALSE;

	return TRUE;
}

static void
general_entry_modified (SourceDialog *source_dialog)
{
	const char *text = gtk_entry_get_text (GTK_ENTRY (source_dialog->name_entry));
	gboolean sensitive = text && *text != '\0';
	
	if (source_group_is_remote (source_dialog->source_group)) {
		sensitive &= remote_page_verify (source_dialog);
	}
	
	gtk_widget_set_sensitive (source_dialog->add_button, sensitive);
}

static void
general_update_dialog (SourceDialog *source_dialog)
{
	gboolean remote = FALSE;

	/* These are calendar specific so make sure we have them */
	if (source_dialog->uri_entry)
		g_signal_handlers_block_matched (source_dialog->uri_entry, G_SIGNAL_MATCH_DATA,
						 0, 0, NULL, NULL, source_dialog);

	remote = (source_dialog->source && source_is_remote (source_dialog->source)) 
		|| source_group_is_remote (source_dialog->source_group);

	if (!remote) {
		if (source_dialog->uri_entry)
			gtk_entry_set_text (GTK_ENTRY (source_dialog->uri_entry), "");
		if (source_dialog->refresh_spin)
			gtk_spin_button_set_value (GTK_SPIN_BUTTON (source_dialog->refresh_spin), 30);
	}
	
	general_entry_modified (source_dialog);

	if (source_dialog->uri_hbox)
		gtk_widget_set_sensitive (source_dialog->uri_hbox, remote);
	if (source_dialog->uri_label)
		gtk_widget_set_sensitive (source_dialog->uri_label, remote);
	if (source_dialog->refresh_label)
		gtk_widget_set_sensitive (source_dialog->refresh_label, remote);
	if (source_dialog->refresh_hbox)
		gtk_widget_set_sensitive (source_dialog->refresh_hbox, remote);

	if (source_dialog->uri_entry)
		g_signal_handlers_unblock_matched (source_dialog->uri_entry, G_SIGNAL_MATCH_DATA,
						   0, 0, NULL, NULL, source_dialog);
}

static void
colorpicker_set_color (GnomeColorPicker *color, guint32 rgb)
{
	gnome_color_picker_set_i8 (color, (rgb & 0xff0000) >> 16, (rgb & 0xff00) >> 8, rgb & 0xff, 0xff);
}

static guint32
colorpicker_get_color (GnomeColorPicker *color)
{
	guint8 r, g, b, a;
	guint32 rgb = 0;
	
	gnome_color_picker_get_i8 (color, &r, &g, &b, &a);
	
	rgb   = r;
	rgb <<= 8;
	rgb  |= g;
	rgb <<= 8;
	rgb  |= b;
	
	return rgb;
}					    

static char *
get_refresh_minutes (SourceDialog *source_dialog)
{
	int setting = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (source_dialog->refresh_spin));

	if (source_dialog->refresh_optionmenu)		
		switch (gtk_option_menu_get_history (GTK_OPTION_MENU (source_dialog->refresh_optionmenu))){
		case 0: /* minutes */
			break;
		case 1: /* hours */
			setting *= 60;
			break;
		case 2: /* days */
			setting *= 1440;
			break;
		case 3: /* weeks wtf? why so long */
			setting *= 10080;
			break;
		default:
			g_warning ("Time unit out of range");
			break;
		}

	return g_strdup_printf ("%d", setting);
}

static void
set_refresh_time (SourceDialog *source_dialog) {
        int time;
	int item_num = 0;
	
	const char *refresh_str = e_source_get_property (source_dialog->source, "refresh");
	time = refresh_str ? atoi (refresh_str) : 30;

	if (source_dialog->refresh_optionmenu) {
		if (time && !(time % 10080)) {
			/* weeks */
			item_num = 3;
			time /= 10080;
		} else if (time && !(time % 1440)) {
			/* days */
			item_num = 2;
			time /= 1440;
		} else if (time && !(time % 60)) {
			/* days */
			item_num = 1;
			time /= 60;
		}

		gtk_option_menu_set_history (GTK_OPTION_MENU (source_dialog->refresh_optionmenu), item_num);
	}

	gtk_spin_button_set_value (GTK_SPIN_BUTTON (source_dialog->refresh_spin), time);
	return;
}

static void
source_to_dialog (SourceDialog *source_dialog)
{
	ESource *source = source_dialog->source;
	gboolean remote = FALSE;

	g_signal_handlers_block_matched (source_dialog->name_entry, G_SIGNAL_MATCH_DATA,
					 0, 0, NULL, NULL, source_dialog);

	/* These are calendar specific so make sure we have them */
	if (source_dialog->uri_entry)
		g_signal_handlers_block_matched (source_dialog->uri_entry, G_SIGNAL_MATCH_DATA,
						 0, 0, NULL, NULL, source_dialog);
	if (source_dialog->refresh_spin)
		g_signal_handlers_block_matched (source_dialog->refresh_spin, G_SIGNAL_MATCH_DATA,
						 0, 0, NULL, NULL, source_dialog);

	gtk_entry_set_text (GTK_ENTRY (source_dialog->name_entry), source ? e_source_peek_name (source) : "");
	
	if (source && source_is_remote (source)) {
		gchar       *uri_str;

		remote = TRUE;

		uri_str = e_source_get_uri (source);
		gtk_entry_set_text (GTK_ENTRY (source_dialog->uri_entry), uri_str);
		g_free (uri_str);

		set_refresh_time (source_dialog);
	} else {
		if (source_dialog->uri_entry)
			gtk_entry_set_text (GTK_ENTRY (source_dialog->uri_entry), "");
		if (source_dialog->refresh_spin)
			gtk_spin_button_set_value (GTK_SPIN_BUTTON (source_dialog->refresh_spin), 30);
	}

	general_update_dialog (source_dialog);

	g_signal_handlers_unblock_matched (source_dialog->name_entry, G_SIGNAL_MATCH_DATA,
					   0, 0, NULL, NULL, source_dialog);
	if (source_dialog->uri_entry)
		g_signal_handlers_unblock_matched (source_dialog->uri_entry, G_SIGNAL_MATCH_DATA,
						   0, 0, NULL, NULL, source_dialog);
	if (source_dialog->refresh_spin)
		g_signal_handlers_unblock_matched (source_dialog->refresh_spin, G_SIGNAL_MATCH_DATA,
						   0, 0, NULL, NULL, source_dialog);
	if (source_dialog->source_color) {
		guint32 color = 0xff00ff00;

		if (source_dialog->source)
			e_source_get_color (source_dialog->source, &color);
		else 
			/* FIXME */;

		colorpicker_set_color (GNOME_COLOR_PICKER (source_dialog->source_color), color);
	}
}

static void
dialog_to_source (SourceDialog *source_dialog)
{
	ESource *source = source_dialog->source;

	g_return_if_fail (source != NULL);

	e_source_set_name (source, gtk_entry_get_text (GTK_ENTRY (source_dialog->name_entry)));
	if (source_is_remote (source)) {
		EUri  *uri;
		gchar *relative_uri;
		gchar *refresh_str;

		/* Our relative_uri is everything but protocol, which is supplied by parent group */
		uri = e_uri_new (gtk_entry_get_text (GTK_ENTRY (source_dialog->uri_entry)));
		if (!uri) {
			g_warning ("Invalid remote URI!");
			return;
		}

		relative_uri = print_uri_noproto (uri);
		e_source_set_relative_uri (source, relative_uri);
		g_free (relative_uri);
		e_uri_free (uri);

		refresh_str = get_refresh_minutes (source_dialog);
		e_source_set_property (source, "refresh", refresh_str);
		g_free (refresh_str);
	}
	
	if (source_dialog->source_color)
		e_source_set_color (source, 
				    colorpicker_get_color (GNOME_COLOR_PICKER (source_dialog->source_color)));
}

static gboolean
source_dialog_is_valid (SourceDialog *source_dialog)
{
	if (!general_page_verify (source_dialog))
		return FALSE;

	if (((source_dialog->source && source_is_remote (source_dialog->source)) ||
	     (source_dialog->source_group && source_group_is_remote (source_dialog->source_group))) &&
	    !remote_page_verify (source_dialog))
		return FALSE;

	return TRUE;
}

static void
editor_set_buttons_sensitive (SourceDialog *source_dialog, gboolean sensitive)
{
	gtk_widget_set_sensitive (glade_xml_get_widget (source_dialog->gui_xml, "ok-button"), sensitive);
}

static void
general_page_modified (SourceDialog *source_dialog)
{
	if (source_dialog->druid) {
		gnome_druid_set_buttons_sensitive (GNOME_DRUID (source_dialog->druid),
						   TRUE,                                 /* Back */
						   general_page_verify (source_dialog),  /* Next */
						   TRUE,                                 /* Cancel */
						   FALSE);                               /* Help */
	} else {
		editor_set_buttons_sensitive (source_dialog, source_dialog_is_valid (source_dialog));
	}
}

static void
remote_page_modified (SourceDialog *source_dialog)
{
	if (source_dialog->druid) {
		gnome_druid_set_buttons_sensitive (GNOME_DRUID (source_dialog->druid),
						   TRUE,                                /* Back */
						   remote_page_verify (source_dialog),  /* Next */
						   TRUE,                                /* Cancel */
						   FALSE);                              /* Help */
	} else {
		editor_set_buttons_sensitive (source_dialog, source_dialog_is_valid (source_dialog));
	}
}

static void
source_group_changed_sensitive (SourceDialog *source_dialog)
{
	source_dialog->source_group =
		g_slist_nth (e_source_list_peek_groups (source_dialog->source_list),
			     gtk_option_menu_get_history (GTK_OPTION_MENU (source_dialog->group_optionmenu)))->data;

	general_update_dialog (source_dialog);
}

static void
general_test_uri (SourceDialog *source_dialog)
{
	/* FIXME this should do something more specific that just showing the uri */
	gnome_url_show (gtk_entry_get_text (GTK_ENTRY (source_dialog->uri_entry)),
			NULL);
}

static void
new_calendar_cancel (SourceDialog *source_dialog)
{
	gtk_widget_destroy (source_dialog->window);
}

static void
new_calendar_add (SourceDialog *source_dialog)
{
	source_dialog->source =
		create_new_source_with_group (GTK_WINDOW (source_dialog->window), source_dialog->source_group, 
					      gtk_entry_get_text (GTK_ENTRY (source_dialog->name_entry)),
					      gtk_entry_get_text (GTK_ENTRY (source_dialog->uri_entry)),
					      E_CAL_SOURCE_TYPE_EVENT);
	dialog_to_source (source_dialog);
	
	gtk_widget_destroy (source_dialog->window);
}

gboolean
calendar_setup_new_calendar (GtkWindow *parent)
{
	SourceDialog *source_dialog = g_new0 (SourceDialog, 1);
	int index;

	source_dialog->gui_xml = glade_xml_new (EVOLUTION_GLADEDIR "/" GLADE_FILE_NAME, "add-calendar-window", NULL);
	if (!source_dialog->gui_xml) {
		g_warning (G_STRLOC ": Cannot load Glade file.");
		g_free (source_dialog);
		return FALSE;
	}

	source_dialog->window = glade_xml_get_widget (source_dialog->gui_xml, "add-calendar-window");

	source_dialog->name_entry = glade_xml_get_widget (source_dialog->gui_xml, "name-entry");
	g_signal_connect_swapped (source_dialog->name_entry, "changed",
				  G_CALLBACK (general_entry_modified), source_dialog);      
	source_dialog->source_list = e_source_list_new_for_gconf_default ("/apps/evolution/calendar/sources");

	source_dialog->group_optionmenu =
		glade_xml_get_widget (source_dialog->gui_xml, "group-optionmenu");
	if (!GTK_IS_MENU (gtk_option_menu_get_menu (GTK_OPTION_MENU (source_dialog->group_optionmenu)))) {
		GtkWidget *menu = gtk_menu_new ();
		gtk_option_menu_set_menu (GTK_OPTION_MENU (source_dialog->group_optionmenu), menu);
		gtk_widget_show (menu);
	}

	/* NOTE: This assumes that we have sources. If they don't exist, they're set up
	 * on startup of the calendar component. */
	index = source_group_menu_add_groups (GTK_MENU_SHELL (gtk_option_menu_get_menu (
		GTK_OPTION_MENU (source_dialog->group_optionmenu))), source_dialog->source_list);
	gtk_option_menu_set_history (GTK_OPTION_MENU (source_dialog->group_optionmenu), index);
	source_dialog->source_group = e_source_list_peek_groups (source_dialog->source_list)->data;
	g_signal_connect_swapped (source_dialog->group_optionmenu, "changed",
				  G_CALLBACK (source_group_changed_sensitive), source_dialog);

	source_dialog->uri_entry = glade_xml_get_widget (source_dialog->gui_xml, "uri-entry");
	source_dialog->uri_label = glade_xml_get_widget (source_dialog->gui_xml, "uri-label");
	source_dialog->uri_hbox = glade_xml_get_widget (source_dialog->gui_xml, "uri-hbox");

	g_signal_connect_swapped (source_dialog->uri_entry, "changed",
				  G_CALLBACK (general_entry_modified), source_dialog);

	source_dialog->refresh_spin = glade_xml_get_widget (source_dialog->gui_xml, "refresh-spin");
	source_dialog->refresh_optionmenu = glade_xml_get_widget (source_dialog->gui_xml, "refresh-optionmenu");
	source_dialog->refresh_label = glade_xml_get_widget (source_dialog->gui_xml, "refresh-label");
	source_dialog->refresh_hbox = glade_xml_get_widget (source_dialog->gui_xml, "refresh-hbox");

	g_signal_connect_swapped (glade_xml_get_widget (source_dialog->gui_xml, "cancel-button"), "clicked",
				  G_CALLBACK (new_calendar_cancel), source_dialog);

	source_dialog->add_button = glade_xml_get_widget (source_dialog->gui_xml, "add-button");
	gtk_widget_set_sensitive (source_dialog->add_button, FALSE);
	g_signal_connect_swapped (source_dialog->add_button, "clicked",
				  G_CALLBACK (new_calendar_add), source_dialog);

	source_dialog->source_color = glade_xml_get_widget (source_dialog->gui_xml, "source-color");

	g_object_weak_ref (G_OBJECT (source_dialog->window),
			   (GWeakNotify) source_dialog_destroy, source_dialog);
	
	source_to_dialog (source_dialog);
	
	g_signal_connect_swapped (glade_xml_get_widget (source_dialog->gui_xml, "uri-button"), "clicked",
				  G_CALLBACK (general_test_uri), source_dialog);
	

	gtk_window_set_type_hint (GTK_WINDOW (source_dialog->window), GDK_WINDOW_TYPE_HINT_DIALOG);
	gtk_window_set_modal (GTK_WINDOW (source_dialog->window), TRUE);

	gtk_widget_show_all (source_dialog->window);
	return TRUE;
}

static void
edit_calendar_finish (SourceDialog *source_dialog)
{
	dialog_to_source (source_dialog);
	gtk_widget_destroy (source_dialog->window);
}

static void
edit_calendar_cancel (SourceDialog *source_dialog)
{
	gtk_widget_destroy (source_dialog->window);
}

gboolean
calendar_setup_edit_calendar (GtkWindow *parent, ESource *source)
{
	SourceDialog *source_dialog = g_new0 (SourceDialog, 1);

	g_return_val_if_fail (source != NULL, FALSE);

	source_dialog->gui_xml = glade_xml_new (EVOLUTION_GLADEDIR "/" GLADE_FILE_NAME, "calendar-editor-window", NULL);
	if (!source_dialog->gui_xml) {
		g_warning (G_STRLOC ": Cannot load Glade file.");
		g_free (source_dialog);
		return FALSE;
	}

	source_dialog->source = source;
	g_object_ref (source);

	source_dialog->window = glade_xml_get_widget (source_dialog->gui_xml, "calendar-editor-window");

	/* General page */
	source_dialog->name_entry = glade_xml_get_widget (source_dialog->gui_xml, "name-entry");
	g_signal_connect_swapped (source_dialog->name_entry, "changed",
				  G_CALLBACK (general_page_modified), source_dialog);
	g_signal_connect_swapped (source_dialog->name_entry, "activate",
				  G_CALLBACK (edit_calendar_finish), source_dialog);

	/* Remote page */
	source_dialog->uri_entry = glade_xml_get_widget (source_dialog->gui_xml, "uri-entry");
	source_dialog->refresh_spin = glade_xml_get_widget (source_dialog->gui_xml, "refresh-spin");
	g_signal_connect_swapped (source_dialog->uri_entry, "changed",
				  G_CALLBACK (remote_page_modified), source_dialog);
	g_signal_connect_swapped (source_dialog->refresh_spin, "changed",
				  G_CALLBACK (remote_page_modified), source_dialog);

	/* Finishing */
	g_signal_connect_swapped (glade_xml_get_widget (source_dialog->gui_xml, "ok-button"), "clicked",
				  G_CALLBACK (edit_calendar_finish), source_dialog);
	g_signal_connect_swapped (glade_xml_get_widget (source_dialog->gui_xml, "cancel-button"), "clicked",
				  G_CALLBACK (edit_calendar_cancel), source_dialog);
	g_object_weak_ref (G_OBJECT (source_dialog->window),
			   (GWeakNotify) source_dialog_destroy, source_dialog);

	/* Prepare and show dialog */
	source_to_dialog (source_dialog);

	gtk_window_set_type_hint (GTK_WINDOW (source_dialog->window), GDK_WINDOW_TYPE_HINT_DIALOG);
	gtk_window_set_modal (GTK_WINDOW (source_dialog->window), TRUE);

	gtk_widget_show_all (source_dialog->window);

	if (!source_is_remote (source_dialog->source))
		gtk_widget_hide (glade_xml_get_widget (source_dialog->gui_xml, "remote-page"));

	return TRUE;
}

static void
new_task_list_cancel (SourceDialog *source_dialog)
{
	gtk_widget_destroy (source_dialog->window);
}

static void
new_task_list_add (SourceDialog *source_dialog)
{
	source_dialog->source =
		create_new_source_with_group (GTK_WINDOW (source_dialog->window), source_dialog->source_group, 
					      gtk_entry_get_text (GTK_ENTRY (source_dialog->name_entry)),
					      gtk_entry_get_text (GTK_ENTRY (source_dialog->uri_entry)),
					      E_CAL_SOURCE_TYPE_TODO);
	dialog_to_source (source_dialog);
	
	gtk_widget_destroy (source_dialog->window);
}

gboolean
calendar_setup_new_task_list (GtkWindow *parent)
{
	SourceDialog *source_dialog = g_new0 (SourceDialog, 1);
	int index;

	source_dialog->gui_xml = glade_xml_new (EVOLUTION_GLADEDIR "/" GLADE_FILE_NAME, "add-task-list-window", NULL);
	if (!source_dialog->gui_xml) {
		g_warning (G_STRLOC ": Cannot load Glade file.");
		g_free (source_dialog);
		return FALSE;
	}

	source_dialog->window = glade_xml_get_widget (source_dialog->gui_xml, "add-task-list-window");

	source_dialog->name_entry = glade_xml_get_widget (source_dialog->gui_xml, "name-entry");
	g_signal_connect_swapped (source_dialog->name_entry, "changed",
				  G_CALLBACK (general_entry_modified), source_dialog);      
	source_dialog->source_list = e_source_list_new_for_gconf_default ("/apps/evolution/tasks/sources");

	source_dialog->group_optionmenu =
		glade_xml_get_widget (source_dialog->gui_xml, "group-optionmenu");
	if (!GTK_IS_MENU (gtk_option_menu_get_menu (GTK_OPTION_MENU (source_dialog->group_optionmenu)))) {
		GtkWidget *menu = gtk_menu_new ();
		gtk_option_menu_set_menu (GTK_OPTION_MENU (source_dialog->group_optionmenu), menu);
		gtk_widget_show (menu);
	}

	/* NOTE: This assumes that we have sources. If they don't exist, they're set up
	 * on startup of the calendar component. */
	index = source_group_menu_add_groups (GTK_MENU_SHELL (gtk_option_menu_get_menu (
		GTK_OPTION_MENU (source_dialog->group_optionmenu))), source_dialog->source_list);
	gtk_option_menu_set_history (GTK_OPTION_MENU (source_dialog->group_optionmenu), index);
	source_dialog->source_group = e_source_list_peek_groups (source_dialog->source_list)->data;
	g_signal_connect_swapped (source_dialog->group_optionmenu, "changed",
				  G_CALLBACK (source_group_changed_sensitive), source_dialog);

	source_dialog->uri_entry = glade_xml_get_widget (source_dialog->gui_xml, "uri-entry");
	source_dialog->uri_label = glade_xml_get_widget (source_dialog->gui_xml, "uri-label");
	source_dialog->uri_hbox = glade_xml_get_widget (source_dialog->gui_xml, "uri-hbox");

	g_signal_connect_swapped (source_dialog->uri_entry, "changed",
				  G_CALLBACK (general_entry_modified), source_dialog);

	source_dialog->refresh_spin = glade_xml_get_widget (source_dialog->gui_xml, "refresh-spin");
	source_dialog->refresh_optionmenu = glade_xml_get_widget (source_dialog->gui_xml, "refresh-optionmenu");
	source_dialog->refresh_label = glade_xml_get_widget (source_dialog->gui_xml, "refresh-label");
	source_dialog->refresh_hbox = glade_xml_get_widget (source_dialog->gui_xml, "refresh-hbox");

	g_signal_connect_swapped (glade_xml_get_widget (source_dialog->gui_xml, "cancel-button"), "clicked",
				  G_CALLBACK (new_task_list_cancel), source_dialog);

	source_dialog->add_button = glade_xml_get_widget (source_dialog->gui_xml, "add-button");
	gtk_widget_set_sensitive (source_dialog->add_button, FALSE);

	g_signal_connect_swapped (source_dialog->add_button, "clicked",
				  G_CALLBACK (new_task_list_add), source_dialog);
	g_object_weak_ref (G_OBJECT (source_dialog->window),
			   (GWeakNotify) source_dialog_destroy, source_dialog);
	
	source_dialog->source_color = glade_xml_get_widget (source_dialog->gui_xml, "source-color");

	g_signal_connect_swapped (glade_xml_get_widget (source_dialog->gui_xml, "uri-button"), "clicked",
				  G_CALLBACK (general_test_uri), source_dialog);

	source_to_dialog (source_dialog);

	gtk_window_set_type_hint (GTK_WINDOW (source_dialog->window), GDK_WINDOW_TYPE_HINT_DIALOG);
	gtk_window_set_modal (GTK_WINDOW (source_dialog->window), TRUE);

	gtk_widget_show_all (source_dialog->window);
	return TRUE;
}

static void
edit_task_list_finish (SourceDialog *source_dialog)
{
	dialog_to_source (source_dialog);
	gtk_widget_destroy (source_dialog->window);
}

static void
edit_task_list_cancel (SourceDialog *source_dialog)
{
	gtk_widget_destroy (source_dialog->window);
}

gboolean
calendar_setup_edit_task_list (GtkWindow *parent, ESource *source)
{
	SourceDialog *source_dialog = g_new0 (SourceDialog, 1);

	g_return_val_if_fail (source != NULL, FALSE);

	source_dialog->gui_xml = glade_xml_new (EVOLUTION_GLADEDIR "/" GLADE_FILE_NAME, "task-list-editor-window", NULL);
	if (!source_dialog->gui_xml) {
		g_warning (G_STRLOC ": Cannot load Glade file.");
		g_free (source_dialog);
		return FALSE;
	}

	source_dialog->source = source;
	g_object_ref (source);

	source_dialog->window = glade_xml_get_widget (source_dialog->gui_xml, "task-list-editor-window");

	/* General page */
	source_dialog->name_entry = glade_xml_get_widget (source_dialog->gui_xml, "name-entry");
	g_signal_connect_swapped (source_dialog->name_entry, "changed",
				  G_CALLBACK (general_page_modified), source_dialog);
	g_signal_connect_swapped (source_dialog->name_entry, "activate",
				  G_CALLBACK (edit_calendar_finish), source_dialog);

	/* Remote page */
	source_dialog->uri_entry = glade_xml_get_widget (source_dialog->gui_xml, "uri-entry");
	source_dialog->refresh_spin = glade_xml_get_widget (source_dialog->gui_xml, "refresh-spin");
	g_signal_connect_swapped (source_dialog->uri_entry, "changed",
				  G_CALLBACK (remote_page_modified), source_dialog);
	g_signal_connect_swapped (source_dialog->refresh_spin, "changed",
				  G_CALLBACK (remote_page_modified), source_dialog);

	/* Finishing */
	g_signal_connect_swapped (glade_xml_get_widget (source_dialog->gui_xml, "ok-button"), "clicked",
				  G_CALLBACK (edit_task_list_finish), source_dialog);
	g_signal_connect_swapped (glade_xml_get_widget (source_dialog->gui_xml, "cancel-button"), "clicked",
				  G_CALLBACK (edit_task_list_cancel), source_dialog);
	g_object_weak_ref (G_OBJECT (source_dialog->window),
			   (GWeakNotify) source_dialog_destroy, source_dialog);

	/* Prepare and show dialog */
	source_to_dialog (source_dialog);

	gtk_window_set_type_hint (GTK_WINDOW (source_dialog->window), GDK_WINDOW_TYPE_HINT_DIALOG);
	gtk_window_set_modal (GTK_WINDOW (source_dialog->window), TRUE);

	gtk_widget_show_all (source_dialog->window);

	if (!source_is_remote (source_dialog->source))
		gtk_widget_hide (glade_xml_get_widget (source_dialog->gui_xml, "remote-page"));

	return TRUE;
}
