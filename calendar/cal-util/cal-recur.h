/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Evolution calendar recurrence rule functions
 *
 * Copyright (C) 2000 Ximian, Inc.
 *
 * Author: Damon Chaplin <damon@ximian.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
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

#ifndef CAL_RECUR_H
#define CAL_RECUR_H

#include <libgnome/gnome-defs.h>
#include <glib.h>
#include <cal-util/cal-component.h>

BEGIN_GNOME_DECLS

typedef gboolean (* CalRecurInstanceFn) (CalComponent *comp,
					 time_t        instance_start,
					 time_t        instance_end,
					 gpointer      data);

/*
 * Calls the given callback function for each occurrence of the event that
 * intersects the range between the given start and end times (the end time is
 * not included). Note that the occurrences may start before the given start
 * time.
 *
 * If the callback routine returns FALSE the occurrence generation stops.
 *
 * Both start and end can be -1, in which case we start at the events first
 * instance and continue until it ends, or forever if it has no enddate.
 */
void	cal_recur_generate_instances	(CalComponent		*comp,
					 time_t			 start,
					 time_t			 end,
					 CalRecurInstanceFn	 cb,
					 gpointer                cb_data);

END_GNOME_DECLS

#endif
