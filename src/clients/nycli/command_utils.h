/*  XMMS2 - X Music Multiplexer System
 *  Copyright (C) 2003-2011 XMMS2 Team
 *
 *  PLUGINS ARE NOT CONSIDERED TO BE DERIVED WORK !!!
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 */

#ifndef __COMMAND_UTILS_H__
#define __COMMAND_UTILS_H__

#include <xmmsclient/xmmsclient.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"
#include "playlist_positions.h"

#define MAX_STRINGLIST_TOKENS 10

typedef struct command_arg_time_St command_arg_time_t;

typedef enum {
	COMMAND_ARG_TIME_POSITION,
	COMMAND_ARG_TIME_OFFSET
} command_arg_time_type_t;

struct command_arg_time_St {
	union {
		guint pos;
		gint offset;
	} value;
	command_arg_time_type_t type;
};

gboolean command_flag_boolean_get (command_context_t *ctx, const gchar *name, gboolean *v);
gboolean command_flag_int_get (command_context_t *ctx, const gchar *name, gint *v);
gboolean command_flag_string_get (command_context_t *ctx, const gchar *name, const gchar **v);
gboolean command_flag_stringlist_get (command_context_t *ctx, const gchar *name, const gchar ***v);
gchar *command_name_get (command_context_t *ctx);
gint command_arg_count (command_context_t *ctx);
gchar** command_argv_get (command_context_t *ctx);
gboolean command_arg_int_get (command_context_t *ctx, gint at, gint *v);
gboolean command_arg_string_get (command_context_t *ctx, gint at, const gchar **v);
gboolean command_arg_longstring_get (command_context_t *ctx, gint at, gchar **v);
gboolean command_arg_longstring_get_escaped (command_context_t *ctx, gint at, gchar **v);
gboolean command_arg_time_get (command_context_t *ctx, gint at, command_arg_time_t *v);
gboolean command_arg_pattern_get (command_context_t *ctx, gint at, xmmsc_coll_t **v, gboolean warn);
gboolean command_arg_positions_get (command_context_t *ctx, gint at, playlist_positions_t **p, gint currpos);

#endif /* __COMMAND_UTILS_H__ */
