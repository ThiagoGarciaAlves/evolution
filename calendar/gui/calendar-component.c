/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/* calendar-component.c
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Authors: Ettore Perazzoli <ettore@ximian.com>
 *          Rodrigo Moya <rodrigo@ximian.com>
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <errno.h>
#include <glib/gi18n.h>
#include <bonobo/bonobo-control.h>
#include <bonobo/bonobo-exception.h>
#include <libical/icalvcal.h>
#include <libedataserver/e-data-server-util.h>
#include <libedataserver/e-url.h>
#include <libecal/e-cal-time-util.h>
#include <libedataserverui/e-source-selector.h>
#include <shell/e-user-creatable-items-handler.h>
#include <shell/e-component-view.h>
#include "e-calendar-view.h"
#include "calendar-config-keys.h"
#include "calendar-config.h"
#include "calendar-component.h"
#include "calendar-commands.h"
#include "control-factory.h"
#include "gnome-cal.h"
#include "migration.h"
#include "e-comp-editor-registry.h"
#include "comp-util.h"
#include "common/authentication.h"
#include "dialogs/calendar-setup.h"
#include "dialogs/comp-editor.h"
#include "dialogs/copy-source-dialog.h"
#include "dialogs/event-editor.h"
#include "misc/e-info-label.h"
#include "e-util/e-error.h"
#include "e-cal-menu.h"
#include "e-cal-popup.h"

/* IDs for user creatable items */
#define CREATE_EVENT_ID        "event"
#define CREATE_MEETING_ID      "meeting"
#define CREATE_ALLDAY_EVENT_ID "allday-event"
#define CREATE_CALENDAR_ID      "calendar"
#define WEB_BASE_URI "webcal://"
#define CONTACTS_BASE_URI "contacts://"
#define WEATHER_BASE_URI "weather://"
#define PERSONAL_RELATIVE_URI "system"

enum DndTargetType {
	DND_TARGET_TYPE_CALENDAR_LIST,
};
#define CALENDAR_TYPE "text/calendar"
#define XCALENDAR_TYPE "text/x-calendar"
static GtkTargetEntry drag_types[] = {
	{ CALENDAR_TYPE, 0, DND_TARGET_TYPE_CALENDAR_LIST },
	{ XCALENDAR_TYPE, 0, DND_TARGET_TYPE_CALENDAR_LIST }
};
static gint num_drag_types = sizeof(drag_types) / sizeof(drag_types[0]);

#define PARENT_TYPE bonobo_object_get_type ()
static BonoboObjectClass *parent_class = NULL;

typedef struct
{
	ESourceList *source_list;
	ESourceList *task_source_list;
	ESourceList *memo_source_list;

	GSList *source_selection;
	GSList *task_source_selection;
	GSList *memo_source_selection;

	GnomeCalendar *calendar;

	EInfoLabel *info_label;
	GtkWidget *source_selector;

	BonoboControl *view_control;
	BonoboControl *sidebar_control;
	BonoboControl *statusbar_control;

	GList *notifications;

	EUserCreatableItemsHandler *creatable_items_handler;

	EActivityHandler *activity_handler;

	float	     vpane_pos;
} CalendarComponentView;

struct _CalendarComponentPrivate {

	GConfClient *gconf_client;
	int gconf_notify_id;

	ESourceList *source_list;
	ESourceList *task_source_list;
	ESourceList *memo_source_list;

	GList *views;

	ECal *create_ecal;

	GList *notifications;
};

/* FIXME This should be gnome cal likely */
extern ECompEditorRegistry *comp_editor_registry;

static void
calcomp_vpane_realized (GtkWidget *vpane, CalendarComponentView *view)
{
	gtk_paned_set_position (GTK_PANED (vpane), (int)(view->vpane_pos*vpane->allocation.height));

}

static gboolean
calcomp_vpane_resized (GtkWidget *vpane, GdkEventButton *e, CalendarComponentView *view)
{

	view->vpane_pos = gtk_paned_get_position (GTK_PANED (vpane));
	calendar_config_set_tag_vpane_pos (view->vpane_pos/(float)vpane->allocation.height);

	return FALSE;
}

static void
ensure_sources (CalendarComponent *component)
{
	GSList *groups;
	ESourceList *source_list;
	ESourceGroup *group;
	ESourceGroup *on_this_computer;
	ESourceGroup *on_the_web;
	ESourceGroup *contacts;
	ESourceGroup *weather;
	ESource *personal_source;
	ESource *birthdays_source;
	char *base_uri, *base_uri_proto;
	const gchar *base_dir;
	gchar *create_source;

	on_this_computer = NULL;
	on_the_web = NULL;
	contacts = NULL;
	weather = NULL;
	personal_source = NULL;
	birthdays_source = NULL;

	if (!e_cal_get_sources (&source_list, E_CAL_SOURCE_TYPE_EVENT, NULL)) {
		g_warning ("Could not get calendar source list from GConf!");
		return;
	}

	base_dir = calendar_component_peek_base_directory (component);
	base_uri = g_build_filename (base_dir, "local", NULL);

	base_uri_proto = g_filename_to_uri (base_uri, NULL, NULL);

	groups = e_source_list_peek_groups (source_list);
	if (groups) {
		/* groups are already there, we need to search for things... */
		GSList *g;

		for (g = groups; g; g = g->next) {

			group = E_SOURCE_GROUP (g->data);

			/* compare only file:// part. If user home dir name changes we do not want to create
			   one more group  */

			if (!on_this_computer && !strncmp (base_uri_proto, e_source_group_peek_base_uri (group), 7))
				on_this_computer = group;
			else if (!on_the_web && !strcmp (WEB_BASE_URI, e_source_group_peek_base_uri (group)))
				on_the_web = group;
			else if (!contacts && !strcmp (CONTACTS_BASE_URI, e_source_group_peek_base_uri (group)))
				contacts = group;
			else if (!weather && !strcmp (WEATHER_BASE_URI, e_source_group_peek_base_uri (group)))
				weather = group;
		}
	}

	if (on_this_computer) {
		/* make sure "Personal" shows up as a source under
		   this group */
		GSList *sources = e_source_group_peek_sources (on_this_computer);
		GSList *s;
		for (s = sources; s; s = s->next) {
			ESource *source = E_SOURCE (s->data);
			const gchar *relative_uri;

			relative_uri = e_source_peek_relative_uri (source);
			if (relative_uri == NULL)
				continue;
			if (!strcmp (PERSONAL_RELATIVE_URI, relative_uri)) {
				personal_source = source;
				break;
			}
		}
		/* Make sure we have the correct base uri. This can change when user's
		   homedir name changes */
		if (strcmp (base_uri_proto, e_source_group_peek_base_uri (on_this_computer))) {
		    e_source_group_set_base_uri (on_this_computer, base_uri_proto);

		    /* *sigh* . We shouldn't  need this sync call here as set_base_uri
		       call results in synching to gconf, but that happens in idle loop
		       and too late to prevent user seeing "Can not Open ... because of invalid uri" error.*/
		    e_source_list_sync (source_list,NULL);
		}
	}
	else {
		/* create the local source group */
		group = e_source_group_new (_("On This Computer"), base_uri_proto);
		e_source_list_add_group (source_list, group, -1);

		on_this_computer = group;
	}

	if (!personal_source) {
		char *primary_calendar = calendar_config_get_primary_calendar();
		GSList *calendars_selected;

		/* Create the default Person addressbook */
		personal_source = e_source_new (_("Personal"), PERSONAL_RELATIVE_URI);
		e_source_group_add_source (on_this_computer, personal_source, -1);
		g_object_unref (personal_source);

		calendars_selected = calendar_config_get_calendars_selected ();
		if (!primary_calendar && !calendars_selected) {
			GSList selected;

			calendar_config_set_primary_calendar (e_source_peek_uid (personal_source));

			selected.data = (gpointer)e_source_peek_uid (personal_source);
			selected.next = NULL;
			calendar_config_set_calendars_selected (&selected);
		}

		if (calendars_selected) {
			g_slist_foreach (calendars_selected, (GFunc) g_free, NULL);
			g_slist_free (calendars_selected);
		}

		g_free (primary_calendar);
		e_source_set_color_spec (personal_source, "#BECEDD");
	}

	if (!on_the_web) {
		/* Create the On the web source group */
		group = e_source_group_new (_("On The Web"), WEB_BASE_URI);
		e_source_list_add_group (source_list, group, -1);

		on_the_web = group;
	}
	if (contacts) {
		GSList *sources = e_source_group_peek_sources (contacts);
		if (sources) {
			birthdays_source = E_SOURCE (sources->data); /* There is only one source under Contacts Group*/

			if (sources->next) {
				/* Ensure we have only one contacts source - we was able to create more than one before */
				GSList *l = NULL, *p;

				for (p = sources->next; p; p = p->next)
					l = g_slist_prepend (l, p->data);

				for (p = l; p; p = p->next)
					e_source_group_remove_source (contacts, p->data);

				g_slist_free (l);
			}
		}
	}
	else  {
		/* Create the contacts group */
		group = e_source_group_new (_("Contacts"), CONTACTS_BASE_URI);
		e_source_list_add_group (source_list, group, -1);
		contacts = group;
	}

	create_source = e_source_group_get_property (contacts, "create_source");
	if (!create_source)
		e_source_group_set_property (contacts, "create_source", "no");
	g_free (create_source);

	if (!birthdays_source) {
		birthdays_source = e_source_new (_("Birthdays & Anniversaries"), "/");
		e_source_group_add_source (contacts, birthdays_source, -1);
		g_object_unref (birthdays_source);
	}

	if (!e_source_get_property (birthdays_source, "delete"))
		e_source_set_property(birthdays_source, "delete", "no");

	if (e_source_peek_color_spec (birthdays_source) == NULL)
		e_source_set_color_spec (birthdays_source, "#DDBECE");

	if (!weather) {
		/* Create the weather group */
		group = e_source_group_new (_("Weather"), WEATHER_BASE_URI);
		e_source_list_add_group (source_list, group, -1);
		weather = group;
	}

	component->priv->source_list = source_list;

	g_free (base_uri_proto);
	g_free (base_uri);
}


/* Utility functions.  */

static gboolean
is_in_selection (GSList *selection, ESource *source)
{
	GSList *l;

	for (l = selection; l; l = l->next) {
		ESource *selected_source = l->data;

		if (!strcmp (e_source_peek_uid (selected_source), e_source_peek_uid (source)))
			return TRUE;
	}

	return FALSE;
}

static gboolean
is_in_uids (GSList *uids, ESource *source)
{
	GSList *l;

	for (l = uids; l; l = l->next) {
		const char *uid = l->data;

		if (!strcmp (uid, e_source_peek_uid (source)))
			return TRUE;
	}

	return FALSE;
}

static void
update_uris_for_selection (CalendarComponentView *component_view)
{
	GSList *selection, *l, *uids_selected = NULL;

	selection = e_source_selector_get_selection (E_SOURCE_SELECTOR (component_view->source_selector));

	for (l = component_view->source_selection; l; l = l->next) {
		ESource *old_selected_source = l->data;

		if (!is_in_selection (selection, old_selected_source))
			gnome_calendar_remove_source (component_view->calendar, E_CAL_SOURCE_TYPE_EVENT, old_selected_source);
	}

	for (l = selection; l; l = l->next) {
		ESource *selected_source = l->data;

		if (gnome_calendar_add_source (component_view->calendar, E_CAL_SOURCE_TYPE_EVENT, selected_source))
			uids_selected = g_slist_append (uids_selected, (char *) e_source_peek_uid (selected_source));
	}

	e_source_selector_free_selection (component_view->source_selection);
	component_view->source_selection = selection;

	/* Save the selection for next time we start up */
	calendar_config_set_calendars_selected (uids_selected);
	g_slist_free (uids_selected);
}

static void
update_uri_for_primary_selection (CalendarComponentView *component_view)
{
	ESource *source;

	source = e_source_selector_peek_primary_selection (E_SOURCE_SELECTOR (component_view->source_selector));
	if (!source)
		return;

	/* Set the default */
	gnome_calendar_set_default_source (component_view->calendar, E_CAL_SOURCE_TYPE_EVENT, source);

	/* Make sure we are embedded first */
	calendar_control_sensitize_calendar_commands (component_view->view_control, component_view->calendar, TRUE);

	/* Save the selection for next time we start up */
	calendar_config_set_primary_calendar (e_source_peek_uid (source));
}

static void
update_selection (CalendarComponentView *component_view)
{
	GSList *selection, *uids_selected, *l;

	/* Get the selection in gconf */
	uids_selected = calendar_config_get_calendars_selected ();

	/* Remove any that aren't there any more */
	selection = e_source_selector_get_selection (E_SOURCE_SELECTOR (component_view->source_selector));

	for (l = selection; l; l = l->next) {
		ESource *source = l->data;

		if (!is_in_uids (uids_selected, source))
			e_source_selector_unselect_source (E_SOURCE_SELECTOR (component_view->source_selector), source);
	}

	e_source_selector_free_selection (selection);

	/* Make sure the whole selection is there */
	for (l = uids_selected; l; l = l->next) {
		char *uid = l->data;
		ESource *source;

		source = e_source_list_peek_source_by_uid (component_view->source_list, uid);
		if (source)
			e_source_selector_select_source (E_SOURCE_SELECTOR (component_view->source_selector), source);

		g_free (uid);
	}
	g_slist_free (uids_selected);
}

static void
update_task_memo_selection (CalendarComponentView *component_view, ECalSourceType type)
{
	GSList *uids_selected, *l, *source_selection;
	ESourceList *source_list = NULL;

	if (type == E_CAL_SOURCE_TYPE_TODO) {
		/* Get the selection in gconf */
		uids_selected = calendar_config_get_tasks_selected ();
		source_list = component_view->task_source_list;
		source_selection = component_view->task_source_selection;
	} else {
		uids_selected = calendar_config_get_memos_selected ();
		source_list = component_view->memo_source_list;
		source_selection = component_view->memo_source_selection;
	}

	/* Remove any that aren't there any more */
	for (l = source_selection; l; l = l->next) {
		char *uid = l->data;
		ESource *source;

		source = e_source_list_peek_source_by_uid (component_view->source_list, uid);
		if (!source)
			gnome_calendar_remove_source_by_uid (component_view->calendar, type, uid);
		else if (!is_in_uids (uids_selected, source))
			gnome_calendar_remove_source (component_view->calendar, type, source);

		g_free (uid);
	}
	g_slist_free (source_selection);

	/* Make sure the whole selection is there */
	for (l = uids_selected; l; l = l->next) {
		char *uid = l->data;
		ESource *source;

		source = e_source_list_peek_source_by_uid (source_list, uid);
		if (source && !gnome_calendar_add_source (component_view->calendar, type, source))
			/* FIXME do something */;
	}

	if (type == E_CAL_SOURCE_TYPE_TODO)
		component_view->task_source_selection = uids_selected;
	else
		component_view->memo_source_selection = uids_selected;
}

static void
update_primary_selection (CalendarComponentView *component_view)
{
	ESource *source = NULL;
	char *uid;

	uid = calendar_config_get_primary_calendar ();
	if (uid) {
		source = e_source_list_peek_source_by_uid (component_view->source_list, uid);
		g_free (uid);
	}

	if (source) {
		e_source_selector_set_primary_selection (E_SOURCE_SELECTOR (component_view->source_selector), source);
	} else {
		/* Try to create a default if there isn't one */
		source = e_source_list_peek_source_any (component_view->source_list);
		if (source)
			e_source_selector_set_primary_selection (E_SOURCE_SELECTOR (component_view->source_selector), source);
	}
}

static void
update_primary_task_memo_selection (CalendarComponentView *component_view, ECalSourceType type)
{
	ESource *source = NULL;
	char *uid;
	ESourceList *source_list = NULL;

	if (type == E_CAL_SOURCE_TYPE_TODO) {
		uid = calendar_config_get_primary_tasks ();
		source_list = component_view->task_source_list;
	} else {
		uid = calendar_config_get_primary_memos ();
		source_list = component_view->memo_source_list;
	}

	if (uid) {
		source = e_source_list_peek_source_by_uid (source_list, uid);

		g_free (uid);
	}

	if (source)
		gnome_calendar_set_default_source (component_view->calendar, type, source);
}

/* Callbacks.  */
static void
copy_calendar_cb (EPopup *ep, EPopupItem *pitem, void *data)
{
	CalendarComponentView *component_view = data;
	ESource *selected_source;

	selected_source = e_source_selector_peek_primary_selection (E_SOURCE_SELECTOR (component_view->source_selector));
	if (!selected_source)
		return;

	copy_source_dialog (GTK_WINDOW (gtk_widget_get_toplevel (ep->target->widget)), selected_source, E_CAL_SOURCE_TYPE_EVENT);
}

static void
delete_calendar_cb (EPopup *ep, EPopupItem *pitem, void *data)
{
	CalendarComponentView *component_view = data;
	ESource *selected_source;
	ECal *cal;
	char *uri;

	selected_source = e_source_selector_peek_primary_selection (E_SOURCE_SELECTOR (component_view->source_selector));
	if (!selected_source)
		return;

	if (e_error_run((GtkWindow *)gtk_widget_get_toplevel(ep->target->widget),
			"calendar:prompt-delete-calendar", e_source_peek_name(selected_source)) != GTK_RESPONSE_YES)
		return;

	/* first, ask the backend to remove the calendar */
	uri = e_source_get_uri (selected_source);
	cal = e_cal_model_get_client_for_uri (gnome_calendar_get_calendar_model (component_view->calendar), uri);
	if (!cal)
		cal = e_cal_new_from_uri (uri, E_CAL_SOURCE_TYPE_EVENT);
	g_free (uri);
	if (cal) {
		if (e_cal_remove (cal, NULL)) {
			if (e_source_selector_source_is_selected (E_SOURCE_SELECTOR (component_view->source_selector),
								  selected_source)) {
				gnome_calendar_remove_source (component_view->calendar, E_CAL_SOURCE_TYPE_EVENT, selected_source);
				e_source_selector_unselect_source (E_SOURCE_SELECTOR (component_view->source_selector),
								   selected_source);
			}

			e_source_group_remove_source (e_source_peek_group (selected_source), selected_source);
			e_source_list_sync (component_view->source_list, NULL);
		}
	}
}

static void
new_calendar_cb (EPopup *ep, EPopupItem *pitem, void *data)
{
	calendar_setup_edit_calendar (GTK_WINDOW (gtk_widget_get_toplevel(ep->target->widget)), NULL, pitem->user_data);
}

static void
edit_calendar_cb (EPopup *ep, EPopupItem *pitem, void *data)
{
	CalendarComponentView *component_view = data;
	ESource *selected_source;

	selected_source = e_source_selector_peek_primary_selection (E_SOURCE_SELECTOR (component_view->source_selector));
	if (!selected_source)
		return;

	calendar_setup_edit_calendar (GTK_WINDOW (gtk_widget_get_toplevel(ep->target->widget)), selected_source, NULL);
}

static EPopupItem ecc_source_popups[] = {
	{ E_POPUP_ITEM, "10.new", N_("_New Calendar"), new_calendar_cb, NULL, "x-office-calendar", 0, 0 },
	{ E_POPUP_ITEM, "15.copy", N_("_Copy..."), copy_calendar_cb, NULL, "edit-copy", 0, E_CAL_POPUP_SOURCE_PRIMARY },

	{ E_POPUP_BAR, "20.bar" },
	{ E_POPUP_ITEM, "20.delete", N_("_Delete"), delete_calendar_cb, NULL, "edit-delete", 0,E_CAL_POPUP_SOURCE_USER|E_CAL_POPUP_SOURCE_PRIMARY|E_CAL_POPUP_SOURCE_DELETE },

	{ E_POPUP_BAR, "99.bar" },
	{ E_POPUP_ITEM, "99.properties", N_("_Properties"), edit_calendar_cb, NULL, "document-properties", 0, E_CAL_POPUP_SOURCE_PRIMARY },
};

static void
ecc_source_popup_free(EPopup *ep, GSList *list, void *data)
{
	g_slist_free(list);
}

static gboolean
popup_event_cb(ESourceSelector *selector, ESource *insource, GdkEventButton *event, CalendarComponentView *component_view)
{
	ECalPopup *ep;
	ECalPopupTargetSource *t;
	GSList *menus = NULL;
	int i;
	GtkMenu *menu;

	/** @HookPoint-ECalPopup: Calendar Source Selector Context Menu
	 * @Id: org.gnome.evolution.calendar.source.popup
	 * @Class: org.gnome.evolution.calendar.popup:1.0
	 * @Target: ECalPopupTargetSource
	 *
	 * The context menu on the source selector in the calendar window.
	 */
	ep = e_cal_popup_new("org.gnome.evolution.calendar.source.popup");
	t = e_cal_popup_target_new_source(ep, selector);
	t->target.widget = (GtkWidget *)component_view->calendar;

	for (i=0;i<sizeof(ecc_source_popups)/sizeof(ecc_source_popups[0]);i++)
		menus = g_slist_prepend(menus, &ecc_source_popups[i]);

	e_popup_add_items((EPopup *)ep, menus, NULL, ecc_source_popup_free, component_view);

	menu = e_popup_create_menu_once((EPopup *)ep, (EPopupTarget *)t, 0);
	gtk_menu_popup(menu, NULL, NULL, NULL, NULL, event?event->button:0, event?event->time:gtk_get_current_event_time());

	return TRUE;
}

static void
source_selection_changed_cb (ESourceSelector *selector, CalendarComponentView *component_view)
{
	update_uris_for_selection (component_view);
}

static void
primary_source_selection_changed_cb (ESourceSelector *selector, CalendarComponentView *component_view)
{
	update_uri_for_primary_selection (component_view);
}

static void
source_changed_cb (ESource *source, GnomeCalendar *calendar)
{
	if (calendar) {
		GtkWidget *widget = gnome_calendar_get_current_view_widget (calendar);

		if (widget)
			gtk_widget_queue_draw (widget);
	}
}

static void
source_added_cb (GnomeCalendar *calendar, ECalSourceType source_type, ESource *source, CalendarComponentView *component_view)
{
	switch (source_type) {
	case E_CAL_SOURCE_TYPE_EVENT:
		e_source_selector_select_source (E_SOURCE_SELECTOR (component_view->source_selector), source);
		g_signal_connect (source, "changed", G_CALLBACK (source_changed_cb), calendar);
		break;
	default:
		break;
	}
}

static void
source_removed_cb (GnomeCalendar *calendar, ECalSourceType source_type, ESource *source, CalendarComponentView *component_view)
{
	switch (source_type) {
	case E_CAL_SOURCE_TYPE_EVENT:
		g_signal_handlers_disconnect_by_func (source, G_CALLBACK (source_changed_cb), calendar);
		e_source_selector_unselect_source (E_SOURCE_SELECTOR (component_view->source_selector), source);
		break;
	default:
		break;
	}
}

static void
set_info (CalendarComponentView *component_view)
{
	icaltimezone *zone;
	struct icaltimetype start_tt, end_tt;
	time_t start_time, end_time;
	struct tm start_tm, end_tm;
	char buffer[512], end_buffer[256];
	GnomeCalendarViewType view;

	gnome_calendar_get_visible_time_range (component_view->calendar, &start_time, &end_time);
	zone = gnome_calendar_get_timezone (component_view->calendar);

	start_tt = icaltime_from_timet_with_zone (start_time, FALSE, zone);
	start_tm.tm_year = start_tt.year - 1900;
	start_tm.tm_mon = start_tt.month - 1;
	start_tm.tm_mday = start_tt.day;
	start_tm.tm_hour = start_tt.hour;
	start_tm.tm_min = start_tt.minute;
	start_tm.tm_sec = start_tt.second;
	start_tm.tm_isdst = -1;
	start_tm.tm_wday = time_day_of_week (start_tt.day, start_tt.month - 1,
					     start_tt.year);

	/* Take one off end_time so we don't get an extra day. */
	end_tt = icaltime_from_timet_with_zone (end_time - 1, FALSE, zone);
	end_tm.tm_year = end_tt.year - 1900;
	end_tm.tm_mon = end_tt.month - 1;
	end_tm.tm_mday = end_tt.day;
	end_tm.tm_hour = end_tt.hour;
	end_tm.tm_min = end_tt.minute;
	end_tm.tm_sec = end_tt.second;
	end_tm.tm_isdst = -1;
	end_tm.tm_wday = time_day_of_week (end_tt.day, end_tt.month - 1,
					   end_tt.year);

	view = gnome_calendar_get_view (component_view->calendar);

	switch (view) {
	case GNOME_CAL_DAY_VIEW:
	case GNOME_CAL_WORK_WEEK_VIEW:
	case GNOME_CAL_WEEK_VIEW:
		if (start_tm.tm_year == end_tm.tm_year
		    && start_tm.tm_mon == end_tm.tm_mon
		    && start_tm.tm_mday == end_tm.tm_mday) {
			e_utf8_strftime (buffer, sizeof (buffer),
				  _("%A %d %b %Y"), &start_tm);
		} else if (start_tm.tm_year == end_tm.tm_year) {
			e_utf8_strftime (buffer, sizeof (buffer),
				  _("%a %d %b"), &start_tm);
			e_utf8_strftime (end_buffer, sizeof (end_buffer),
				  _("%a %d %b %Y"), &end_tm);
			strcat (buffer, " - ");
			strcat (buffer, end_buffer);
		} else {
			e_utf8_strftime (buffer, sizeof (buffer),
				  _("%a %d %b %Y"), &start_tm);
			e_utf8_strftime (end_buffer, sizeof (end_buffer),
				  _("%a %d %b %Y"), &end_tm);
			strcat (buffer, " - ");
			strcat (buffer, end_buffer);
		}
		break;
	case GNOME_CAL_MONTH_VIEW:
	case GNOME_CAL_LIST_VIEW:
		if (start_tm.tm_year == end_tm.tm_year) {
			if (start_tm.tm_mon == end_tm.tm_mon) {
				e_utf8_strftime (buffer, sizeof (buffer),
					  "%d", &start_tm);
				e_utf8_strftime (end_buffer, sizeof (end_buffer),
					  _("%d %b %Y"), &end_tm);
				strcat (buffer, " - ");
				strcat (buffer, end_buffer);
			} else {
				e_utf8_strftime (buffer, sizeof (buffer),
					  _("%d %b"), &start_tm);
				e_utf8_strftime (end_buffer, sizeof (end_buffer),
					  _("%d %b %Y"), &end_tm);
				strcat (buffer, " - ");
				strcat (buffer, end_buffer);
			}
		} else {
			e_utf8_strftime (buffer, sizeof (buffer),
				  _("%d %b %Y"), &start_tm);
			e_utf8_strftime (end_buffer, sizeof (end_buffer),
				  _("%d %b %Y"), &end_tm);
			strcat (buffer, " - ");
			strcat (buffer, end_buffer);
		}
		break;
	default:
		g_return_if_reached ();
	}

	e_info_label_set_info (component_view->info_label, _("Calendars"), buffer);
}

static void
calendar_dates_changed_cb (GnomeCalendar *calendar, CalendarComponentView *component_view)
{
	set_info (component_view);
}

static void
config_primary_selection_changed_cb (GConfClient *client, guint id, GConfEntry *entry, gpointer data)
{
	CalendarComponent *calendar_component = data;
	CalendarComponentPrivate *priv = calendar_component->priv;

	if (priv->create_ecal) {
		g_object_unref (priv->create_ecal);
		priv->create_ecal = NULL;
	}
}

static void
config_tasks_selection_changed_cb (GConfClient *client, guint id, GConfEntry *entry, gpointer data)
{
	update_task_memo_selection (data, E_CAL_SOURCE_TYPE_TODO);
}


static void
config_primary_tasks_selection_changed_cb (GConfClient *client, guint id, GConfEntry *entry, gpointer data)
{
	update_primary_task_memo_selection (data, E_CAL_SOURCE_TYPE_TODO);
}

static void
config_memos_selection_changed_cb (GConfClient *client, guint id, GConfEntry *entry, gpointer data)
{
	update_task_memo_selection (data, E_CAL_SOURCE_TYPE_JOURNAL);
}


static void
config_primary_memos_selection_changed_cb (GConfClient *client, guint id, GConfEntry *entry, gpointer data)
{
	update_primary_task_memo_selection (data, E_CAL_SOURCE_TYPE_JOURNAL);
}

/* Evolution::Component CORBA methods.  */
static void
impl_handleURI (PortableServer_Servant servant, const char *uri, CORBA_Environment *ev)
{
	CalendarComponent *calendar_component = CALENDAR_COMPONENT (bonobo_object_from_servant (servant));
	CalendarComponentPrivate *priv;
	GList *l;
	CalendarComponentView *view = NULL;
	char *src_uid = NULL;
	char *uid = NULL;
	char *rid = NULL;

	priv = calendar_component->priv;

	l = g_list_last (priv->views);
	if (!l)
		return;

	view = l->data;

	if (!strncmp (uri, "calendar:", 9)) {
		EUri *euri = e_uri_new (uri);
		const char *p;
		char *header, *content;
		size_t len, clen;
		time_t start = -1, end = -1;

		p = euri->query;
		if (p) {
			while (*p) {
				len = strcspn (p, "=&");

				/* If it's malformed, give up. */
				if (p[len] != '=')
					break;

				header = (char *) p;
				header[len] = '\0';
				p += len + 1;

				clen = strcspn (p, "&");

				content = g_strndup (p, clen);

				if (!g_ascii_strcasecmp (header, "startdate")) {
					start = time_from_isodate (content);
				} else if (!g_ascii_strcasecmp (header, "enddate")) {
					end = time_from_isodate (content);
				} else if (!g_ascii_strcasecmp (header, "source-uid")) {
					src_uid = g_strdup (content);
				} else if (!g_ascii_strcasecmp (header, "comp-uid")) {
					uid = g_strdup (content);
				} else if (!g_ascii_strcasecmp (header, "comp-rid")) {
					rid = g_strdup (content);
				}

				g_free (content);

				p += clen;
				if (*p == '&') {
					p++;
					if (!strcmp (p, "amp;"))
						p += 4;
				}
			}

			if (start != -1) {

				if (end == -1)
					end = start;
					gnome_calendar_set_selected_time_range (view->calendar, start, end);
			}
			if (src_uid && uid)
				gnome_calendar_edit_appointment (view->calendar, src_uid, uid, rid);

			g_free (src_uid);
			g_free (uid);
			g_free (rid);
		}
		e_uri_free (euri);
	}
}

static void
impl_upgradeFromVersion (PortableServer_Servant servant,
			 CORBA_short major,
			 CORBA_short minor,
			 CORBA_short revision,
			 CORBA_Environment *ev)
{
	GError *err = NULL;
	CalendarComponent *calendar_component = CALENDAR_COMPONENT (bonobo_object_from_servant (servant));

	if (!migrate_calendars (calendar_component, major, minor, revision, &err)) {
		GNOME_Evolution_Component_UpgradeFailed *failedex;

		failedex = GNOME_Evolution_Component_UpgradeFailed__alloc();
		failedex->what = CORBA_string_dup(_("Failed upgrading calendars."));
		failedex->why = CORBA_string_dup(err->message);
		CORBA_exception_set(ev, CORBA_USER_EXCEPTION, ex_GNOME_Evolution_Component_UpgradeFailed, failedex);
	}

	if (err)
		g_error_free(err);
}

static gboolean
selector_tree_drag_drop (GtkWidget *widget,
			 GdkDragContext *context,
			 int x,
			 int y,
			 guint time,
			 CalendarComponent *component)
{
	GtkTreeViewColumn *column;
	int cell_x;
	int cell_y;
	GtkTreePath *path;
	GtkTreeModel *model;
	GtkTreeIter iter;
	gpointer data;

	if (!gtk_tree_view_get_path_at_pos  (GTK_TREE_VIEW (widget), x, y, &path,
					     &column, &cell_x, &cell_y))
		return FALSE;


	model = gtk_tree_view_get_model (GTK_TREE_VIEW (widget));

	if (!gtk_tree_model_get_iter (model, &iter, path)) {
		gtk_tree_path_free (path);
		return FALSE;
	}

	gtk_tree_model_get (model, &iter, 0, &data, -1);

	if (E_IS_SOURCE_GROUP (data)) {
		g_object_unref (data);
		gtk_tree_path_free (path);
		return FALSE;
	}

	gtk_tree_path_free (path);
	return TRUE;
}

static gboolean
selector_tree_drag_motion (GtkWidget *widget,
			   GdkDragContext *context,
			   int x,
			   int y,
			   guint time,
			   gpointer user_data)
{
	GtkTreePath *path = NULL;
	gpointer data = NULL;
	GtkTreeViewDropPosition pos;
	GtkTreeModel *model;
	GtkTreeIter iter;
	GdkDragAction action = GDK_ACTION_DEFAULT;

	if (!gtk_tree_view_get_dest_row_at_pos (GTK_TREE_VIEW (widget),
						x, y, &path, &pos))
		goto finish;

	model = gtk_tree_view_get_model (GTK_TREE_VIEW (widget));

	if (!gtk_tree_model_get_iter (model, &iter, path))
		goto finish;

	gtk_tree_model_get (model, &iter, 0, &data, -1);

	if (E_IS_SOURCE_GROUP (data) || e_source_get_readonly (data))
		goto finish;

	gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW (widget), path, GTK_TREE_VIEW_DROP_INTO_OR_BEFORE);
	action = context->suggested_action;

 finish:
	if (path)
		gtk_tree_path_free (path);
	if (data)
		g_object_unref (data);

	gdk_drag_status (context, action, time);
	return TRUE;
}

static gboolean
update_single_object (ECal *client, icalcomponent *icalcomp)
{
	char *uid;
	icalcomponent *tmp_icalcomp;

	uid = (char *) icalcomponent_get_uid (icalcomp);

	if (e_cal_get_object (client, uid, NULL, &tmp_icalcomp, NULL))
		return e_cal_modify_object (client, icalcomp, CALOBJ_MOD_ALL, NULL);

	return e_cal_create_object (client, icalcomp, &uid, NULL);
}

static gboolean
update_objects (ECal *client, icalcomponent *icalcomp)
{
	icalcomponent *subcomp;
	icalcomponent_kind kind;

	kind = icalcomponent_isa (icalcomp);
	if (kind == ICAL_VTODO_COMPONENT || kind == ICAL_VEVENT_COMPONENT)
		return update_single_object (client, icalcomp);
	else if (kind != ICAL_VCALENDAR_COMPONENT)
		return FALSE;

	subcomp = icalcomponent_get_first_component (icalcomp, ICAL_ANY_COMPONENT);
	while (subcomp) {
		gboolean success;

		kind = icalcomponent_isa (subcomp);
		if (kind == ICAL_VTIMEZONE_COMPONENT) {
			icaltimezone *zone;

			zone = icaltimezone_new ();
			icaltimezone_set_component (zone, subcomp);

			success = e_cal_add_timezone (client, zone, NULL);
			icaltimezone_free (zone, 1);
			if (!success)
				return success;
		} else if (kind == ICAL_VTODO_COMPONENT ||
			   kind == ICAL_VEVENT_COMPONENT) {
			success = update_single_object (client, subcomp);
			if (!success)
				return success;
		}

		subcomp = icalcomponent_get_next_component (icalcomp, ICAL_ANY_COMPONENT);
	}

	return TRUE;
}

static void
selector_tree_drag_data_received (GtkWidget *widget,
				  GdkDragContext *context,
				  gint x,
				  gint y,
				  GtkSelectionData *data,
				  guint info,
				  guint time,
				  gpointer user_data)
{
	GtkTreePath *path = NULL;
	GtkTreeViewDropPosition pos;
	gpointer source = NULL;
	GtkTreeModel *model;
	GtkTreeIter iter;
	gboolean success = FALSE;
	icalcomponent *icalcomp = NULL;
	ECal *client = NULL;

	if (!gtk_tree_view_get_dest_row_at_pos (GTK_TREE_VIEW (widget),
						x, y, &path, &pos))
		goto finish;

	model = gtk_tree_view_get_model (GTK_TREE_VIEW (widget));

	if (!gtk_tree_model_get_iter (model, &iter, path))
		goto finish;


	gtk_tree_model_get (model, &iter, 0, &source, -1);

	if (E_IS_SOURCE_GROUP (source) || e_source_get_readonly (source))
		goto finish;

	icalcomp = icalparser_parse_string ((char *)data->data);

	if (icalcomp) {
		char * uid;

		/* FIXME deal with GDK_ACTION_ASK */
		if (context->action == GDK_ACTION_COPY) {
			uid = e_cal_component_gen_uid ();
			icalcomponent_set_uid (icalcomp, uid);
		}

		client = auth_new_cal_from_source (source,
						   E_CAL_SOURCE_TYPE_EVENT);

		if (client) {
			if (e_cal_open (client, TRUE, NULL)) {
				success = TRUE;
				update_objects (client, icalcomp);
			}

			g_object_unref (client);
		}

		icalcomponent_free (icalcomp);
	}

 finish:
	if (source)
		g_object_unref (source);
	if (path)
		gtk_tree_path_free (path);

	gtk_drag_finish (context, success, context->action == GDK_ACTION_MOVE, time);
}

static void
selector_tree_drag_leave (GtkWidget *widget, GdkDragContext *context, guint time, gpointer data)
{
	gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW (widget),
					NULL, GTK_TREE_VIEW_DROP_BEFORE);
}

static void
control_activate_cb (BonoboControl *control, gboolean activate, gpointer data)
{
	CalendarComponentView *component_view = data;

	if (activate) {
		BonoboUIComponent *uic;
		uic = bonobo_control_get_ui_component (component_view->view_control);

		e_user_creatable_items_handler_activate (component_view->creatable_items_handler, uic);
	}
}


static void
config_create_ecal_changed_cb (GConfClient *client, guint id, GConfEntry *entry, gpointer data)
{
	CalendarComponent *calendar_component = data;
	CalendarComponentPrivate *priv;

	priv = calendar_component->priv;

	g_object_unref (priv->create_ecal);
	priv->create_ecal = NULL;

	priv->notifications = g_list_remove (priv->notifications, GUINT_TO_POINTER (id));
}

static ECal *
setup_create_ecal (CalendarComponent *calendar_component, CalendarComponentView *component_view)
{
	CalendarComponentPrivate *priv;
	ESource *source = NULL;
	char *uid;
	guint not;

	priv = calendar_component->priv;

	/* Try to use the client from the calendar first to avoid re-opening things */
	if (component_view) {
		ECal *default_ecal;

		default_ecal = gnome_calendar_get_default_client (component_view->calendar);
		if (default_ecal)
			return default_ecal;
	}

	/* If there is an existing fall back, use that */
	if (priv->create_ecal)
		return priv->create_ecal;

	/* Get the current primary calendar, or try to set one if it doesn't already exist */
	uid = calendar_config_get_primary_calendar ();
	if (uid) {
		source = e_source_list_peek_source_by_uid (priv->source_list, uid);
		g_free (uid);

		priv->create_ecal = auth_new_cal_from_source (source, E_CAL_SOURCE_TYPE_EVENT);
	}

	if (!priv->create_ecal) {
		/* Try to create a default if there isn't one */
		source = e_source_list_peek_source_any (priv->source_list);
		if (source)
			priv->create_ecal = auth_new_cal_from_source (source, E_CAL_SOURCE_TYPE_EVENT);
	}

	if (priv->create_ecal) {
		icaltimezone *zone;

		zone = calendar_config_get_icaltimezone ();
		e_cal_set_default_timezone (priv->create_ecal, zone, NULL);

		if (!e_cal_open (priv->create_ecal, FALSE, NULL)) {
			GtkWidget *dialog;

			dialog = gtk_message_dialog_new (NULL, GTK_DIALOG_MODAL,
							 GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
							 _("Unable to open the calendar '%s' for creating events and meetings"),
							 e_source_peek_name (source));

			gtk_dialog_run (GTK_DIALOG (dialog));
			gtk_widget_destroy (dialog);
			g_object_unref (priv->create_ecal);
			priv->create_ecal = NULL;

			return NULL;
		}

	} else {
		GtkWidget *dialog;

		dialog = gtk_message_dialog_new (NULL, GTK_DIALOG_MODAL,
						 GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
						 _("There is no calendar available for creating events and meetings"));
		gtk_dialog_run (GTK_DIALOG (dialog));
		gtk_widget_destroy (dialog);

		return NULL;
	}

	/* Handle the fact it may change on us */
	not = calendar_config_add_notification_primary_calendar (config_create_ecal_changed_cb,
								 calendar_component);
	priv->notifications = g_list_prepend (priv->notifications, GUINT_TO_POINTER (not));

	/* Save the primary source for use elsewhere */
	calendar_config_set_primary_calendar (e_source_peek_uid (source));

	return priv->create_ecal;
}

static gboolean
create_new_event (CalendarComponent *calendar_component, CalendarComponentView *component_view, gboolean is_allday, gboolean is_meeting)
{
	ECal *ecal;
	ECalendarView *view;

	ecal = setup_create_ecal (calendar_component, component_view);
	if (!ecal)
		return FALSE;

	if (component_view && (view = E_CALENDAR_VIEW (gnome_calendar_get_current_view_widget (component_view->calendar)))) {
		e_calendar_view_new_appointment_full (view, is_allday, is_meeting, TRUE);
	} else {
		ECalComponent *comp;
		CompEditor *editor;
		CompEditorFlags flags;

		flags = COMP_EDITOR_USER_ORG | COMP_EDITOR_NEW_ITEM;
		if (is_meeting)
			flags |= COMP_EDITOR_MEETING;
		comp = cal_comp_event_new_with_current_time (ecal, is_allday);
		editor = event_editor_new (ecal, flags);
		e_cal_component_commit_sequence (comp);

		comp_editor_edit_comp (editor, comp);
		if (is_meeting)
			event_editor_show_meeting (EVENT_EDITOR (editor));
		gtk_window_present (GTK_WINDOW (editor));

		e_comp_editor_registry_add (comp_editor_registry, editor, TRUE);
	}

	return TRUE;
}

static void
create_local_item_cb (EUserCreatableItemsHandler *handler, const char *item_type_name, void *data)
{
	CalendarComponent *calendar_component = data;
	CalendarComponentPrivate *priv;
	CalendarComponentView *component_view = NULL;
	GList *l;

	priv = calendar_component->priv;

	for (l = priv->views; l; l = l->next) {
		component_view = l->data;

		if (component_view->creatable_items_handler == handler)
			break;

		component_view = NULL;
	}

	if (strcmp (item_type_name, CREATE_EVENT_ID) == 0)
		create_new_event (calendar_component, component_view, FALSE, FALSE);
 	else if (strcmp (item_type_name, CREATE_ALLDAY_EVENT_ID) == 0)
		create_new_event (calendar_component, component_view, TRUE, FALSE);
	else if (strcmp (item_type_name, CREATE_MEETING_ID) == 0)
		create_new_event (calendar_component, component_view, FALSE, TRUE);
	else if (strcmp (item_type_name, CREATE_CALENDAR_ID) == 0)
		calendar_setup_new_calendar (GTK_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (component_view->calendar))));
}

static CalendarComponentView *
create_component_view (CalendarComponent *calendar_component)
{
	CalendarComponentPrivate *priv;
	CalendarComponentView *component_view;
	GtkWidget *selector_scrolled_window, *vbox, *vpane;
	GtkWidget *statusbar_widget;
	guint not;
	AtkObject *a11y;

	priv = calendar_component->priv;

	/* Create the calendar component view */
	component_view = g_new0 (CalendarComponentView, 1);

	vpane = gtk_vpaned_new ();
	g_signal_connect_after (vpane, "realize",
				G_CALLBACK(calcomp_vpane_realized), component_view);
	g_signal_connect (vpane, "button_release_event",
			  G_CALLBACK (calcomp_vpane_resized), component_view);
	gtk_widget_show (vpane);
	/* Add the source lists */
	component_view->source_list = g_object_ref (priv->source_list);
	component_view->task_source_list = g_object_ref (priv->task_source_list);
	component_view->memo_source_list = g_object_ref (priv->memo_source_list);
	component_view->vpane_pos = calendar_config_get_tag_vpane_pos ();

	/* Create sidebar selector */
	component_view->source_selector = e_source_selector_new (calendar_component->priv->source_list);
	e_source_selector_set_select_new ((ESourceSelector *)component_view->source_selector, TRUE);
	a11y = gtk_widget_get_accessible (GTK_WIDGET (component_view->source_selector));
	atk_object_set_name (a11y, _("Calendar Source Selector"));

	g_signal_connect (component_view->source_selector, "drag-motion", G_CALLBACK (selector_tree_drag_motion),
			  calendar_component);
	g_signal_connect (component_view->source_selector, "drag-leave", G_CALLBACK (selector_tree_drag_leave),
			  calendar_component);
	g_signal_connect (component_view->source_selector, "drag-drop", G_CALLBACK (selector_tree_drag_drop),
			  calendar_component);
	g_signal_connect (component_view->source_selector, "drag-data-received",
			  G_CALLBACK (selector_tree_drag_data_received), calendar_component);

	gtk_drag_dest_set(component_view->source_selector, GTK_DEST_DEFAULT_ALL, drag_types,
			  num_drag_types, GDK_ACTION_COPY | GDK_ACTION_MOVE);

	gtk_widget_show (component_view->source_selector);

	selector_scrolled_window = gtk_scrolled_window_new (NULL, NULL);
	gtk_container_add (GTK_CONTAINER (selector_scrolled_window), component_view->source_selector);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (selector_scrolled_window),
					GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (selector_scrolled_window),
					     GTK_SHADOW_IN);
	gtk_widget_show (selector_scrolled_window);

	component_view->info_label = (EInfoLabel *)e_info_label_new("x-office-calendar");
	e_info_label_set_info (component_view->info_label, _("Calendars"), "");
	gtk_widget_show (GTK_WIDGET (component_view->info_label));

	vbox = gtk_vbox_new(FALSE, 0);
	gtk_box_pack_start(GTK_BOX (vbox), GTK_WIDGET (component_view->info_label), FALSE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX (vbox), selector_scrolled_window, TRUE, TRUE, 0);
	gtk_widget_show (vbox);

	gtk_paned_pack1 (GTK_PANED (vpane), vbox, FALSE, FALSE);

	component_view->sidebar_control = bonobo_control_new (vpane);

	/* Create main view */
	component_view->view_control = control_factory_new_control ();
	if (!component_view->view_control) {
		/* FIXME free memory */

		return NULL;
	}

	component_view->calendar = (GnomeCalendar *) bonobo_control_get_widget (component_view->view_control);

	gtk_paned_pack2 (GTK_PANED (vpane), gnome_calendar_get_tag (component_view->calendar), FALSE, FALSE);

	/* This signal is thrown if backends die - we update the selector */
	g_signal_connect (component_view->calendar, "source_added",
			  G_CALLBACK (source_added_cb), component_view);
	g_signal_connect (component_view->calendar, "source_removed",
			  G_CALLBACK (source_removed_cb), component_view);

	/* Create status bar */
	statusbar_widget = e_task_bar_new ();
	component_view->activity_handler = e_activity_handler_new ();
	e_activity_handler_attach_task_bar (component_view->activity_handler, E_TASK_BAR (statusbar_widget));
	gtk_widget_show (statusbar_widget);

	component_view->statusbar_control = bonobo_control_new (statusbar_widget);

	gnome_calendar_set_activity_handler (component_view->calendar, component_view->activity_handler);

	/* connect after setting the initial selections, or we'll get unwanted calls
	   to calendar_control_sensitize_calendar_commands */
	g_signal_connect (component_view->source_selector, "selection_changed",
			  G_CALLBACK (source_selection_changed_cb), component_view);
	g_signal_connect (component_view->source_selector, "primary_selection_changed",
			  G_CALLBACK (primary_source_selection_changed_cb), component_view);
	g_signal_connect (component_view->source_selector, "popup_event",
			  G_CALLBACK (popup_event_cb), component_view);

	/* Set up the "new" item handler */
	component_view->creatable_items_handler = e_user_creatable_items_handler_new ("calendar", create_local_item_cb, calendar_component);
	g_signal_connect (component_view->view_control, "activate", G_CALLBACK (control_activate_cb), component_view);

	/* We use this to update the component information */
	set_info (component_view);
	g_signal_connect (component_view->calendar, "dates_shown_changed",
			  G_CALLBACK (calendar_dates_changed_cb), component_view);

	/* Load the selection from the last run */
	update_selection (component_view);
	update_primary_selection (component_view);
	update_task_memo_selection (component_view, E_CAL_SOURCE_TYPE_TODO);
	update_primary_task_memo_selection (component_view, E_CAL_SOURCE_TYPE_TODO);
	update_task_memo_selection (component_view, E_CAL_SOURCE_TYPE_JOURNAL);
	update_primary_task_memo_selection (component_view, E_CAL_SOURCE_TYPE_JOURNAL);

	/* If the tasks/memos selection changes elsewhere, update it for the mini
	   mini tasks view sidebar */
	not = calendar_config_add_notification_tasks_selected (config_tasks_selection_changed_cb,
							       component_view);
	component_view->notifications = g_list_prepend (component_view->notifications, GUINT_TO_POINTER (not));

	not = calendar_config_add_notification_memos_selected (config_memos_selection_changed_cb,
							       component_view);
	component_view->notifications = g_list_prepend (component_view->notifications, GUINT_TO_POINTER (not));

	not = calendar_config_add_notification_primary_tasks (config_primary_tasks_selection_changed_cb,
							      component_view);
	component_view->notifications = g_list_prepend (component_view->notifications, GUINT_TO_POINTER (not));

	not = calendar_config_add_notification_primary_memos (config_primary_memos_selection_changed_cb,
							      component_view);
	component_view->notifications = g_list_prepend (component_view->notifications, GUINT_TO_POINTER (not));

	return component_view;
}

static void
destroy_component_view (CalendarComponentView *component_view)
{
	GList *l;

	if (component_view->source_list)
		g_object_unref (component_view->source_list);

	if (component_view->task_source_list)
		g_object_unref (component_view->task_source_list);

	if (component_view->memo_source_list)
		g_object_unref (component_view->memo_source_list);

	if (component_view->source_selection)
		e_source_selector_free_selection (component_view->source_selection);

	for (l = component_view->notifications; l; l = l->next)
		calendar_config_remove_notification (GPOINTER_TO_UINT (l->data));
	g_list_free (component_view->notifications);

	if (component_view->creatable_items_handler)
		g_object_unref (component_view->creatable_items_handler);

	if (component_view->activity_handler)
		g_object_unref (component_view->activity_handler);

	if (component_view->task_source_selection) {
		g_slist_foreach (component_view->task_source_selection, (GFunc) g_free, NULL);
		g_slist_free (component_view->task_source_selection);
	}

	if (component_view->memo_source_selection) {
		g_slist_foreach (component_view->memo_source_selection, (GFunc) g_free, NULL);
		g_slist_free (component_view->memo_source_selection);
	}

	g_free (component_view);
}

static void
view_destroyed_cb (gpointer data, GObject *where_the_object_was)
{
	CalendarComponent *calendar_component = data;
	CalendarComponentPrivate *priv;
	GList *l;

	priv = calendar_component->priv;

	for (l = priv->views; l; l = l->next) {
		CalendarComponentView *component_view = l->data;

		if (G_OBJECT (component_view->view_control) == where_the_object_was) {
			priv->views = g_list_remove (priv->views, component_view);
			destroy_component_view (component_view);

			break;
		}
	}
}

static GNOME_Evolution_ComponentView
impl_createView (PortableServer_Servant servant,
		 GNOME_Evolution_ShellView parent,
		 CORBA_boolean select_item,
		 CORBA_Environment *ev)
{
	CalendarComponent *calendar_component = CALENDAR_COMPONENT (bonobo_object_from_servant (servant));
	CalendarComponentPrivate *priv;
	CalendarComponentView *component_view;
	EComponentView *ecv;

	priv = calendar_component->priv;

	/* Create the calendar component view */
	component_view = create_component_view (calendar_component);
	if (!component_view) {
		/* FIXME Should we describe the problem in a control? */
		bonobo_exception_set (ev, ex_GNOME_Evolution_Component_Failed);

		return CORBA_OBJECT_NIL;
	}

	g_object_weak_ref (G_OBJECT (component_view->view_control), view_destroyed_cb, calendar_component);
	priv->views = g_list_append (priv->views, component_view);

	/* TODO: Make CalendarComponentView just subclass EComponentView */
	ecv = e_component_view_new_controls (parent, "calendar", component_view->sidebar_control,
					     component_view->view_control, component_view->statusbar_control);

	return BONOBO_OBJREF(ecv);
}


static void
impl_requestCreateItem (PortableServer_Servant servant,
			const CORBA_char *item_type_name,
			CORBA_Environment *ev)
{
	CalendarComponent *calendar_component = CALENDAR_COMPONENT (bonobo_object_from_servant (servant));
	gboolean result = FALSE;

	if (strcmp (item_type_name, CREATE_EVENT_ID) == 0)
		result = create_new_event (calendar_component, NULL, FALSE, FALSE);
 	else if (strcmp (item_type_name, CREATE_ALLDAY_EVENT_ID) == 0)
		result = create_new_event (calendar_component, NULL, TRUE, FALSE);
	else if (strcmp (item_type_name, CREATE_MEETING_ID) == 0)
		result = create_new_event (calendar_component, NULL, FALSE, TRUE);
	else if (strcmp (item_type_name, CREATE_CALENDAR_ID) == 0)
		/* FIXME Should we use the last opened window? */
		calendar_setup_new_calendar (NULL);
	else
		bonobo_exception_set (ev, ex_GNOME_Evolution_Component_UnknownType);

	if (!result)
		bonobo_exception_set (ev, ex_GNOME_Evolution_Component_Failed);
}

/* GObject methods.  */

static void
impl_dispose (GObject *object)
{
	CalendarComponent *calendar_component = CALENDAR_COMPONENT (object);
	CalendarComponentPrivate *priv = calendar_component->priv;
	GList *l;

	if (priv->source_list != NULL) {
		g_object_unref (priv->source_list);
		priv->source_list = NULL;
	}

	if (priv->gconf_client != NULL) {
		g_object_unref (priv->gconf_client);
		priv->gconf_client = NULL;
	}

	if (priv->create_ecal) {
		g_object_unref (priv->create_ecal);
		priv->create_ecal = NULL;
	}

	for (l = priv->views; l; l = l->next) {
		CalendarComponentView *component_view = l->data;

		g_object_weak_unref (G_OBJECT (component_view->view_control), view_destroyed_cb, calendar_component);
	}
	g_list_free (priv->views);
	priv->views = NULL;

	for (l = priv->notifications; l; l = l->next)
		calendar_config_remove_notification (GPOINTER_TO_UINT (l->data));
	g_list_free (priv->notifications);
	priv->notifications = NULL;

	(* G_OBJECT_CLASS (parent_class)->dispose) (object);
}

static void
impl_finalize (GObject *object)
{
	CalendarComponentPrivate *priv = CALENDAR_COMPONENT (object)->priv;
	GList *l;

	for (l = priv->views; l; l = l->next) {
		CalendarComponentView *component_view = l->data;

		destroy_component_view (component_view);
	}
	g_list_free (priv->views);

	g_free (priv);

	(* G_OBJECT_CLASS (parent_class)->finalize) (object);
}

static void
calendar_component_class_init (CalendarComponentClass *class)
{
	POA_GNOME_Evolution_Component__epv *epv = &class->epv;
	GObjectClass *object_class = G_OBJECT_CLASS (class);

	parent_class = g_type_class_peek_parent (class);

	epv->upgradeFromVersion      = impl_upgradeFromVersion;
	epv->createView              = impl_createView;
	epv->requestCreateItem       = impl_requestCreateItem;
	epv->handleURI               = impl_handleURI;

	object_class->dispose  = impl_dispose;
	object_class->finalize = impl_finalize;
}

static void
calendar_component_init (CalendarComponent *component)
{
	CalendarComponentPrivate *priv;
	guint not;

	priv = g_new0 (CalendarComponentPrivate, 1);

	/* EPFIXME: Should use a custom one instead?  Also we should add
	 * calendar_component_peek_gconf_client().  */
	priv->gconf_client = gconf_client_get_default ();

	not = calendar_config_add_notification_primary_calendar (config_primary_selection_changed_cb,
								 component);
	priv->notifications = g_list_prepend (priv->notifications, GUINT_TO_POINTER (not));

	component->priv = priv;
	ensure_sources (component);

	if (!e_cal_get_sources (&priv->task_source_list, E_CAL_SOURCE_TYPE_TODO, NULL))
		;
	if (!e_cal_get_sources (&priv->memo_source_list, E_CAL_SOURCE_TYPE_JOURNAL, NULL))
		;
}


/* Public API.  */

ESourceList *
calendar_component_peek_source_list (CalendarComponent *component)
{
	return component->priv->source_list;
}

BONOBO_TYPE_FUNC_FULL (CalendarComponent, GNOME_Evolution_Component, PARENT_TYPE, calendar_component)
