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

#include <glib.h>

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "xmmsc/xmmsc_idnumbers.h"
#include "xmmsc/xmmsv.h"
#include "xmmsc/xmmsc_schema.h"
#include "xmmspriv/xmms_config.h"
#include "xmmspriv/xmms_utils.h"
#include "xmms/xmms_ipc.h"
#include "xmms/xmms_log.h"

/*
#include "xmms/util.h"
#include "xmms/xmms.h"
#include "xmms/object.h"
#include "xmms/signal_xmms.h"
#include "xmms/plugin.h"
#include "xmms/ipc.h"
*/

/** @internal */
typedef enum {
	XMMS_CONFIG_STATE_INVALID,
	XMMS_CONFIG_STATE_START,
	XMMS_CONFIG_STATE_SECTION,
	XMMS_CONFIG_STATE_PROPERTY
} xmms_configparser_state_t;

typedef struct dump_tree_data_St {
	FILE *fp;
	xmms_configparser_state_t state;

	gchar indent[128];
	guint indent_level;

	gchar *prev_key;
} dump_tree_data_t;

static GTree *xmms_config_client_list_values (xmms_config_t *conf, xmms_error_t *err);
static void xmms_config_property_destroy (xmms_object_t *object);
static gchar *xmms_config_client_get_value (xmms_config_t *conf, const gchar *key, xmms_error_t *err);
static gchar *xmms_config_client_register_value (xmms_config_t *config, const gchar *name, const gchar *def_value, xmms_error_t *error);
static gint compare_key (gconstpointer a, gconstpointer b, gpointer user_data);
static void xmms_config_client_set_value (xmms_config_t *conf, const gchar *key, const gchar *value, xmms_error_t *err);
static xmmsv_t *xmms_config_path_data (xmms_config_t *config, const gchar *path);
static xmmsv_t *xmms_config_path_parent (xmms_config_t *config, const gchar *path);
gboolean value_is_consistent (xmmsv_t *config_value, xmmsv_t *value);
gboolean list_is_consistent (xmmsv_t *config_value, xmmsv_t *value);
gboolean dict_is_consistent (xmmsv_t *config_value, xmmsv_t *value);
gboolean config_register_path (xmms_config_t *config, const gchar *path);
gboolean is_digit (const gchar *s);
gboolean fill_tree_from_values (xmmsv_t *parent_node, const gchar *prefix, GTree *tree);
static gboolean dump_node_tree (xmmsv_t *node, dump_tree_data_t *data);
gboolean xmms_config_set_unlocked (xmms_config_t *config, const gchar *path, xmmsv_t *value);
void xmms_config_value_callback_remove (xmmsv_t *value, xmms_object_handler_t cb, gpointer userdata);
void xmms_config_value_callback_set (xmmsv_t *value, const gchar *path, xmms_object_handler_t cb, gpointer userdata);
gboolean xmms_config_set_if_not_existent (xmms_config_t *config, const gchar *prefix, xmmsv_t *default_value);

void xmms_configv_unref (xmmsv_t *value);

#include "config_ipc.c"

/**
 * @defgroup Config Config
 * @brief Controls configuration for the server.
 *
 * The configuration is saved to, and loaded from an XML file. It's split into
 * plugin, client and core parts. This documents the configuration for parts
 * inside the server. For plugin config see each server object's documentation.
 *
 * @ingroup XMMSServer
 * @{
 */

/**
 * Global parsed config
 */
struct xmms_config_St {
	xmms_object_t obj;

	const gchar *filename;
	xmmsv_t *properties_list;
	GTree *schemas;

	/* Lock on globals are great! */
	GMutex *mutex;

	/* parsing */
	gboolean is_parsing;
	GQueue *states;
	GQueue *sections;
	gchar *value_name;
	gchar *value_type;
	guint version;
};

/**
 * A config property in the configuration file
 */
struct xmms_config_property_St {
	xmms_object_t obj;

	/** Name of the config directive */
	gchar *name;
	/** The data */
	xmmsv_t *value;
};

/**
 * Global config
 * Since there can only be one configuration per server
 * we can have the convenience of having it as a global variable.
 */

static xmms_config_t *global_config = NULL;

/**
 * Config file version
 */
#define XMMS_CONFIG_VERSION 3

/**
 * @}
 * @addtogroup Config
 * @{
 */

/**
 * Config functions
 */

/**
 * Lookup config key and return its associated value as a string.
 * This is a convenient function to make it easier to get a configuration value
 * rather than having to call #xmms_config_property_get_string separately.
 * The caller must free the returned string using g_free().
 *
 * @param conf Global config
 * @param key Configuration property to lookup
 * @param err if error occurs this will be filled in
 *
 * @return A string with the value.
 */
gchar *
xmms_config_property_lookup_get_string (xmms_config_t *conf, const gchar *key,
                                        xmms_error_t *err)
{
	xmms_config_property_t *prop;

	prop = xmms_config_lookup (key);
	if (!prop) {
		xmms_error_set (err, XMMS_ERROR_NOENT,
		                "Trying to get non-existent property");
		return NULL;
	}

	return xmms_config_property_get_string (prop);
}

/**
 * Look up a config key from the global config
 * @param path A configuration path. Could be core.myconfig or
 * effect.foo.myconfig
 * @return An #xmms_config_property_t
 */
xmms_config_property_t *
xmms_config_lookup (const gchar *path)
{
	xmms_config_property_t *prop = NULL;
	xmms_config_t *config = global_config;
	xmmsv_t *prop_value;

	g_return_val_if_fail (config, NULL);

	prop_value = xmms_config_path_data (config, path);
	if (prop_value) {
		prop = (xmms_config_property_t *) xmmsv_get_obj (prop_value);
		if (!prop) {
			/* FIXME this leaks like mad, as API has changed and now gives out a copy instead of a reference, also from xmms_config_property_register */
			prop = xmms_object_new (xmms_config_property_t, xmms_config_property_destroy);
			prop->name = g_strdup (path);
			prop->value = prop_value;
			xmmsv_ref (prop_value);
		}
	}
	return prop;
}

/**
 * Get the name of a config property.
 * @param prop The config property
 * @return Name of config property
 */
const gchar *
xmms_config_property_get_name (const xmms_config_property_t *prop)
{
	g_return_val_if_fail (prop, NULL);

	return prop->name;
}

/**
 * Set the data of the config property to a new value
 * @param prop The config property
 * @param data The value to set
 */
void
xmms_config_property_set_data (xmms_config_property_t *prop, const gchar *data)
{
	xmms_config_t *config = global_config;
	gchar *path;
	xmmsv_t *value;

	g_return_if_fail (prop);
	g_return_if_fail (data);

	value = xmmsv_new_string (data);
	path = g_strdup (prop->name);
	xmms_config_set (config, path, value);
	g_free (path);
	xmmsv_unref (value);
}

/**
 * Return the value of a config property as a string. If the property does not
 exist an empty string will be returned. The caller must free the returned
 string using g_free().
 * @param prop The config property
 * @return value as string.
 */
gchar *
xmms_config_property_get_string (const xmms_config_property_t *prop)
{
	gchar *s;
	gboolean ok;
	xmms_config_t *config = global_config;

	g_return_val_if_fail (prop, NULL);
	s = xmms_config_get_string (config, prop->name, &ok);
	if (!s) {
		s = g_strdup ("");
	}
	return s;
}

/**
 * Return the value of a config property as an int
 * @param prop The config property
 * @return value as int
 */
gint32
xmms_config_property_get_int (const xmms_config_property_t *prop)
{
	gint32 i;
	gboolean ok;
	xmms_config_t *config = global_config;

	g_return_val_if_fail (prop, 0);
	i = xmms_config_get_int (config, prop->name, &ok);
	if (ok) {
		return i;
	} else {
		return 0;
	}
}

/**
 * Return the value of a config property as a float
 * @param prop The config property
 * @return value as float
 */
gfloat
xmms_config_property_get_float (const xmms_config_property_t *prop)
{
	gfloat f;
	gboolean ok;
	xmms_config_t *config = global_config;

	g_return_val_if_fail (prop, 0.0);
	f = xmms_config_get_float (config, prop->name, &ok);
	if (ok) {
		return f;
	} else {
		return 0.0;
	}
}

/**
 * Set a callback function for a config property.
 * This will be called each time the property's value changes.
 * @param prop The config property
 * @param cb The callback to set
 * @param userdata Data to pass on to the callback
 */
void
xmms_config_property_callback_set (xmms_config_property_t *prop,
                                   xmms_object_handler_t cb,
                                   gpointer userdata)
{
	xmms_config_t *config;
	g_return_if_fail (prop);

	if (!cb)
		return;


	config = global_config;
	xmms_config_callback_set (config, prop->name, cb, userdata);
}

/**
 * Remove a callback from a config property
 * @param prop The config property
 * @param cb The callback to remove
 */
void
xmms_config_property_callback_remove (xmms_config_property_t *prop,
                                      xmms_object_handler_t cb,
                                      gpointer userdata)
{
	xmmsv_t *value;
	xmms_config_t *config;
	g_return_if_fail (prop);

	if (!cb)
		return;

	config = global_config;
	value = xmms_config_path_data (config, prop->name);
	xmms_config_value_callback_remove (value, cb, userdata);
}

/**
 * Register a new config property. This should be called from the init code
 * as XMMS2 won't allow set/get on properties that haven't been registered.
 * If the property is already registered, this statement will only register the
 * callback function.
 *
 * @param path The path in the config tree.
 * @param default_value If the value was not found in the configfile, what
 * should we use?
 * @param cb A callback function that will be called if the value is changed by
 * the client. Can be set to NULL.
 * @param userdata Data to pass to the callback function.
 * @return A newly allocated #xmms_config_property_t for the registered
 * property.
 */
xmms_config_property_t *
xmms_config_property_register (const gchar *path,
                               const gchar *default_value,
                               xmms_object_handler_t cb,
                               gpointer userdata)
{
	xmms_config_t *config;
	xmmsv_t *value;
	xmms_config_property_t *prop;

	config = global_config;

	value = xmmsv_new_string (default_value);

	xmms_config_register_value (config, path, value, cb, userdata);
	xmmsv_unref (value);
	prop = xmms_config_lookup (path);
	return prop;
}

/**
* Get the xmmst_v struct value for the given path. The returned value is a copy,
* so the caller must unref it using xmmsv_unref().
*
* @param config The config singleton
* @param path The path in the config tree.
* @return A copy of the xmms_v struct in the path
*/
xmmsv_t *
xmms_config_get (xmms_config_t *config, const gchar *path)
{
	xmmsv_t *value, *out_value = NULL;

	if (!config) { /* hack! */
		config = global_config;
	}
	if (strlen (path) == 0) {
		return xmmsv_copy (config->properties_list);
	}
	g_mutex_lock (config->mutex);

	value = xmms_config_path_data (config, path);
	g_mutex_unlock (config->mutex);
	if (value) {
		out_value = xmmsv_copy (value);
	}
	return out_value;
}

/**
* Set the xmmst_v struct value for the given path. If the property is not registered
* it is registered in the tree. This function checks for consistency between the
* existing tree nodes and the new ones, and will fail if there is a mismatch in
* types. It is however valid to shrink or enlarge the tree.
*
* @param config The config singleton
* @param path The path in the config tree.
* @param value The node to set.
* @return True if property was set succesfully
*/
gboolean
xmms_config_set (xmms_config_t *config, const gchar *path, xmmsv_t *value)
{
	gboolean ret;
	gchar *data = NULL, *path_root;
	const gchar *s;
	gint32 i;
	gfloat f;
	GTree *dict;
	xmmsv_t *value_set, *schema, *subschema;
	xmms_config_property_t *obj;

	if (!config) {
		config = global_config; /* hack! */
	}

	g_return_val_if_fail (path, FALSE);

	path_root = g_strndup (path, strcspn (path, "."));
	schema = (xmmsv_t *) g_tree_lookup (config->schemas, path_root);
	subschema = xmms_schema_get_subschema (schema, path);
	g_free (path_root);

	if (!xmms_schema_validate (subschema, value, NULL)) {
		return FALSE;
	}
	g_mutex_lock (config->mutex);
	/* TODO must check if any parent has a callback, to force its call */

	ret = xmms_config_set_unlocked (config, path, value);

	g_mutex_unlock (config->mutex);

	switch (xmmsv_get_type (value)) {
	case XMMSV_TYPE_STRING:
		xmmsv_get_string (value, &s);
		data = g_strdup (s);
		break;
	case XMMSV_TYPE_INT32:
		xmmsv_get_int (value, &i);
		data = g_strdup_printf ("%i", i);
		break;
	case XMMSV_TYPE_FLOAT:
		xmmsv_get_float (value, &f);
		data = g_strdup_printf ("%.6f", f);
		break;
		/* TODO what to emit when a list or a dict is set */
	default:
		break;
	}

	value_set = xmms_config_path_data (config, path);
	if (data && value_set) {
		obj = (xmms_config_property_t *) xmmsv_get_obj (value_set);
		if (obj) {
			/* TODO emit value typed, not cast to string */
			xmms_object_emit (XMMS_OBJECT (obj),
			                  XMMS_IPC_SIGNAL_CONFIGVALUE_CHANGED,
			                  (gpointer) data);
			dict = g_tree_new_full (compare_key, NULL,
			                        NULL, (GDestroyNotify) xmmsv_unref);
			g_tree_insert (dict, (gchar *) path,
			               xmmsv_copy (value_set));
			xmms_object_emit_f (XMMS_OBJECT (config),
			                    XMMS_IPC_SIGNAL_CONFIGVALUE_CHANGED,
			                    XMMSV_TYPE_DICT,
			                    dict);

			g_tree_destroy (dict);
		}
		g_free (data);
	}
	/* save the database to disk, so we don't lose any data
     * if the daemon crashes
     */
	xmms_config_save ();
	return ret;
}

gboolean
xmms_config_set_unlocked (xmms_config_t *config, const gchar *path, xmmsv_t *value)
{
	xmmsv_t *parent_value, *data_value, *new_val;
	xmms_config_property_t *obj = NULL;
	gchar last_name[128];
	gint len = sizeof (last_name);
	const gchar *lastdot;
	gboolean ret;

	if (strlen (path) == 0) { /* special case: root dictionary is completely replaced */
		if (xmmsv_is_type (value, XMMSV_TYPE_DICT)) {
			if (config->properties_list) {
				xmms_configv_unref (config->properties_list);
			}
			config->properties_list = xmmsv_copy (value);
			return TRUE;
		} else {
			return FALSE;
		}
	}
	if (!config_register_path (config, path)) {
		return FALSE;
	}
	parent_value = xmms_config_path_parent (config, path);
	data_value = xmms_config_path_data (config, path);
	if (data_value) { /* to get object from current value in tree */
		obj = xmmsv_get_obj (data_value);
		xmmsv_set_obj (data_value, NULL); /* release ownership */
	}
	lastdot = strrchr (path, '.');
	if (lastdot) {
		strncpy (last_name, lastdot + 1, len);
	} else {
		strncpy (last_name, path, len);
	}
	if (data_value && !value_is_consistent (data_value, value)) {
		xmms_log_info ("Inconsistent value structure. Old structure replaced.");
	}
	new_val = xmmsv_copy (value);
	if (xmmsv_is_type (parent_value, XMMSV_TYPE_DICT)) {
		ret = xmmsv_dict_set (parent_value, last_name, new_val);
		xmmsv_unref (new_val); /* need to unref as setting in list increases the ref count */
	} else if (xmmsv_is_type (parent_value, XMMSV_TYPE_LIST)) {
		gint index = strtol (last_name, NULL, 10);
		if (index == 0 && strcmp (last_name, "0") != 0) {
			xmms_log_error ("Invalid index for list node.");
			xmmsv_unref (new_val);
			return FALSE;
		}
		ret = xmmsv_list_set (parent_value, index, new_val);
		xmmsv_unref (new_val); /* need to unref as setting in list increases the ref count */
	}
	if (obj) {
		g_assert (strcmp (obj->name, path) == 0);
		obj->value = new_val;
		xmmsv_set_obj (new_val, obj);
	}
	return ret;
}

/**
* Set the integer value for the given path. If the property is not registered
* it is registered in the tree. This function checks calls xmms_config_set internally
*
* @param config The config singleton
* @param path The path in the config tree.
* @param value The integer value
* @return True if property was set succesfully
*/
gboolean
xmms_config_set_int (xmms_config_t *config, const gchar *path, gint32 value)
{
	xmmsv_t *val;
	gboolean ret;

	val = xmmsv_new_int (value);
	ret = xmms_config_set (config, path, val);
	xmmsv_unref (val);
	return ret;
}

/**
* Set the float value for the given path. If the property is not registered
* it is registered in the tree. This function checks calls xmms_config_set internally
*
* @param config The config singleton
* @param path The path in the config tree.
* @param value The float value
* @return True if property was set succesfully
*/
gboolean xmms_config_set_float (xmms_config_t *config, const gchar *path, gfloat value)
{
	xmmsv_t *val;
	gboolean ret;

	val = xmmsv_new_float (value);
	ret = xmms_config_set (config, path, val);
	xmmsv_unref (val);
	return ret;
}

/**
* Set the string value for the given path. If the property is not registered
* it is registered in the tree. This function checks calls xmms_config_set internally
*
* @param config The config singleton
* @param path The path in the config tree.
* @param value The string value
* @return True if property was set succesfully
*/
gboolean xmms_config_set_string (xmms_config_t *config, const gchar *path, const gchar *value)
{
	xmmsv_t *val;
	gboolean ret;

	val = xmmsv_new_string (value);
	ret = xmms_config_set (config, path, val);
	xmmsv_unref (val);
	return ret;
}

/**
* Gets the integer value for the given path. If the node is of float type, no conversion
* is made and the return value will be garbage. If the node is a string, the value
* will be converted
*
* @param config The config singleton
* @param path The path in the config tree.
* @param ok Will be set to true if the property was found, otherwise the return value is garbage
* @return The integer value
*/
gint32
xmms_config_get_int (xmms_config_t *config, const gchar *path, gboolean *ok)
{
	xmmsv_t *value = NULL;
	gint32 i = 0;
	const gchar *s;

	*ok = TRUE;
	value = xmms_config_get (config, path);
	if (value) {
		*ok = xmmsv_get_int (value, &i);
		if (!*ok && xmmsv_is_type (value, XMMSV_TYPE_STRING)) {
			*ok = xmmsv_get_string (value, &s);
			i = atoi (s);
		}
		xmmsv_unref (value);
	}
	return i;
}

/**
* Gets the float value for the given path. If the node is of int type, no conversion
* is made and the return value will be garbage. If the node is a string, the value
* will be converted
*
* @param config The config singleton
* @param path The path in the config tree.
* @param ok Will be set to true if the property was found, otherwise the return value is garbage
* @return The integer value
*/
gfloat
xmms_config_get_float (xmms_config_t *config, const gchar *path, gboolean *ok)
{
	xmmsv_t *value = NULL;
	const gchar *s;
	gfloat f = 0.0;

	*ok = TRUE;
	value = xmms_config_get (config, path);
	if (value) {
		*ok = xmmsv_get_float (value, &f);
		if (!*ok) {
			*ok = xmmsv_get_string (value, &s);
			if (*ok) {
				f = strtod (s, NULL);
			} else {
				f = 0.0; /* give up */
			}
		}
		xmmsv_unref (value);
	}
	return f;
}

/**
* Gets the string value for the given path. If the value is a float or an int, it
* will be converted to a string.
* In contrast to xmms_config_property_get_string, xmms_config_get_string will return
* a NULL pointer if the property doesn't exist.
* The caller must free returned string with g_free()
*
* @param config The config singleton
* @param path The path in the config tree.
* @param ok Will be set to true if the property was found, otherwise the return value is garbage
* @return The string value
*/
gchar* xmms_config_get_string (xmms_config_t *config, const gchar *path, gboolean *ok)
{
	xmmsv_t *value = NULL;
	const gchar *s;
	gchar *out = NULL;

	*ok = FALSE;
	value = xmms_config_get (config, path);
	if (value) {
		*ok = xmmsv_get_string (value, &s);
		if (*ok) {
			out = g_strdup_printf ("%s", s);
		}
		xmmsv_unref (value);
	}
	return out;
}

/**
 * Register a new config property value. This should be called from the init code
 * as XMMS2 won't allow set/get on properties that haven't been registered.
 * If a property node of the value is already registered, this statement will only register the
 * callback function, without changing its value.
 *
 * @param config The config object
 * @param path The path in the config tree.
 * @param default_value holds values to set if any particular node doesn't exist
 * @param cb A callback function that will be called if the value is changed by
 * the client. Can be set to NULL.
 * @param userdata Data to pass to the callback function.
 * @return True if the registration was sucessful
 * property.
 */
gboolean
xmms_config_register_value (xmms_config_t *config,
                            const gchar *path,
                            xmmsv_t *default_value,
                            xmms_object_handler_t cb,
                            gpointer userdata)
{
	xmmsv_t *value, *new_value = NULL;
	gboolean ret;

	if (!config) {
		config = global_config;
	}

	value = xmms_config_path_data (config, path);

	if (!value) {
		new_value = default_value;
		ret = xmms_config_set (config, path, new_value);
	} else {
		if (value_is_consistent (value, default_value)) {
			ret = xmms_config_set_if_not_existent (config, path, default_value);
			new_value = xmms_config_path_data (config, path);
		} else {
			ret = xmms_config_set (config, path, default_value);
			new_value = default_value;
		}
	}

	if (cb) {
		xmms_config_callback_set (config, path, cb, userdata);
	}

	return ret;
}

/**
 * Set a callback function for a config value node.
 * This will be called each time the property's value changes.
 * @param prop The config property
 * @param cb The callback to set
 * @param userdata Data to pass on to the callback
 */
void
xmms_config_callback_set (xmms_config_t *config,
                          const gchar *path,
                          xmms_object_handler_t cb,
                          gpointer userdata)
{
	xmmsv_t *value;

	if (!config) {
		config = global_config;
	}
	value = xmms_config_path_data (config, path);
	if (!value)
		return;
	xmms_config_value_callback_set (value, path, cb, userdata);
}

/**
 * Remove a callback from a config value node.
 * @param prop The config property
 * @param cb The callback to remove
 */
void
xmms_config_callback_remove (xmms_config_t *config,
                             const gchar *path,
                             xmms_object_handler_t cb,
                             gpointer userdata)
{
	xmmsv_t *value;

	if (!config) {
		config = global_config;
	}
	value = xmms_config_path_data (config, path);
	if (!value)
		return;
	xmms_config_value_callback_remove (value, cb, userdata);
}

/**
 * Set schema for configuration node for validation and information when
 * setting properties. A schema can only be registered for nodes belonging to the
 * root dicitionary of the config tree. Only one schema can be registered per
 * node, so if a schema is already registered, it will be replaced by the new one.
 * @param prop The config property
 * @param key The name of the node to assign the schema to
 * @param schema The schema
 * @return true is the schema was registered successfully
 */
gboolean
xmms_config_register_schema (xmms_config_t *config, const gchar *key, xmmsv_t *value)
{
	gboolean ret;
	/* TODO make this function append schemas to existing tree allowing any path
	  and possibily have a clear_schema function as well */

	if (strchr (key, '.')) {
		xmms_log_error ("Can only set root schemas.");
		return FALSE;
	}

	ret = TRUE;
	g_tree_insert (config->schemas, strdup (key), (gpointer) value);
	return ret;
}

/**
 * Checks the value and path against registered schemas in the config properties
 * and reports whether it validates against the schema. If there is no schema,
 * this function will return true
 * @param config The config object
 * @param path the path where the value would go
 * @param value The value to be checked
 * @return true is the value conforms to the schema, or there is no schema registered
 */

gboolean
xmms_config_value_is_valid (xmms_config_t *config, const gchar *path, xmmsv_t *value)
{
	const gchar *subpath;
	xmmsv_t *schema;

	subpath = strchr (path, '.') + 1;

	schema = (xmmsv_t *) g_tree_lookup (config->schemas, subpath);

	return xmms_schema_validate (schema, value, NULL);
}

void
xmms_config_value_callback_set (xmmsv_t *value,
                                const gchar *path,
                                xmms_object_handler_t cb,
                                gpointer userdata)
{
	xmms_config_property_t *obj;
	if (!cb)
		return;
	obj = (xmms_config_property_t *) xmmsv_get_obj (value);
	if (!obj) {
		/* TODO this must be freed, in this file not inside xmmsv_t */
		obj = xmms_object_new (xmms_config_property_t, xmms_config_property_destroy);
		obj->name = g_strdup (path);
		obj->value = value;
		xmmsv_set_obj (value, obj);
	}
	xmms_object_connect (XMMS_OBJECT (obj),
	                     XMMS_IPC_SIGNAL_CONFIGVALUE_CHANGED,
	                     (xmms_object_handler_t) cb, userdata);
}

void
xmms_config_value_callback_remove (xmmsv_t *value,
                                   xmms_object_handler_t cb,
                                   gpointer userdata)
{
	xmms_config_property_t *obj;
	if (!cb)
		return;
	obj = (xmms_config_property_t *) xmmsv_get_obj (value);
	if (obj) {
		xmms_object_disconnect (XMMS_OBJECT (obj),
		                        XMMS_IPC_SIGNAL_CONFIGVALUE_CHANGED, cb, userdata);
	}

	g_return_if_fail (obj);
}


gboolean
xmms_config_set_if_not_existent (xmms_config_t *config,
                                 const gchar *prefix,
                                 xmmsv_t *new_value)
{
	gboolean ret = TRUE;
	xmmsv_t *node, *value;
	const gchar *key;
	gchar new_prefix[128];
	gint i;
	xmmsv_list_iter_t *itl;
	xmmsv_dict_iter_t *itd;

	node = xmms_config_path_data (config, prefix);
	if (!node || xmmsv_is_type (node, XMMSV_TYPE_NONE)) {
		return xmms_config_set (config, prefix, new_value);
	}

	switch (xmmsv_get_type (node)) {
	case XMMSV_TYPE_LIST:
		xmmsv_get_list_iter (node, &itl);
		i = 0;
		while (xmmsv_list_iter_valid (itl)) {
			gboolean tmp_ret;
			xmmsv_list_iter_entry (itl, &value);
			sprintf (new_prefix, "%s.%i", prefix, i);
			tmp_ret = xmms_config_set_if_not_existent (config, new_prefix, value);
			if (!tmp_ret) {
				ret = FALSE;
			}
			xmmsv_list_iter_next (itl);
			i++;
		}
		break;
	case XMMSV_TYPE_DICT:
		xmmsv_get_dict_iter (node, &itd);
		while (xmmsv_dict_iter_valid (itd)) {
			gboolean tmp_ret;
			xmmsv_dict_iter_pair (itd, &key, &value);
			if (strlen (prefix) > 0) {
				sprintf (new_prefix, "%s.%s", prefix, key);
			} else { /* root node is always a dict */
				strcpy (new_prefix, key);
			}
			tmp_ret = xmms_config_set_if_not_existent (config, new_prefix, value);
			if (!tmp_ret) {
				ret = FALSE;
			}
			xmmsv_dict_iter_next (itd);
		}
		break;
	case XMMSV_TYPE_INT32:
	case XMMSV_TYPE_FLOAT:
	case XMMSV_TYPE_STRING:
	case XMMSV_TYPE_NONE:
		/* end of tree, do nothing */
		break;
	default:
		xmms_log_error ("Invalid type in config");
		ret = FALSE;
		break;
	}

	return ret;
}

void
xmms_configv_unref (xmmsv_t *value)
{
	g_return_if_fail (value);
	xmms_object_t *obj;
	/* FIXME get inside the values and free the objects */
	obj = xmmsv_get_obj (value);
	if (obj) {
		xmms_object_unref (obj);
	}
	xmmsv_unref (value);
}

/**
 * @}
 *
 * @if internal
 * @addtogroup Config
 * @{
 */

/**
 * @internal Get the current parser state for the given element name
 * @param[in] name Element name to match to a state
 * @return Parser state matching element name
 */
static xmms_configparser_state_t
get_current_state (const gchar *name)
{
	static struct {
		const gchar *name;
		xmms_configparser_state_t state;
	} *ptr, lookup[] = {
		{"xmms", XMMS_CONFIG_STATE_START},
		{"section", XMMS_CONFIG_STATE_SECTION},
		{"property", XMMS_CONFIG_STATE_PROPERTY},
		{NULL, XMMS_CONFIG_STATE_INVALID}
	};

	for (ptr = lookup; ptr && ptr->name; ptr++) {
		if (!strcmp (ptr->name, name)) {
			return ptr->state;
		}
	}

	return XMMS_CONFIG_STATE_INVALID;
}

/**
 * @internal Look for the value associated with an attribute name, given lists
 * of attribute names and attribute values.
 * @param[in] names List of attribute names
 * @param[in] values List of attribute values matching up to names
 * @param[in] needle Attribute name to look for
 * @return The attribute value, or NULL if not found
 */
static const gchar *
lookup_attribute (const gchar **names, const gchar **values,
                  const gchar *needle)
{
	const gchar **n, **v;

	for (n = names, v = values; *n && *v; n++, v++) {
		if (!strcmp ((gchar *) *n, needle)) {
			return *v;
		}
	}

	return NULL;
}

/**
 * @internal Parse start tag in config file. This function is called whenever
 * a start tag is encountered by the GMarkupParser from #xmms_config_init
 * @param ctx The parser context.
 * @param name The name of the element encountered
 * @param attr_name List of attribute names in tag
 * @param attr_data List of attribute data in tag
 * @param userdata User data - In this case, the global config
 * @param error GError to be filled in if an error is encountered
 */
static void
xmms_config_parse_start (GMarkupParseContext *ctx,
                         const gchar *name,
                         const gchar **attr_name,
                         const gchar **attr_data,
                         gpointer userdata,
                         GError **error)
{
	xmms_config_t *config = userdata;
	xmms_configparser_state_t state;
	const gchar *attr;
	const gchar *type_attr;

	state = get_current_state (name);
	g_queue_push_head (config->states, GINT_TO_POINTER (state));

	switch (state) {
		case XMMS_CONFIG_STATE_INVALID:
			*error = g_error_new (G_MARKUP_ERROR,
			                      G_MARKUP_ERROR_UNKNOWN_ELEMENT,
			                      "Unknown element '%s'", name);
			return;
		case XMMS_CONFIG_STATE_START:
			/* check config version here */
			attr = lookup_attribute (attr_name, attr_data, "version");
			if (attr) {
				if (strcmp (attr, "0.02") == 0) {
					config->version = 2;
				} else {
					config->version = atoi (attr);
				}
			}
			return;
		default:
			break;
	}

	attr = lookup_attribute (attr_name, attr_data, "name");
	if (!attr) {
		*error = g_error_new (G_MARKUP_ERROR,
		                      G_MARKUP_ERROR_INVALID_CONTENT,
		                      "Attribute 'name' missing");
		return;
	}
	type_attr = lookup_attribute (attr_name, attr_data, "type");

	switch (state) {
		case XMMS_CONFIG_STATE_SECTION:
			g_queue_push_head (config->sections, g_strdup (attr));

			break;
		case XMMS_CONFIG_STATE_PROPERTY:
			g_free (config->value_name);
			g_free (config->value_type);
			config->value_name = g_strdup (attr);
			if (type_attr) {
				config->value_type = g_strdup (type_attr);
			} else {
				config->value_type = g_strdup ("string");
			}

			break;
		default:
			break;
	}
}

/**
 * @internal Parse end tag in config file. This function is called whenever
 * an end tag is encountered by the GMarkupParser from #xmms_config_init
 * @param ctx The parser context.
 * @param name The name of the element encountered
 * @param userdata User data - In this case, the global config
 * @param error GError to be filled in if an error is encountered
 */
static void
xmms_config_parse_end (GMarkupParseContext *ctx,
                       const gchar *name,
                       gpointer userdata,
                       GError **error)
{
	xmms_config_t *config = userdata;
	xmms_configparser_state_t state;

	state = GPOINTER_TO_INT (g_queue_pop_head (config->states));

	switch (state) {
		case XMMS_CONFIG_STATE_SECTION:
			g_free (g_queue_pop_head (config->sections));

			break;
		case XMMS_CONFIG_STATE_PROPERTY:
			g_free (config->value_name);
			g_free (config->value_type);
			config->value_name = NULL;
			config->value_type = NULL;

			break;
		default:
			break;
	}
}

/**
 * @internal Parse text in config file. This function is called whenever
 * text (anything between start and end tags) is encountered by the
 * GMarkupParser from #xmms_config_init
 * @param ctx The parser context.
 * @param text The text
 * @param text_len Length of the text
 * @param userdata User data - In this case, the global config
 * @param error GError to be filled in if an error is encountered
 */
static void
xmms_config_parse_text (GMarkupParseContext *ctx,
                        const gchar *text,
                        gsize text_len,
                        gpointer userdata,
                        GError **error)
{
	xmms_config_t *config = userdata;
	xmmsv_t *value;
	xmms_configparser_state_t state;
	GList *l;
	gchar key[256] = "";
	gsize siz = sizeof (key);

	state = GPOINTER_TO_INT (g_queue_peek_head (config->states));

	if (state != XMMS_CONFIG_STATE_PROPERTY)
		return;

	/* assemble the config key, based on the traversed sections */
	for (l = config->sections->tail; l; l = l->prev) {
		g_strlcat (key, l->data, siz);
		g_strlcat (key, ".", siz);
	}

	g_strlcat (key, config->value_name, siz);

	if (strcmp (config->value_type, "int32") == 0) {
		value = xmmsv_new_int (atoi (text));
	} else if (strcmp (config->value_type, "float") == 0) {
		value = xmmsv_new_float (strtod (text, NULL));
	} else if (strcmp (config->value_type, "string") == 0) {
		value = xmmsv_new_string (text);
	} else {
		value = xmmsv_new_string (text);
	}
	xmms_config_set (config, key, value);
	xmmsv_unref (value);
}

/**
 * @internal Set a key to a new value
 * @param conf The config
 * @param key The key to look for
 * @param value The value to set the key to
 * @param err To be filled in if an error occurs
 */
static void
xmms_config_client_set_value (xmms_config_t *conf,
                              const gchar *key, const gchar *value,
                              xmms_error_t *err)
{
	xmmsv_t *prop_value, *member, *new_list;
	gchar **values;
	guint num;
	gint32 i;
	gfloat f;

	prop_value = xmms_config_path_data (conf, key);
	if (prop_value) {
		switch (xmmsv_get_type (prop_value)) {
		case XMMSV_TYPE_LIST:
			values = g_strsplit (value, ",", 0);
			num = g_strv_length (values);
			new_list = xmmsv_new_list ();
			for (i = 0; i < num; i++) {
				if (xmmsv_list_get (prop_value, i, &member)) {
					switch (xmmsv_get_type (member)) {
					case XMMSV_TYPE_INT32:
						i = atoi (value);
						xmmsv_list_append (new_list, xmmsv_new_int (i));
						break;
					case XMMSV_TYPE_FLOAT:
						f = atof (value);
						xmmsv_list_append (new_list, xmmsv_new_float (f));
						break;
					case XMMSV_TYPE_STRING:
						xmmsv_list_append (new_list, xmmsv_new_string (values[i]));
						break;
					default:
						/* keep the rest as they are */
						/* TODO give the client a warning? */
						xmmsv_list_append (new_list, member);
					}
				}
				xmms_config_set (conf, key, new_list);
			}
			break;
		case XMMSV_TYPE_INT32:
			i = atoi (value);
			xmms_config_set_int (conf, key, i);
			break;
		case XMMSV_TYPE_FLOAT:
			f = atof (value);
			xmms_config_set_float (conf, key, f);
			break;
		case XMMSV_TYPE_STRING:
			xmms_config_set_string (conf, key, value);
			break;
		default:
			xmms_error_set (err, XMMS_ERROR_NOENT,
			                "Trying to set invalid type");
		}
	} else {
		xmms_error_set (err, XMMS_ERROR_NOENT,
		                "Trying to set non-existent config property");
	}
	g_strfreev (values);
}

gboolean
fill_tree_from_values (xmmsv_t *parent_node, const gchar *prefix, GTree *tree)
{
	xmmsv_t *value;
	const gchar *key;
	gchar new_prefix[128];
	gchar *new_key;
	gint i;
	xmmsv_list_iter_t *itl;
	xmmsv_dict_iter_t *itd;

	switch (xmmsv_get_type (parent_node)) {
	case XMMSV_TYPE_LIST:
		g_return_val_if_fail (xmmsv_get_list_iter (parent_node, &itl), FALSE);
		i = 0;
		while (xmmsv_list_iter_valid (itl)) {
			xmmsv_list_iter_entry (itl, &value);
			sprintf (new_prefix, "%s.%i", prefix, i);
			fill_tree_from_values (value, new_prefix, tree);
			xmmsv_list_iter_next (itl);
			i++;
		}
		xmmsv_list_iter_explicit_destroy (itl);
		break;
	case XMMSV_TYPE_DICT:
		g_return_val_if_fail (xmmsv_get_dict_iter (parent_node, &itd), FALSE);
		while (xmmsv_dict_iter_valid (itd)) {
			xmmsv_dict_iter_pair (itd, &key, &value);
			if (strlen (prefix) > 0) {
				sprintf (new_prefix, "%s.%s", prefix, key);
			} else { /* root node is always a dict */
				strcpy (new_prefix, key);
			}
			fill_tree_from_values (value, new_prefix, tree);
			xmmsv_dict_iter_next (itd);
		}
		xmmsv_dict_iter_explicit_destroy (itd);
		break;
	case XMMSV_TYPE_INT32:
	case XMMSV_TYPE_FLOAT:
	case XMMSV_TYPE_STRING:
		new_key = g_strdup (prefix);
		g_tree_insert (tree, new_key, parent_node);
		break;
	default:
		xmms_log_error ("Invalid type in config");
		break;
	}
	return TRUE;
}

/**
 * @internal List all keys and values in the config.
 * @param conf The config
 * @param err To be filled in if an error occurs
 * @return a dict with config properties and values
 */
static GTree *
xmms_config_client_list_values (xmms_config_t *config, xmms_error_t *err)
{
	GTree *ret;

	ret = g_tree_new_full (compare_key, NULL,
	                       g_free, NULL);

	g_mutex_lock (config->mutex);
	fill_tree_from_values (config->properties_list, "", ret);
	g_mutex_unlock (config->mutex);

	return ret;
}

/**
 * @internal Look for a key in the config and return its value as a string
 * @param conf The config
 * @param key The key to look for
 * @param err To be filled in if an error occurs
 * @return The value of the key, or NULL if not found
 */
static gchar *
xmms_config_client_get_value (xmms_config_t *conf, const gchar *key,
                              xmms_error_t *err)
{
	return g_strdup (xmms_config_property_lookup_get_string (conf, key, err));
}

/**
 * @internal Destroy a config object
 * @param object The object to destroy
 */
static void
xmms_config_destroy (xmms_object_t *object)
{
	xmms_config_t *config = (xmms_config_t *)object;

	g_mutex_free (config->mutex);

	xmmsv_unref (config->properties_list);
	g_tree_unref (config->schemas);
	global_config = NULL;

	xmms_config_unregister_ipc_commands ();
}

static gint
compare_key (gconstpointer a, gconstpointer b, gpointer user_data)
{
	return strcmp ((gchar *) a, (gchar *) b);
}

/**
 * @internal Clear data in a config object
 * @param config The config object to clear
 */
static void
clear_config (xmms_config_t *config)
{
	xmmsv_unref (config->properties_list);
	config->properties_list = xmmsv_new_dict ();

	config->version = XMMS_CONFIG_VERSION;

	g_free (config->value_name);
	g_free (config->value_type);
	config->value_name = NULL;
	config->value_type = NULL;
}

xmms_config_t *
xmms_config_new (const gchar *filename)
{
	if (!global_config) {
		xmms_config_init (filename);
	} else {
		xmms_log_error ("Config singleton exists.");
	}
	return global_config;
}

/**
 * @internal Initialize and parse the config file. Resets to default config
 * on parse error.
 * @param[in] filename The absolute path to a config file as a string.
 */
void
xmms_config_init (const gchar *filename)
{
	GMarkupParser pars;
	GMarkupParseContext *ctx;
	xmms_config_t *config;
	int ret, fd = -1;
	gboolean parserr = FALSE, eof = FALSE;

	config = xmms_object_new (xmms_config_t, xmms_config_destroy);
	config->mutex = g_mutex_new ();
	config->filename = filename;

	config->properties_list = xmmsv_new_dict ();
	config->schemas = g_tree_new_full (compare_key, NULL,
	                                   g_free, (GDestroyNotify) xmmsv_unref);

	config->version = 0;
	global_config = config;

	xmms_config_register_ipc_commands (XMMS_OBJECT (config));

	memset (&pars, 0, sizeof (pars));

	pars.start_element = xmms_config_parse_start;
	pars.end_element = xmms_config_parse_end;
	pars.text = xmms_config_parse_text;

	if (g_file_test (filename, G_FILE_TEST_EXISTS)) {
		fd = open (filename, O_RDONLY);
	}

	if (fd > -1) {
		config->is_parsing = TRUE;
		config->states = g_queue_new ();
		config->sections = g_queue_new ();
		ctx = g_markup_parse_context_new (&pars, 0, config, NULL);

		while ((!eof) && (!parserr)) {
			GError *error = NULL;
			gchar buffer[1024];

			ret = read (fd, buffer, 1024);
			if (ret < 1) {
				g_markup_parse_context_end_parse (ctx, &error);
				if (error) {
					xmms_log_error ("Cannot parse config file: %s",
					                error->message);
					g_error_free (error);
					error = NULL;
					parserr = TRUE;
				}
				eof = TRUE;
			}

			g_markup_parse_context_parse (ctx, buffer, ret, &error);
			if (error) {
				xmms_log_error ("Cannot parse config file: %s",
				                error->message);
				g_error_free (error);
				error = NULL;
				parserr = TRUE;
			}
			/* check config file version, assumes that g_markup_context_parse
			 * above managed to parse the <xmms> element during the first
			 * iteration of this loop */
			if (XMMS_CONFIG_VERSION > config->version) {
				clear_config (config);
				break;
			}
		}

		close (fd);
		g_markup_parse_context_free (ctx);

		while (!g_queue_is_empty (config->sections)) {
			g_free (g_queue_pop_head (config->sections));
		}

		g_queue_free (config->states);
		g_queue_free (config->sections);

		config->is_parsing = FALSE;
	} else {
		xmms_log_info ("No configfile specified, using default values.");
	}

	if (parserr) {
		xmms_log_info ("The config file could not be parsed, reverting to default configuration..");
		clear_config (config);
	}
}

/**
 * @internal Shut down the config layer - free memory from the global
 * configuration.
 */
void
xmms_config_shutdown ()
{
	xmms_object_unref (global_config);

}

static void
put_value_in_data (xmmsv_t *node, const gchar* prop_name, dump_tree_data_t *data)
{
	gint32 int_val;
	gfloat f;
	const gchar *s;
	gchar value_str[128];

	switch (xmmsv_get_type (node)) {
	case XMMSV_TYPE_INT32:
		xmmsv_get_int (node, &int_val);
		sprintf (value_str, "%i", int_val);
		fprintf (data->fp, "%s<property name=\"%s\" type=\"int32\">%s</property>\n",
		         data->indent, prop_name, value_str);
		break;
	case XMMSV_TYPE_FLOAT:
		xmmsv_get_float (node, &f);
		sprintf (value_str, "%f", f);
		fprintf (data->fp, "%s<property name=\"%s\" type=\"float\">%s</property>\n",
		         data->indent, prop_name, value_str);
		break;
	case XMMSV_TYPE_STRING:
		xmmsv_get_string (node, &s);
		strcpy (value_str, s);
		fprintf (data->fp, "%s<property name=\"%s\" type=\"string\">%s</property>\n",
		         data->indent, prop_name, value_str);
		break;
	case XMMSV_TYPE_LIST:
	case XMMSV_TYPE_DICT:
		fprintf (data->fp, "%s<section name=\"%s\">\n",
		         data->indent, prop_name);
		/* increase indent level */
		g_assert (data->indent_level < 127);
		data->indent[data->indent_level] = '\t';
		data->indent[++data->indent_level] = '\0';

		dump_node_tree (node, data);
		/* close section */
		data->indent[--data->indent_level] = '\0';

		fprintf (data->fp, "%s</section>\n", data->indent);
		break;
	default:
		/* do nothing with other types. we shouldn't get here anyway */
		break;
	}
}

static gboolean
dump_node_tree (xmmsv_t *node, dump_tree_data_t *data)
{
	gchar prop_name[32];
	const gchar *key;

	g_assert (xmmsv_is_type (node, XMMSV_TYPE_LIST)
	          || xmmsv_is_type (node, XMMSV_TYPE_DICT));

	if (xmmsv_is_type (node, XMMSV_TYPE_LIST)) {
		gint i;
		xmmsv_list_iter_t *it;
		g_return_val_if_fail (xmmsv_get_list_iter (node, &it), FALSE);
		i = 0;

		while (xmmsv_list_iter_valid (it)) {
			xmmsv_t *value;
			sprintf (prop_name, "%i", i);
			xmmsv_list_iter_entry (it, &value);
			put_value_in_data (value, prop_name, data);
			i++;
			xmmsv_list_iter_next (it);
		}
		xmmsv_list_iter_explicit_destroy (it);

	} else { /* is a dict */
		xmmsv_dict_iter_t *it;
		g_return_val_if_fail (xmmsv_get_dict_iter (node, &it), FALSE);
		while (xmmsv_dict_iter_valid (it)) {
			xmmsv_t *value;
			xmmsv_dict_iter_pair (it, &key, &value);
			put_value_in_data (value, key, data);
			xmmsv_dict_iter_next (it);
		}
		xmmsv_dict_iter_explicit_destroy (it);
	}

	return TRUE;
}

/**
 * @internal Save the global configuration to disk.
 * @param file Absolute path to configfile. This will be overwritten.
 * @return TRUE on success.
 */
gboolean
xmms_config_save (void)
{
	FILE *fp = NULL;
	dump_tree_data_t data;

	xmms_config_t *config = global_config;

	g_return_val_if_fail (config, FALSE);

	/* don't try to save config while it's being read */
	if (config->is_parsing)
		return FALSE;

	if (!(fp = fopen (config->filename, "w"))) {
		xmms_log_error ("Couldn't open %s for writing.",
		                config->filename);
		return FALSE;
	}

	fprintf (fp, "<?xml version=\"1.0\"?>\n<xmms version=\"%i\">\n",
	         XMMS_CONFIG_VERSION);

	data.fp = fp;
	data.state = XMMS_CONFIG_STATE_START;
	data.prev_key = NULL;

	strcpy (data.indent, "\t");
	data.indent_level = 1;

	dump_node_tree (config->properties_list, &data);

	/* close the remaining section tags. the final indent level
	 * was started with the opening xmms tag, so the loop condition
	 * is '> 1' here rather than '> 0'.
	 */
	while (data.indent_level > 1) {
		/* decrease indent level */
		data.indent[--data.indent_level] = '\0';

		fprintf (fp, "%s</section>\n", data.indent);
	}

	fprintf (fp, "</xmms>\n");
	fclose (fp);

	return TRUE;
}

/*
 * Value manipulation
 */

/**
 * @internal Destroy a config value
 * @param object The object to destroy
 */
static void
xmms_config_property_destroy (xmms_object_t *object)
{
	xmms_config_property_t *prop = (xmms_config_property_t *) object;

	g_free (prop->name);
}

/**
 * @internal Register a client config value
 * @param config The config
 * @param name The name of the config value
 * @param def_value The default value to use
 * @param error To be filled in if an error occurs
 * @return The full path to the config value registered
 */
static gchar *
xmms_config_client_register_value (xmms_config_t *config,
                                   const gchar *name,
                                   const gchar *def_value,
                                   xmms_error_t *error)
{
	gchar *tmp;
	tmp = g_strdup_printf ("clients.%s", name);
	xmms_config_property_register (tmp, def_value, NULL, NULL);
	return tmp;
}

xmmsv_t *
xmms_config_path_data (xmms_config_t *config, const gchar *path)
{
	xmmsv_t *out_val = NULL, *val1, *val2;
	gchar **node_names;
	gint i;
	gboolean found;

	out_val = NULL;
	val1 = config->properties_list;
	g_return_val_if_fail (path, NULL);

	node_names = g_strsplit (path, ".", 32);

	if (node_names == NULL) {
		return NULL;
	}
	if (node_names[0] == NULL) {
		g_strfreev (node_names);
		return config->properties_list;
	}
	i = 0;
	found = TRUE;

	while (node_names[i]) {
		if (xmmsv_is_type (val1, XMMSV_TYPE_DICT)) {
			if (!xmmsv_dict_get (val1, node_names[i], &val2)) {
				found = FALSE;
				break; /* key not found! */
			}
		} else if (xmmsv_is_type (val2, XMMSV_TYPE_LIST)) {
			int index = strtol (node_names[i], NULL, 10);
			if (index == 0 && g_strcmp0 (node_names[i], "0") != 0) {
				found = FALSE;
				break;
			}
			if (!xmmsv_list_get (val1, index, &val2)) {
				found = FALSE;
				break;
			}
		} else { /* node is value */
			break;
		}
		val1 = val2;
		i++;
	}

	if (found && node_names[i] == NULL) { /* property matched */
		out_val = val1;
	}
	g_strfreev (node_names);
	return out_val;
}

xmmsv_t *
xmms_config_path_parent (xmms_config_t *config, const gchar *path)
{
	xmmsv_t *parent, *val1, *val2;
	gchar **node_names;
	gint i;
	guint len;
	gboolean found;

	parent = NULL;
	val1 = config->properties_list;

	node_names = g_strsplit (path, ".", -1);
	len = g_strv_length (node_names);

	if (node_names == NULL) {
		return NULL;
	}
	i = 0;
	found = TRUE;
	/* no locking here as this function should be inside a lock already */
	while (node_names[i]) {
		if (xmmsv_is_type (val1, XMMSV_TYPE_DICT)) {
			if (!xmmsv_dict_get (val1, node_names[i], &val2)) {
				found = FALSE;
				break; /* key not found! */
			}
		} else if (xmmsv_is_type (val2, XMMSV_TYPE_LIST)) {
			int index = strtol (node_names[i], NULL, 10);
			if (index == 0 && g_strcmp0 (node_names[i], "0") != 0) {
				found = FALSE;
				break;
			}
			if (!xmmsv_list_get (val1, index, &val2)) {
				found = FALSE;
				break;
			}
		} else { /* node is value */
			i++;
			break;
		}
		parent = val1;
		val1 = val2;
		i++;
	}

	if (!found || node_names[i] != NULL) { /* property not matched */
		parent = NULL;
	}
	if (i == 0 && len == 1) {
		parent = config->properties_list; /* should append to root node */
	}
	g_strfreev (node_names);
	return parent;
}

gboolean
value_is_consistent (xmmsv_t *config_value, xmmsv_t *value)
{
	g_assert (xmmsv_is_type (value, XMMSV_TYPE_INT32) || xmmsv_is_type (value, XMMSV_TYPE_FLOAT)
	          || xmmsv_is_type (value, XMMSV_TYPE_STRING) || xmmsv_is_type (value, XMMSV_TYPE_LIST)
	          || xmmsv_is_type (value, XMMSV_TYPE_DICT) || xmmsv_is_type (value, XMMSV_TYPE_NONE));
	g_assert (xmmsv_is_type (config_value, XMMSV_TYPE_INT32) || xmmsv_is_type (config_value, XMMSV_TYPE_FLOAT)
	          || xmmsv_is_type (config_value, XMMSV_TYPE_STRING) || xmmsv_is_type (config_value, XMMSV_TYPE_LIST)
	          || xmmsv_is_type (config_value, XMMSV_TYPE_DICT) || xmmsv_is_type (config_value, XMMSV_TYPE_NONE));
	if (xmmsv_is_type (value, XMMSV_TYPE_NONE)
	        || xmmsv_is_type (config_value, XMMSV_TYPE_NONE)) {
		return TRUE; /* none type is used to clear node types */
	}
	if (xmmsv_get_type (config_value) != xmmsv_get_type (value)) {
		return FALSE;
	}
	if (xmmsv_is_type (config_value, XMMSV_TYPE_LIST)) {
		return list_is_consistent (config_value, value);
	} else if (xmmsv_is_type (config_value, XMMSV_TYPE_DICT)) {
		return dict_is_consistent (config_value, value);
	}
	return TRUE;
}

gboolean
list_is_consistent (xmmsv_t *config_value, xmmsv_t *value)
{
	xmmsv_list_iter_t *it1, *it2;
	xmmsv_t *v1, *v2;

	g_assert (config_value && value);

	if (!xmmsv_is_type (config_value, XMMSV_TYPE_LIST)
	        || !xmmsv_is_type (value, XMMSV_TYPE_LIST)) {
		return FALSE;
	}
	xmmsv_get_list_iter (config_value, &it1);
	xmmsv_get_list_iter (value, &it2);
	while (xmmsv_list_iter_valid (it1) && xmmsv_list_iter_valid (it2)) {
		xmmsv_list_iter_entry (it1, &v1);
		xmmsv_list_iter_entry (it2, &v2);
		if (!value_is_consistent (v1,v2)) {
			xmmsv_list_iter_explicit_destroy (it1);
			xmmsv_list_iter_explicit_destroy (it2);
			return FALSE;
		}
		xmmsv_list_iter_next (it1);
		xmmsv_list_iter_next (it2);
	}
	if (it1) {
		xmmsv_list_iter_explicit_destroy (it1);
	}
	if (it2) {
		xmmsv_list_iter_explicit_destroy (it2);
	}
	return TRUE;
}

gboolean
dict_is_consistent (xmmsv_t *config_value, xmmsv_t *value)
{
	xmmsv_dict_iter_t *it1, *it2;
	xmmsv_t *v1, *v2;
	const char *key1, *key2;

	if (!xmmsv_is_type (config_value, XMMSV_TYPE_DICT)
	        || !xmmsv_is_type (value, XMMSV_TYPE_DICT)) {
		return FALSE;
	}
	xmmsv_get_dict_iter (config_value, &it1);
	xmmsv_get_dict_iter (value, &it2);
	while (xmmsv_dict_iter_valid (it1) && xmmsv_dict_iter_valid (it2)) {
		xmmsv_dict_iter_pair (it1, &key1, &v1);
		xmmsv_dict_iter_pair (it2, &key2, &v2);
		if (!value_is_consistent (v1,v2) ||
		        strcmp (key1, key2) != 0) { /* TODO don't enforce order of keys */
			xmmsv_dict_iter_explicit_destroy (it1);
			xmmsv_dict_iter_explicit_destroy (it2);
			return FALSE;
		}
		xmmsv_dict_iter_next (it1);
		xmmsv_dict_iter_next (it2);
	}
	if (it1) {
		xmmsv_dict_iter_explicit_destroy (it1);
	}
	if (it2) {
		xmmsv_dict_iter_explicit_destroy (it2);
	}
	return TRUE;
}

gboolean
config_register_path (xmms_config_t *config, const gchar *path)
{
	gchar **parts;
	gchar current_path[128];
	gint len = sizeof (current_path);
	gboolean ret;
	gint i, numparts;
	xmmsv_t *parent_value, *child_value;

	parts = g_strsplit (path, ".", -1);
	strcpy (current_path, "");
	numparts = g_strv_length (parts);
	ret = TRUE;
	parent_value = xmms_config_path_parent (config, parts[0]);
	g_strlcat (current_path, parts[0], len);

	for (i = 0; i < numparts; i++) {
		child_value = NULL;
		if (xmmsv_is_type (parent_value, XMMSV_TYPE_LIST)) {
			gint index = strtol (parts[i], NULL, 10);
			xmmsv_list_get (parent_value, index, &child_value);
		} else if (xmmsv_is_type (parent_value, XMMSV_TYPE_DICT)) {
			xmmsv_dict_get (parent_value, parts[i], &child_value);
		}
		/* check if consistent */
		if (!child_value) {
			if (i < numparts - 1) {
				if (is_digit (parts[i + 1])) {
					child_value = xmmsv_new_list ();
				} else {
					child_value = xmmsv_new_dict ();
				}
			} else { /* last node in path */
				child_value = xmmsv_new_none ();
				if (i != numparts - 1) {
					ret = FALSE;
					break; /* would be nice to clean up created paths so far */
				}
			}
			if (xmmsv_is_type (parent_value, XMMSV_TYPE_LIST)) {
				gint index = strtol (parts[i], NULL, 10);
				while (xmmsv_list_get_size (parent_value) <= index) { /* fill up list to necessary size with copies of the same */
					xmmsv_t *new_item = xmmsv_copy (child_value);
					xmmsv_list_append (parent_value, new_item);
					xmmsv_unref (new_item);
				}
				ret = xmmsv_list_set (parent_value, index, child_value);
				xmmsv_unref (child_value);
			} else if (xmmsv_is_type (parent_value, XMMSV_TYPE_DICT)) {
				ret = xmmsv_dict_set (parent_value, parts[i], child_value);
				xmmsv_unref (child_value);
			}
		}
		parent_value = child_value;
		if (i < numparts - 1) {
			g_strlcat (current_path, ".", len);
			g_strlcat (current_path, parts[i + 1], len);
		}
	}
	g_strfreev (parts);
	return ret;
}

gboolean
is_digit (const gchar *s)
{
	gint value;
	gboolean ret;
	gchar s2[16];

	value = strtol (s, NULL, 10);
	sprintf (s2, "%i", value);

	ret = strcmp (s, s2) == 0 ? TRUE : FALSE;

	return ret;
}
/** @} */
