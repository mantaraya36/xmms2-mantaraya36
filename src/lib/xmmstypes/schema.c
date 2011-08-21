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

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>

#include "xmmsc/xmmsc_schema.h"

xmmsv_t *find_by_title_in_list (xmmsv_t *list, const char *title);
int schema_type_matches (xmms_schema_type_t schema_type, xmmsv_type_t value_type);
int value_found_in_list (xmmsv_t *list, xmmsv_t *value);
int value_in_range (xmmsv_t *value, xmmsv_t *schema);
xmmsv_t *get_subschema_from_union (xmmsv_t *union_, const char *node_name);
int validate_union (xmmsv_t *schema, xmmsv_t *value);
int str_is_digit (const char *s);

/* schema functions are not reentrant, as they assume they are working on a
  single copy owned by a single thread, which should be the case both for
  clients and the server */

int
xmms_schema_validate (xmmsv_t *schema, xmmsv_t *value, char **error_path)
{
	const char *title;
	int32_t i;
	xmms_schema_type_t schema_type;
	xmmsv_type_t value_type;
	xmmsv_t *subschema, *member, *schema_keys, *enum_ = NULL;

	subschema = schema;
	if (schema) {
		xmmsv_dict_entry_get_string (subschema, "title", &title);
		xmmsv_dict_entry_get_int (subschema, "type", &i);
		schema_type = (xmms_schema_type_t) i;
		value_type = xmmsv_get_type (value);
	}

	if (!schema || schema_type == XMMS_SCHEMA_ANY) {
		return 1;
	}
	if (schema_type == XMMS_SCHEMA_UNION) {
		return validate_union (schema, value);
	}
	if (!schema_type_matches (schema_type, value_type)) {
		return 0;
	}

	if (value_type == XMMSV_TYPE_DICT) {
		xmmsv_dict_iter_t *it;
		xmmsv_dict_get (schema, "entry-type", &schema_keys);
		xmmsv_get_dict_iter (value, &it);
		while (xmmsv_dict_iter_valid (it)) {
			xmmsv_dict_iter_pair (it, &title, &member);
			subschema = find_by_title_in_list (schema_keys, title);
			if (!subschema) {
				return 0;
			}
			if (!xmms_schema_validate (subschema, member, error_path)) {
				return 0;
			}
			xmmsv_dict_iter_next (it);
		}
	} else if (value_type == XMMSV_TYPE_LIST) {
		xmmsv_list_iter_t *it;
		xmmsv_dict_get (schema, "entry-type", &subschema);
		xmmsv_get_list_iter (value, &it);
		while (xmmsv_list_iter_valid (it)) {
			xmmsv_list_iter_entry (it, &member);
			if (!xmms_schema_validate (subschema, member, error_path)) {
				return 0;
			}
			xmmsv_list_iter_next (it);
		}
	} else { /* validate enums */
		xmmsv_dict_get (schema, "enum", &enum_);
		if (enum_ && xmmsv_list_get_size (enum_) > 0
		        && !value_found_in_list (enum_, value)) {
			return 0;
		}
		if (!value_in_range (value, schema)) {
			return 0;
		}
	}
	return 1;
}

xmmsv_t *xmms_schema_get_subschema (xmmsv_t *schema, const char *path)
{
	xmmsv_t *subschema, *child, *union_schema;
	char node_path[XMMS_SCHEMA_PATH_MAX];
	char *node_name, *remaining_path;
	const char *subnode_name;
	xmms_schema_type_t parent_node_type, node_type = XMMS_SCHEMA_ANY;
	int32_t i;

	if (!schema) {
		return 0;
	}
	child = schema;
	strncpy (node_path, path, sizeof (node_path));
	node_name = strtok (node_path,".");
    while (node_name != NULL)
    {
		subschema = child;
		if (!subschema) {
			return NULL;
		}
		xmmsv_dict_entry_get_string (subschema, "title", &subnode_name);
		xmmsv_dict_entry_get_int (subschema, "type", &i);
		parent_node_type = node_type;
		node_type = (xmms_schema_type_t) i;
		if (strcmp (node_name, subnode_name) != 0
		        && parent_node_type != XMMS_SCHEMA_LIST
		        && node_type != XMMS_SCHEMA_UNION
		        && !str_is_digit (node_name)) {
			return NULL;
		}
		switch (node_type) {
		case XMMS_SCHEMA_DICT:
			if (!xmmsv_dict_get (subschema, "entry-type", &child)) {
				return NULL;
			}
			node_name = strtok (NULL, ".");
			child = find_by_title_in_list (child, node_name);
			break;
		case XMMS_SCHEMA_LIST:
			if (!xmmsv_dict_get (subschema, "entry-type", &child)) {
				return NULL;
			}
			node_name = strtok (NULL, ".");
			break;
		case XMMS_SCHEMA_UNION:
			remaining_path = strdup (node_name);
			node_name = strtok (NULL, ".");
			while (node_name) {
				remaining_path = strcat (remaining_path, ".");
				remaining_path = strcat (remaining_path, node_name);
				node_name = strtok (NULL, ".");
			}
			union_schema = get_subschema_from_union (subschema, remaining_path);
			if (!union_schema) {
				subschema = NULL;
			}
			if (strchr (remaining_path, '.') != NULL) {
				subschema = union_schema;
			}
			free (remaining_path);
			/* not sure this will work with nested unions... */
			break;
		default:
			node_name = strtok (NULL, ".");
		}
    }

	return subschema;
}

xmmsv_t *
xmms_schema_get_enum (xmmsv_t *schema, const char *path)
{
	xmmsv_t *enum_;
	enum_ = xmms_schema_get_subschema (schema, path);
	return enum_;
}

xmmsv_t *
xmms_schema_get_union_values (xmmsv_t *schema, const char *path)
{
	xmmsv_t *subschema, *values;

	subschema = xmms_schema_get_subschema (schema, path);

	xmmsv_dict_get (subschema, "values", &values);
	return values;
}

xmms_schema_type_t
xmms_schema_get_type (xmmsv_t *schema, const char *path)
{
	int32_t type;
	xmmsv_t *subschema;

	subschema = xmms_schema_get_subschema (schema, path);

	xmmsv_dict_entry_get_int (subschema, "type", &type);
	return (xmms_schema_type_t) type;
}

const char *
xmms_schema_get_title (xmmsv_t *schema, const char *path)
{
	const char *text;
	xmmsv_t *subschema;

	subschema = xmms_schema_get_subschema (schema, path);

	xmmsv_dict_entry_get_string (subschema, "title", &text);
	return text;
}

const char *
xmms_schema_get_description (xmmsv_t *schema, const char *path)
{
	const char *text;
	xmmsv_t *subschema;

	subschema = xmms_schema_get_subschema (schema, path);

	xmmsv_dict_entry_get_string (subschema, "desc", &text);
	return text;
}

/* builders, for parts of the server which need to register schemas */
xmmsv_t *
xmms_schema_build_int32 (const char *title, const char *description,
                         int32_t default_)
{
	xmmsv_t *schema;
	schema = xmmsv_build_dict (XMMSV_DICT_ENTRY_INT ("type", XMMS_SCHEMA_INT32),
	                           XMMSV_DICT_ENTRY_STR ("title", title),
	                           XMMSV_DICT_ENTRY_STR ("desc", description),
	                           XMMSV_DICT_ENTRY_INT ("default", default_),
	                           XMMSV_DICT_END);
	return schema;
}

xmmsv_t *
xmms_schema_build_int32_full (const char *title, const char *description,
                              int32_t default_, int32_t min, int32_t max,
                              xmmsv_t *enum_)
{
	xmmsv_t *schema;
	if (! enum_) {
		enum_ = xmmsv_new_list ();
	}
	schema = xmmsv_build_dict (XMMSV_DICT_ENTRY_INT ("type", XMMS_SCHEMA_INT32),
	                           XMMSV_DICT_ENTRY_STR ("title", title),
	                           XMMSV_DICT_ENTRY_STR ("desc", description),
	                           XMMSV_DICT_ENTRY_INT ("default", default_),
	                           XMMSV_DICT_ENTRY_INT ("min", min),
	                           XMMSV_DICT_ENTRY_INT ("max", max),
	                           XMMSV_DICT_ENTRY ("enum", enum_),
	                           XMMSV_DICT_END);
	return schema;
}

xmmsv_t *
xmms_schema_build_float (const char *title, const char *description, float default_)
{
	xmmsv_t *schema;
	schema = xmmsv_build_dict (XMMSV_DICT_ENTRY_INT ("type", XMMS_SCHEMA_FLOAT),
	                           XMMSV_DICT_ENTRY_STR ("title", title),
	                           XMMSV_DICT_ENTRY_STR ("desc", description),
	                           XMMSV_DICT_ENTRY_FLOAT ("default", default_),
	                           XMMSV_DICT_END);
	return schema;
}

xmmsv_t *
xmms_schema_build_float_full (const char *title, const char *description,
                              float default_, float min, float max,
                              xmmsv_t *enum_)
{
	xmmsv_t *schema;
	if (! enum_) {
		enum_ = xmmsv_new_list ();
	}
	schema = xmmsv_build_dict (XMMSV_DICT_ENTRY_INT ("type", XMMS_SCHEMA_FLOAT),
	                           XMMSV_DICT_ENTRY_STR ("title", title),
	                           XMMSV_DICT_ENTRY_STR ("desc", description),
	                           XMMSV_DICT_ENTRY_FLOAT ("default", default_),
	                           XMMSV_DICT_ENTRY_FLOAT ("min", min),
	                           XMMSV_DICT_ENTRY_FLOAT ("max", max),
	                           XMMSV_DICT_ENTRY ("enum", enum_),
	                           XMMSV_DICT_END);
	return schema;
}

xmmsv_t *
xmms_schema_build_string (const char *title, const char *description,
                          const char  *default_)
{
	xmmsv_t *schema;
	schema = xmmsv_build_dict (XMMSV_DICT_ENTRY_INT ("type", XMMS_SCHEMA_STRING),
	                           XMMSV_DICT_ENTRY_STR ("title", title),
	                           XMMSV_DICT_ENTRY_STR ("desc", description),
	                           XMMSV_DICT_ENTRY_STR ("default", default_),
	                           XMMSV_DICT_END);
	return schema;
}

xmmsv_t *
xmms_schema_build_string_all (const char *title, const char *description,
                              const char  *default_, xmmsv_t *enum_)
{
	xmmsv_t *schema;
	if (! enum_) {
	    enum_ = xmmsv_new_list ();
	}
	schema = xmmsv_build_dict (XMMSV_DICT_ENTRY_INT ("type", XMMS_SCHEMA_STRING),
	                           XMMSV_DICT_ENTRY_STR ("title", title),
	                           XMMSV_DICT_ENTRY_STR ("desc", description),
	                           XMMSV_DICT_ENTRY_STR ("default", default_),
	                           XMMSV_DICT_ENTRY ("enum", enum_),
	                           XMMSV_DICT_END);
	return schema;
}

xmmsv_t *
xmms_schema_build_list (const char *title, const char *description,
                        xmmsv_t *entry_type)
{
	xmmsv_t *schema;
	schema = xmmsv_build_dict (XMMSV_DICT_ENTRY_INT ("type", XMMS_SCHEMA_LIST),
	                           XMMSV_DICT_ENTRY_STR ("title", title),
	                           XMMSV_DICT_ENTRY_STR ("desc", description),
	                           XMMSV_DICT_ENTRY ("entry-type", entry_type),
	                           XMMSV_DICT_END);
	return schema;
}

xmmsv_t *
xmms_schema_build_dict (const char *title, const char *description,
                        xmmsv_t *entry_type)
{
	xmmsv_t *schema;
	schema = xmmsv_build_dict (XMMSV_DICT_ENTRY_INT ("type", XMMS_SCHEMA_DICT),
	                           XMMSV_DICT_ENTRY_STR ("title", title),
	                           XMMSV_DICT_ENTRY_STR ("desc", description),
	                           XMMSV_DICT_ENTRY ("entry-type", entry_type),
	                           XMMSV_DICT_END);
	return schema;
}

xmmsv_t *
xmms_schema_build_dict_entry_types (xmmsv_t *first_entry, ...)
{
	xmmsv_t *schema, *element;
	va_list ap;

	schema = xmmsv_new_list ();

	va_start (ap, first_entry);
	element = first_entry;

	while (element) {
		if (!xmmsv_list_append (schema, element)) {
			xmmsv_unref (schema);
			return NULL;
		}

		xmmsv_unref (element);

		element = va_arg (ap, xmmsv_t *);
	}
	va_end (ap);
	return schema;
}

xmmsv_t *
xmms_schema_build_union (const char *description, xmmsv_t *first_entry, ...)
{
	xmmsv_t *schema, *values, *element = NULL;
	va_list ap;

	values = xmmsv_new_list ();

	va_start (ap, first_entry);
	element = first_entry;

	while (element) {
		if (!xmmsv_list_append (values, element)) {
			xmmsv_unref (values);
			return NULL;
		}

		xmmsv_unref (element);

		element = va_arg (ap, xmmsv_t *);
	}
	va_end (ap);
	schema = xmmsv_build_dict (XMMSV_DICT_ENTRY_INT ("type", XMMS_SCHEMA_UNION),
	                           XMMSV_DICT_ENTRY_STR ("desc", description),
	                           XMMSV_DICT_ENTRY ("values", values),
	                           XMMSV_DICT_END);
	return schema;
}

xmmsv_t *
xmms_schema_build_any (const char *title, const char *description)
{
	xmmsv_t *schema;
	schema = xmmsv_build_dict (XMMSV_DICT_ENTRY_INT ("type", XMMS_SCHEMA_ANY),
	                           XMMSV_DICT_ENTRY_STR ("desc", description),
	                           XMMSV_DICT_END);
	return schema;
}

xmmsv_t *
find_by_title_in_list (xmmsv_t *list, const char *title)
{
	xmmsv_list_iter_t *it;
	xmmsv_t *dict = NULL;
	const char *tmp_title;

	if (!title || !list || !xmmsv_is_type (list, XMMSV_TYPE_LIST)) {
		return NULL;
	}

	xmmsv_get_list_iter (list, &it);
	while (xmmsv_list_iter_valid (it)) {
		xmmsv_list_iter_entry (it, &dict);
		if (xmmsv_dict_entry_get_string (dict, "title", &tmp_title)
		        && strcmp (tmp_title, title) == 0) {
			return dict;
		}
		xmmsv_list_iter_next (it);
	}
	return NULL;
}

int
schema_type_matches (xmms_schema_type_t schema_type, xmmsv_type_t value_type)
{
	if ( (schema_type == XMMS_SCHEMA_STRING && value_type == XMMSV_TYPE_STRING)
	        || (schema_type == XMMS_SCHEMA_INT32 && value_type == XMMSV_TYPE_INT32)
	        || (schema_type == XMMS_SCHEMA_FLOAT && value_type == XMMSV_TYPE_FLOAT)
	        || (schema_type == XMMS_SCHEMA_DICT && value_type == XMMSV_TYPE_DICT)
	        || (schema_type == XMMS_SCHEMA_LIST && value_type == XMMSV_TYPE_LIST)
	         ) {
		return 1;
	}
	return 0;
}

int
value_found_in_list (xmmsv_t *list, xmmsv_t *value)
{
	xmmsv_list_iter_t *it;
	xmmsv_t *entry;
	const char *s1, *s2;
	int32_t i1, i2;
	float f1, f2;

	xmmsv_get_list_iter (list, &it);

	while (xmmsv_list_iter_valid (it)){
		xmmsv_list_iter_entry (it, &entry);
		switch (xmmsv_get_type (entry)) {
		case XMMSV_TYPE_STRING:
			if (xmmsv_get_string (value, &s1)
			        && xmmsv_get_string (entry, &s2)) {
				if (strcmp (s1, s2) == 0) {
					return 1;
				}
			}
			break;
		case XMMSV_TYPE_FLOAT:
			if (xmmsv_get_float (value, &f1)
			        && xmmsv_get_float (entry, &f2)) {
				if (f1 == f2) {
					return 1;
				}
			}
			break;
		case XMMSV_TYPE_INT32:
			if (xmmsv_get_int (value, &i1)
			        && xmmsv_get_int (entry, &i2)) {
				if (i1 == i2) {
					return 1;
				}
			}
			break;
		default:
			/* don't compare collections */
			break;
		}
		xmmsv_list_iter_next (it);
	}

	return 0;
}

int
value_in_range (xmmsv_t *value, xmmsv_t *schema)
{
	xmmsv_type_t value_type;
	float fmin, fmax, fval;
	int32_t imin, imax, ival;

	value_type = xmmsv_get_type (value);
	switch (value_type) {
	case XMMSV_TYPE_INT32:
		xmmsv_get_int (value, &ival);
		if (xmmsv_dict_entry_get_int (schema, "min", &imin)
		        && ival < imin) {
			return 0;
		}
		if (xmmsv_dict_entry_get_int (schema, "max", &imax)
		        && ival > imax) {
			return 0;
		}
		break;
	case XMMSV_TYPE_FLOAT:
		xmmsv_get_float (value, &fval);
		if (xmmsv_dict_entry_get_float (schema, "min", &fmin)
		        && fval < fmin) {
			return 0;
		}
		if (xmmsv_dict_entry_get_float (schema, "max", &fmax)
		        && fval > fmax) {
			return 0;
		}
		break;
	default:
		break;
	}
	return 1;
}

xmmsv_t *
get_subschema_from_union (xmmsv_t *union_, const char *node_name)
{
	/* will return the first match without checking if there are other
       matches in other members of the union */
	xmmsv_t *subschema, *union_values, *union_entry;
	xmmsv_list_iter_t *it;

	if (!xmmsv_dict_get (union_, "values", &union_values)) {
		return 0;
	}

	xmmsv_get_list_iter (union_values, &it);

	while (xmmsv_list_iter_valid (it)) {
		xmmsv_list_iter_entry (it, &union_entry);
		subschema = xmms_schema_get_subschema (union_entry, node_name);
		if (subschema) {
			break;
		}
		xmmsv_list_iter_next (it);
	}

	return subschema;
}

int
validate_union (xmmsv_t *schema, xmmsv_t *value)
{
	xmmsv_t *values, *element;
	xmmsv_list_iter_t *it;

	xmmsv_dict_get (schema, "values", &values);
	xmmsv_get_list_iter (values, &it);

	while (xmmsv_list_iter_valid (it)) {
		xmmsv_list_iter_entry (it, &element);
		if (xmms_schema_validate (element, value, NULL)) {
			return 1;
		}
		xmmsv_list_iter_next (it);
	}
	return 0;
}

int
str_is_digit (const char *s)
{
	int32_t value;
	int32_t ret;
	char s2[8];

	value = strtol (s, NULL, 10);
	sprintf (s2, "%i", value);

	ret = strcmp (s, s2) == 0 ? 1 : 0;

	return ret;
}
