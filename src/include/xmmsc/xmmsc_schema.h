/*  XMMS2 - X Music Multiplexer System
 *  Copyright (C) 2003-2011 XMMS2 Team
 *
 *  PLUGINS ARE NOT CONSIDERED TO BE DERIVED WORK !!!
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 */

/* schema functions are not reentrant, as they assume they are working on a
  single copy owned by a single thread, which should be the case both for
  clients and the server */

#include "xmmsv.h"

typedef enum {
	XMMS_SCHEMA_INT32,
	XMMS_SCHEMA_FLOAT,
	XMMS_SCHEMA_STRING,
	XMMS_SCHEMA_LIST,
	XMMS_SCHEMA_DICT,
	XMMS_SCHEMA_UNION,
	XMMS_SCHEMA_ANY
} xmms_schema_type_t;

#define XMMS_SCHEMA_PATH_MAX 255

int xmms_schema_validate (xmmsv_t *schema, xmmsv_t *value, char **error_path);

/*querying the schema, usually for clients wanting to know how things are
  organized */
xmmsv_t *xmms_schema_get_subschema (xmmsv_t *schema, const char *path);
xmmsv_t *xmms_schema_get_enum (xmmsv_t *schema, const char *path);
xmmsv_t *xmms_schema_get_union_values (xmmsv_t *schema, const char *path);
xmms_schema_type_t xmms_schema_get_type (xmmsv_t *schema, const char *path);
const char *xmms_schema_get_title (xmmsv_t *schema, const char *path);
const char *xmms_schema_get_description (xmmsv_t *schema, const char *path);

/* builders, for parts of the server which need to register schemas */
xmmsv_t *xmms_schema_build_int32 (const char *title, const char *description, int32_t default_);
xmmsv_t *xmms_schema_build_int32_full (const char *title, const char *description,
									   int32_t default_, int32_t min, int32_t max,
									   xmmsv_t *enum_);

xmmsv_t *xmms_schema_build_float (const char *title, const char *description,float default_);
xmmsv_t *xmms_schema_build_float_full (const char *title, const char *description,
									   float default_, float min, float max,
									   xmmsv_t *enum_);
xmmsv_t *xmms_schema_build_string (const char *title, const char *description,
								   const char *default_);
xmmsv_t *xmms_schema_build_string_all (const char *title, const char *description,
								   const char *default_, xmmsv_t *enum_);
xmmsv_t *xmms_schema_build_list (const char *title, const char *description,
								 xmmsv_t *entry_type);
xmmsv_t *xmms_schema_build_dict (const char *title, const char *description,
								 xmmsv_t *entry_type);
xmmsv_t *xmms_schema_build_dict_entry_types (xmmsv_t *first_entry, ...);
xmmsv_t *xmms_schema_build_union (const char *description, xmmsv_t *first_entry, ...);
xmmsv_t *xmms_schema_build_any (const char *title, const char *description);
