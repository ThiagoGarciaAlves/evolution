/* Miscellaneous time-related utilities
 *
 * Copyright (C) 1998 The Free Software Foundation
 *
 * Authors: Federico Mena <federico@nuclecu.unam.mx>
 *          Miguel de Icaza <miguel@nuclecu.unam.mx>
 */

#ifndef TIMEUTIL_H
#define TIMEUTIL_H


#include <time.h>


time_t time_from_isodate (char *str);
time_t time_from_start_duration (time_t start, char *duration);

/* Returns pointer to a statically-allocated buffer with a string of the form
 * 3am, 4am, 12pm, 08h, 17h, etc.
 * The string is internationalized, hopefully correctly.
 */
char *format_simple_hour (int hour, int use_am_pm);


#endif
