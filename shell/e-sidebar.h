/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/* e-sidebar.h
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
 */

#ifndef E_SIDEBAR_H
#define E_SIDEBAR_H

#include <gtk/gtk.h>

/* Standard GObject macros */
#define E_TYPE_SIDEBAR \
	(e_sidebar_get_type ())
#define E_SIDEBAR(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), E_TYPE_SIDEBAR, ESidebar))
#define E_SIDEBAR_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), E_TYPE_SIDEBAR, ESidebarClass))
#define E_IS_SIDEBAR(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), E_TYPE_SIDEBAR))
#define E_IS_SIDEBAR_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_TYPE \
	((obj), E_TYPE_SIDEBAR))
#define E_SIDEBAR_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), E_TYPE_SIDEBAR, ESidebarClass))

G_BEGIN_DECLS

typedef struct _ESidebar ESidebar;
typedef struct _ESidebarClass ESidebarClass;
typedef struct _ESidebarPrivate ESidebarPrivate;

struct _ESidebar {
	GtkBin parent;
	ESidebarPrivate *priv;
};

struct _ESidebarClass {
	GtkBinClass parent_class;
};

GType		e_sidebar_get_type		(void);
GtkWidget *	e_sidebar_new			(void);
void		e_sidebar_add_action		(ESidebar *sidebar,
						 GtkAction *action);
gboolean	e_sidebar_get_actions_visible	(ESidebar *sidebar);
void		e_sidebar_set_actions_visible	(ESidebar *sidebar,
						 gboolean visible);
GtkToolbarStyle	e_sidebar_get_toolbar_style	(ESidebar *sidebar);
void		e_sidebar_set_toolbar_style	(ESidebar *sidebar,
						 GtkToolbarStyle style);

G_END_DECLS

#endif /* E_SIDEBAR_H */
