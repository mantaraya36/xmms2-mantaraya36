/** @file ladspa_plugin.c
 *  LADSPA Hosting plugin
 *
 *  Copyright (C) 2011 XMMS2 Team
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <gmodule.h>


#include "xmms/xmms_log.h"
#include "ladspa_plugin.h"


static LADSPA_Descriptor_Function ladspa_plugin_get_descriptor_function (const gchar* pluginlib);
static const LADSPA_Descriptor *ladspa_plugin_get_descriptor (gchar *pluginlib,
                                                              gchar *pluginname);
gint ladspa_plugin_num_ports (const LADSPA_Descriptor *descriptor,
                              LADSPA_PortDescriptor port_type);

void allocate_bufs (ladspa_plugin_node_t *node, guint buf_size);

static LADSPA_Descriptor_Function
ladspa_plugin_get_descriptor_function (const gchar *pluginlib)
{
	GModule *dl;
	LADSPA_Descriptor_Function desc;
	gboolean exists = FALSE;
	gchar *libname = NULL;
	const char * LADSPA_path;
	gchar ** paths, **p;
	gchar default_path[] = LADSPA_DEFAULT_PATH;

	if (strlen (pluginlib) == 0) {
		return NULL;
	}
	if (pluginlib[0] == '/') { /* absolute path */
		libname = g_strdup (pluginlib);
	} else { /* relative path */
		LADSPA_path = g_getenv ("LADSPA_PATH");
		if (!LADSPA_path || strlen (LADSPA_path) == 0) {
			paths = g_new0 (gchar *, 2);
			paths[0] = g_new0 (gchar, (strlen (default_path) + 1) );
			paths[1] = NULL;
			strncpy (paths[0], default_path, strlen (default_path));
		} else {
			gchar separator[] = ":";
			paths = g_strsplit (LADSPA_path, separator, 16); /* 16 paths should be plenty */
		}
		p = paths;
		while (p != NULL && *p != NULL) {
			if (strlen (pluginlib) + strlen (*p) > 255) { /* count possible additional separator */
				xmms_log_error ("Full library path too long (max 256)");
				return NULL;
			}
			if (pluginlib[strlen (pluginlib) -1] == '/') {
				libname = g_strconcat (*p, pluginlib, NULL);
			} else {
				libname = g_strconcat (*p, "/", pluginlib, NULL);
			}
			exists = g_file_test (libname,G_FILE_TEST_EXISTS);
			if (exists == TRUE) {
				break;
			}
			g_free (libname);
			libname = NULL;
			p++;
		}
		g_strfreev (paths);
	}
	if (!libname) {
		xmms_log_error ("Couldn't find library %s", pluginlib);
		return NULL;
	}
	dl = g_module_open (libname, G_MODULE_BIND_LAZY);
	g_free (libname);
	if (dl == NULL) {
		xmms_log_error ("Couldn't open library %s", pluginlib);
		return NULL;
	}
	g_module_symbol (dl, "ladspa_descriptor", (gpointer *)&desc);
	return desc;
}


static const LADSPA_Descriptor *
ladspa_plugin_get_descriptor (gchar *pluginlib, gchar *pluginname)
{
	LADSPA_Descriptor_Function descriptor_function;
	const LADSPA_Descriptor *descriptor;
	gint index, plugin_count;
	descriptor_function = ladspa_plugin_get_descriptor_function (pluginlib);
	if (descriptor_function == NULL) {
		xmms_log_error ("Could not find descriptor function in library %s", pluginlib);
		return NULL;
	}
	/* If only a single plugin in a library, load it always and warn */
	plugin_count = 0;
	for (index = 0;; index++) {
		descriptor = descriptor_function (index);
		if (descriptor != NULL) {
			plugin_count++;
		} else {
			break;
		}
	}
	if (plugin_count == 1 || strlen (pluginname) == 0) {
		descriptor = descriptor_function (0);
		/* xmms_log_info ("Auto-selected plugin %s", descriptor->Label); */
	} else {
		for (index = 0;; index++) {
			descriptor = descriptor_function (index);
			if (descriptor == NULL) {
				return NULL;
			}
			if (g_strcmp0 (descriptor->Label,pluginname) == 0
			    || g_strcmp0 (descriptor->Name,pluginname) == 0
			    || descriptor->UniqueID == strtoul (pluginname, 0, 10)) {
				break; /* allow matching either Name, Label or UniqueID for convenience */
			}
		}
	}
	return descriptor;
}

ladspa_plugin_node_t *
ladspa_plugin_new_node (const gchar *plugin, gint num_channels, guint buf_size, gint srate)
{
	guint out_count, in_count, ctl_in_port_count;
	ladspa_plugin_node_t *node;
	const LADSPA_Descriptor *descriptor = NULL;
	gchar *pluginlib, *pluginname;
	gchar **plugin_parts;
	guint i;

	if (strlen (plugin) != 0) {
		plugin_parts =  g_strsplit (plugin, ":" , 2);
		pluginlib = plugin_parts[0];
		pluginname = plugin_parts[1];
		if (!pluginname) {
			pluginname = g_new0 (gchar, 1);
		}
		descriptor = ladspa_plugin_get_descriptor (pluginlib, pluginname);
		if (!descriptor) {
			xmms_log_error ("Could not find plugin %s in %s", pluginname,
			                pluginlib);
		}
	} else {
		XMMS_DBG ("Creating empty ladspa node.");
	}

	node = g_new0 (ladspa_plugin_node_t, 1);
	node->next = NULL;
	node->plugin = descriptor;
	node->mode = LADSPA_NONE; /* default to off */
	node->pluginstring = g_strdup (plugin);

	if (!descriptor) {
		return node;
	}
	/* Examine port counts */
	ctl_in_port_count
		= ladspa_plugin_num_ports (descriptor,
		                           LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL);
	in_count
		= ladspa_plugin_num_ports (descriptor,
		                           LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO);
	out_count
		= ladspa_plugin_num_ports (descriptor,
		                           LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO);

	node->num_in_channels = in_count;
	node->num_out_channels = out_count;

	if (out_count == 0) {
		xmms_log_error ("Plugin has no output channels. Aborted.");
		return node;
	}
	/* Instantiate depending on the number of I/O channels */
	if (in_count == out_count && out_count == num_channels) {
		node->instance = g_new0 (LADSPA_Handle, 1);
		node->instance[0] = descriptor->instantiate (descriptor, srate);
		node->num_instances = 1;
		node->mode = LADSPA_DIRECT; /* LADSPA_DIRECT */
	} else if (in_count == 1 && out_count == 1) {
		node->instance = g_new0 (LADSPA_Handle, num_channels);
		node->num_instances = num_channels;
		node->mode = LADSPA_MONO; /* LADSPA_MONO */
		for (i = 0; i < num_channels; i++) {
			node->instance[i] = descriptor->instantiate (descriptor, srate);
		}
	} else if (in_count == 1 && out_count == 2 && num_channels == 2) {
		node->instance = g_new0 (LADSPA_Handle, 2);;
		node->num_instances = num_channels;
		node->mode = LADSPA_BALANCE; /* LADSPA_BALANCE */
		for (i = 0; i < num_channels; i++) {
			node->instance[i] = descriptor->instantiate (descriptor, srate);
		}
	} else { /* LADSPA_OTHER */
		gint nin, nout, n;
		/* determine the minimum number of instances needed */
		nin = (num_channels > in_count ?
		       ceil (num_channels/in_count) : 1 );
		nout = (num_channels > out_count ?
		       ceil (num_channels/out_count) : 1 );
		n = (nin > nout ? nin : nout);
		node->num_instances = n;
		node->mode = LADSPA_OTHER;
		node->instance = g_new0 (LADSPA_Handle, n);
		for (i = 0; i < n; i++) {
			node->instance[i] = descriptor->instantiate (descriptor, srate);
		}
	}
	node->num_ctl_in_ports = ctl_in_port_count;
	allocate_bufs (node, buf_size);
	if (pluginname) {
		g_free (pluginname);
	}
	g_strfreev (plugin_parts);
	return node;
}

void
allocate_bufs (ladspa_plugin_node_t *node, guint buf_size)
{
	gint i;

	node->ctl_in_ports = g_new0 (LADSPA_Data *, node->num_ctl_in_ports);
	for (i = 0; i < node->num_ctl_in_ports; i++) {
		node->ctl_in_ports[i] = g_new0 (LADSPA_Data, buf_size);
	}
	if (node->mode == LADSPA_BALANCE || node->mode == LADSPA_OTHER) {
		node->temp_bufs = g_new0 (gfloat *, node->num_out_channels);
		for (i = 0; i < node->num_out_channels; i++) {
			node->temp_bufs[i] = g_new0 (gfloat, buf_size);
		}
	}
}


void
ladspa_plugin_free_node (ladspa_plugin_node_t *plugin_node)
{
	gint i;
	gint num_temp_bufs = 0;

	for (i = 0; i < plugin_node->num_instances; i++) {
		if (plugin_node->plugin->deactivate) {
			plugin_node->plugin->deactivate (plugin_node->instance[i]);
		}
		plugin_node->plugin->cleanup (plugin_node->instance[i]);
	}
	if (plugin_node->mode == LADSPA_BALANCE
	    || plugin_node->mode == LADSPA_OTHER) {
		if (plugin_node->temp_bufs) {
			for (i = 0; i < num_temp_bufs; i++) {
				g_free (plugin_node->temp_bufs[i]);
			}
			g_free (plugin_node->temp_bufs);
			plugin_node->temp_bufs = NULL;
		}
	}
	for (i = 0; i < plugin_node->num_ctl_in_ports; i++) {
		g_free (plugin_node->ctl_in_ports[i]);
	}
	g_free (plugin_node->ctl_in_ports);
	g_free (plugin_node->pluginstring);

	plugin_node->ctl_in_ports = NULL;
	g_free (plugin_node);
}

LADSPA_Data
ladspa_plugin_get_default_value (const LADSPA_PortRangeHint * port_range_hints, gint port_num)
{
	LADSPA_Data lower, upper, default_value;
	LADSPA_PortRangeHintDescriptor range_hints;

	range_hints = port_range_hints[port_num].HintDescriptor;
	default_value = 0.0;

	if (!LADSPA_IS_HINT_HAS_DEFAULT (range_hints)) {
		return default_value;
	}

	lower = port_range_hints[port_num].LowerBound;
	upper = port_range_hints[port_num].UpperBound;

	if (LADSPA_IS_HINT_DEFAULT_MINIMUM (range_hints)) {
		default_value = lower;
	} else if (LADSPA_IS_HINT_DEFAULT_MAXIMUM (range_hints)) {
		default_value = upper;
	} else if (LADSPA_IS_HINT_DEFAULT_0 (range_hints)) {
		default_value = 0.0;
	} else if (LADSPA_IS_HINT_DEFAULT_1 (range_hints)) {
		default_value = 1.0;
	} else if (LADSPA_IS_HINT_DEFAULT_100 (range_hints)) {
		default_value = 100.0;
	} else if (LADSPA_IS_HINT_DEFAULT_440 (range_hints)) {
		default_value = 440.0;
	} else if (LADSPA_IS_HINT_DEFAULT_LOW (range_hints)) {
		if (LADSPA_IS_HINT_LOGARITHMIC (range_hints)) {
			default_value = exp (log (lower) * 0.75 + log (upper) * 0.25);
		} else {
			default_value =  lower * 0.75 + upper * 0.25;
		}
	} else if (LADSPA_IS_HINT_DEFAULT_MIDDLE (range_hints)) {
		if (LADSPA_IS_HINT_LOGARITHMIC (range_hints)) {
			default_value = exp (log (lower) * 0.5 + log (upper) * 0.5);
		} else {
			default_value =  lower * 0.5 + upper * 0.5;
		}
	} else if (LADSPA_IS_HINT_DEFAULT_HIGH (range_hints)) {
		if (LADSPA_IS_HINT_LOGARITHMIC (range_hints)) {
			default_value = exp (log (lower) * 0.25 + log (upper) * 0.75);
		} else {
			default_value =  lower * 0.75 + upper * 0.75;
		}
	}

	return default_value;
}

gint
ladspa_plugin_num_ports (const LADSPA_Descriptor *descriptor,
                         LADSPA_PortDescriptor port_type)
{
	gint i, count = 0;
	const LADSPA_PortDescriptor * port_descriptors;

	port_descriptors = descriptor->PortDescriptors;
	for (i = 0; i < descriptor->PortCount; i++) {
		if (port_descriptors[i] == port_type) {
			count++;
		}
	}
	return count;
}

gint
ladspa_plugin_get_index_for_parameter (ladspa_plugin_node_t *plugin, const gchar *param_name)
{
	gint i;
	const LADSPA_Descriptor *descriptor = plugin->plugin;

	g_return_val_if_fail (plugin, -1);
	g_return_val_if_fail (param_name, -1);

	for (i = 0; i < descriptor->PortCount; i++) {
		if (LADSPA_IS_PORT_CONTROL (descriptor->PortDescriptors[i])
		        || LADSPA_IS_PORT_INPUT (descriptor->PortDescriptors[i])) {
			if (g_strcmp0 (descriptor->PortNames[i], param_name) == 0) {
				return i;

			}
		}
	}
	return -1;
}

static gint pluginlib_sorter (gconstpointer a,
                              gconstpointer b)
{
   return (strcmp ((const gchar *)a, (const gchar *)b) );
}

GList *
ladspa_get_available_plugins (const gchar *path)
{
	GList *file_list = NULL;
	GDir *d;

	if (path == NULL) {
		path = LADSPA_DEFAULT_PATH;
	}

	d = g_dir_open (path, 0, NULL);
	const gchar * files;
	while ((files = g_dir_read_name (d)) != NULL) {
		file_list = g_list_prepend (file_list, g_strdup (files));
	}

	file_list = g_list_sort (file_list, pluginlib_sorter);
	g_dir_close (d);
	return file_list;
}
