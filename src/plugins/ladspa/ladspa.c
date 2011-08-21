/** @file ladspa.c
 *  LADSPA Hosting plugin
 *
 *  Copyright (C) 2006-2011 XMMS2 Team
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

#include "xmms/xmms_xformplugin.h"
#include "xmms/xmms_config.h"
#include "xmms/xmms_log.h"
#include "ladspa_plugin.h"
#include "xmmsc/xmmsc_schema.h"

#include <glib.h>
#include <stdlib.h>

static gboolean xmms_ladspa_plugin_setup (xmms_xform_plugin_t *xform_plugin);
static gboolean xmms_ladspa_init (xmms_xform_t *xform);
static void xmms_ladspa_destroy (xmms_xform_t *xform);
static gint xmms_ladspa_read (xmms_xform_t *xform, xmms_sample_t *buf, gint len,
                              xmms_error_t *error);

struct ladspa_data_St {
    gboolean enabled;
	gint num_channels; /* number of channels in the xform chain */
	gint buf_size; /* size of de-interleaved buffers */
	gint srate;
	gfloat **in_bufs; /* de-interleaved buffers */
	gfloat **out_bufs;
    ladspa_plugin_node_t *plugin_list;
	GMutex * mutex; /* to lock the plugin_list while it is being modified from a config call */
	xmms_xform_t *xform;
};

typedef struct ladspa_data_St ladspa_data_t;

#define XMMS_DEFAULT_BUFFER_SIZE 4096

static void ladspa_register_schema (xmms_xform_plugin_t *xform_plugin);

/* Allocate global buffers for host */
static void xmms_ladspa_allocate_buffers (ladspa_data_t *priv);
static void xmms_ladspa_free_buffers (ladspa_data_t *priv);

static void xmms_ladspa_reallocate_buffers (ladspa_data_t *priv, gint new_size, gint new_chans);

void ladspa_init_plugins_from_list (ladspa_data_t *priv, xmmsv_t *list);
static gboolean ladspa_init_plugin (ladspa_data_t *priv, const gchar *plugin, gint index);
static void ladspa_init_node (ladspa_data_t *priv,
                              ladspa_plugin_node_t *node,
                              gint index);
static void ladspa_config_changed (xmms_object_t *object, xmmsv_t *data, gpointer userdata);

void deinterleave (xmms_sample_t *buf, gfloat **in_bufs,
                   gint buf_size, gint chans, gint fmt);
void interleave (gfloat **out_bufs, xmms_sample_t *buf,
                 gint buf_size, gint chans, gint fmt);
void process_plugin_node (ladspa_plugin_node_t *plugin_node, ladspa_data_t *priv,
                          gint buf_size);
void clean_control_properties (ladspa_data_t *priv,
                               gint plugin_index, const gchar *plugin_name);
void register_control_property (ladspa_data_t *priv, ladspa_plugin_node_t *node,
                                gint ctl_num,  const char* port_name,
                                LADSPA_Data default_value, gint index);

void connect_param_callbacks (ladspa_data_t *priv, gint index);
void disconnect_param_callbacks (ladspa_data_t *priv, gint index);

ladspa_plugin_node_t *get_plugin_node (ladspa_plugin_node_t *first_node,  gint index);

XMMS_XFORM_PLUGIN ("ladspa",
                   "LADSPA Plugin Host",
                   XMMS_VERSION,
                   "LADSPA Plugin Host",
                   xmms_ladspa_plugin_setup)

static gboolean
xmms_ladspa_plugin_setup (xmms_xform_plugin_t *xform_plugin)
{
	xmms_xform_methods_t methods;
	xmmsv_t *value;

	XMMS_XFORM_METHODS_INIT (methods);

	methods.init = xmms_ladspa_init;
	methods.destroy = xmms_ladspa_destroy;
	methods.read = xmms_ladspa_read;
	methods.seek = xmms_xform_seek; /* Not needed */

	xmms_xform_plugin_methods_set (xform_plugin, &methods);

	value = xmmsv_build_list (XMMSV_LIST_ENTRY_STR (""),
	                          XMMSV_LIST_END);
	xmms_xform_plugin_config_register_value (xform_plugin, "plugin", value,
	                                          NULL, NULL);
	xmmsv_unref (value);
	value = xmmsv_new_dict ();
	xmms_xform_plugin_config_register_value (xform_plugin, "control.0", value,
	                                         NULL, NULL);

	xmms_xform_plugin_indata_add (xform_plugin,
	                              XMMS_STREAM_TYPE_MIMETYPE,
	                              "audio/pcm",
	                              XMMS_STREAM_TYPE_FMT_FORMAT,
	                              XMMS_SAMPLE_FORMAT_S16,
	                              XMMS_STREAM_TYPE_END);

	xmms_xform_plugin_indata_add (xform_plugin,
	                              XMMS_STREAM_TYPE_MIMETYPE,
	                              "audio/pcm",
	                              XMMS_STREAM_TYPE_FMT_FORMAT,
	                              XMMS_SAMPLE_FORMAT_FLOAT,
	                              XMMS_STREAM_TYPE_END);

	ladspa_register_schema (xform_plugin);

	XMMS_DBG ("LADSPA Host xform setup OK.");
	return TRUE;
}

static gboolean
xmms_ladspa_init (xmms_xform_t *xform)
{
	ladspa_data_t *priv;
	const gchar *enabled;
	gint srate, num_channels, fmt;
	guint buf_size;
	xmmsv_t *value, *plugin_list;

	g_return_val_if_fail (xform, FALSE);

	priv = g_new0 (ladspa_data_t, 1);
	g_return_val_if_fail (priv, FALSE);

	priv->mutex = g_mutex_new ();
	priv->plugin_list = NULL;
	priv->xform = xform;

	xmms_xform_private_data_set (xform, priv);
	xmms_xform_outdata_type_copy (xform);

	srate = xmms_xform_indata_get_int (xform, XMMS_STREAM_TYPE_FMT_SAMPLERATE);
	num_channels
		= xmms_xform_indata_get_int (xform, XMMS_STREAM_TYPE_FMT_CHANNELS);
	fmt = xmms_xform_indata_get_int (xform, XMMS_STREAM_TYPE_FMT_FORMAT);
	buf_size = XMMS_DEFAULT_BUFFER_SIZE/(xmms_sample_size_get (fmt) * num_channels);

	priv->num_channels = num_channels;
	priv->buf_size = buf_size;
	priv->srate = srate;

	xmms_ladspa_allocate_buffers (priv);

	xmms_xform_config_callback_set (xform, "enabled", ladspa_config_changed, priv);
	value = xmms_xform_config_lookup_value (xform, "enabled");
	g_return_val_if_fail (value, FALSE);
	xmmsv_get_string (value, &enabled);
	priv->enabled = enabled[0] != '0';
	xmmsv_unref (value);

	plugin_list = xmms_xform_config_lookup_value (xform, "plugin");
	g_return_val_if_fail (value, FALSE);
	ladspa_init_plugins_from_list (priv, plugin_list);
	xmmsv_unref (plugin_list);

	return TRUE;
}

void
ladspa_init_plugins_from_list (ladspa_data_t *priv, xmmsv_t *list)
{
	gint i;
	xmmsv_list_iter_t *it;
	const gchar *plugin;

	i = 0;
	xmmsv_get_list_iter (list, &it);
	while (xmmsv_list_iter_valid (it)) {
		xmmsv_list_iter_entry_string (it, &plugin);

		disconnect_param_callbacks (priv, i);
		if (!ladspa_init_plugin (priv, plugin, i)) {
			xmms_log_error ("LADSPA Plugin init error");
		}
		clean_control_properties (priv, i, plugin);
		connect_param_callbacks (priv, i);
		i++;
		xmmsv_list_iter_next (it);
	}
}

static void
xmms_ladspa_destroy (xmms_xform_t *xform)
{
	ladspa_data_t *priv;
	xmms_config_property_t *config;
	ladspa_plugin_node_t *old_node;
	ladspa_plugin_node_t *plugin_node;
	gint i;

	g_return_if_fail (xform);

	priv = (ladspa_data_t *) xmms_xform_private_data_get (xform);

	g_mutex_lock (priv->mutex); /* force wait until current playback buffer is processed */
	plugin_node = priv->plugin_list;
	i = 0;
	while (plugin_node != NULL) {
		old_node = plugin_node;
		plugin_node = plugin_node->next;
		ladspa_plugin_free_node (old_node);
		disconnect_param_callbacks (priv, i);
		i++;
	}
	priv->plugin_list = NULL;

	/* Free global config options */
	config = xmms_xform_config_lookup (xform, "enabled");
	xmms_config_property_callback_remove (config, ladspa_config_changed, priv);
	config = xmms_xform_config_lookup (xform, "plugin");
	xmms_config_property_callback_remove (config, ladspa_config_changed, priv);

	xmms_ladspa_free_buffers (priv);
	g_mutex_unlock (priv->mutex);
	g_mutex_free (priv->mutex);
	g_free (priv);
}

void
deinterleave (xmms_sample_t *buf, gfloat **in_bufs,
              gint buf_size, gint chans, gint fmt)
{
	gint i,j;
	if (fmt == XMMS_SAMPLE_FORMAT_S16) {
		for (i = 0; i < buf_size; i++) {
			for (j = 0; j < chans; j++) {
				in_bufs[j][i]
					= (gfloat) ( ((gint16 *) buf)[j + (i*chans)] / (gfloat) G_MAXINT16);
			}
		}
	} else if (fmt == XMMS_SAMPLE_FORMAT_FLOAT) {
		for (i = 0; i < buf_size; i++) {
			for (j = 0; j < chans; j++) {
				in_bufs[j][i] = ( (gfloat *) buf)[j + (i*chans)];
			}
		}
	}
}

void
interleave (gfloat **out_bufs, xmms_sample_t *buf,
            gint buf_size, gint chans, gint fmt)
{
	gint i,j ;
	for (i = 0; i < buf_size; i++) {
		for (j = 0; j < chans; j++) {
			if (fmt == XMMS_SAMPLE_FORMAT_S16) {
				/* clip output */
				if (out_bufs[j][i] > 1.0) {
					((gint16 *) buf)[j + (i*chans)] = G_MAXINT16;
				} else if (out_bufs[j][i] <= -1.0) {
					((gint16 *) buf)[j + (i*chans)] = G_MININT16;
				} else {
					((gint16 *) buf)[j + (i*chans)]
						= (gint16) (out_bufs[j][i] * G_MAXINT16);
				}
			} else if (fmt == XMMS_SAMPLE_FORMAT_FLOAT) {
				( (gfloat *) buf)[j + (i*chans)] = out_bufs[j][i];
			}
		}
	}
}

void
process_plugin_node (ladspa_plugin_node_t *plugin_node, ladspa_data_t *priv,
                     gint buf_size)
{
	gint i, j, max_out;
	/* all nodes must finish with their output on out_bufs */
	switch (plugin_node->mode) {
		case LADSPA_DIRECT:
			plugin_node->plugin->run (plugin_node->instance[0], buf_size);
			break;
		case LADSPA_MONO:
			g_assert (plugin_node->num_instances == priv->num_channels);
			for (i = 0; i < plugin_node->num_instances; i++) {
				plugin_node->plugin->run (plugin_node->instance[i], buf_size);
			}
			break;
		case LADSPA_BALANCE:
			g_assert (plugin_node->num_instances == 2 && priv->num_channels == 2 && plugin_node->num_out_channels == 2);
			plugin_node->plugin->run (plugin_node->instance[0], buf_size);
			for (j = 0; j < buf_size; j++) {
				priv->out_bufs[0][j]
					= (1 - plugin_node->balance) * plugin_node->temp_bufs[0][j];
				priv->out_bufs[1][j]
					= plugin_node->balance * plugin_node->temp_bufs[1][j];
			}
			plugin_node->plugin->run (plugin_node->instance[1], buf_size);
			for (j = 0; j < buf_size; j++) {
				priv->out_bufs[0][j]
					+= plugin_node->balance * plugin_node->temp_bufs[0][j];
				priv->out_bufs[1][j]
					+= (1 - plugin_node->balance) * plugin_node->temp_bufs[1][j];
			}
			break;
		case LADSPA_OTHER:
			/* clear output buffers first since we are going to accumulate output there */
			for (i = 0; i < plugin_node->num_out_channels; i++) {
				/* would memset be better here? */
				/* memset (out_bufs[i], 0, buf_size * sizeof (ladspa_data_t) ); */
				for (j = 0; j < buf_size; j++) {
					priv->out_bufs[i][j] = 0.0;
				}
			}
			max_out = (plugin_node->num_instances > priv->num_channels ?
			           plugin_node->num_instances : priv->num_channels);
			for (i = 0; i < max_out; i++) {
				g_assert (plugin_node->num_out_channels > 0);
				plugin_node->plugin->run (plugin_node->instance[i], buf_size);
				for (j = 0; j < buf_size; j++) {
					priv->out_bufs[i % priv->num_channels][j]
						+= plugin_node->temp_bufs[i % plugin_node->num_out_channels][j];
				}
			}
			break;
		case LADSPA_NONE:
			for (i = 0; i < priv->num_channels; i++) {
				for (j = 0; j < buf_size; j++) {
					priv->out_bufs[i][j] = priv->in_bufs[i][j];
				}
			}
			break;
	}

	if (plugin_node->next != NULL) { /* Copy output to input for next plugin */
		for (i = 0; i < buf_size; i++) {
			for (j = 0; j < priv->num_channels; j++) {
				priv->in_bufs[j][i] = priv->out_bufs[j][i];
			}
		}
	}
}

static void
ladspa_register_schema (xmms_xform_plugin_t *xform_plugin)
{
	GList *plugin_libs = ladspa_get_available_plugins (NULL);
	GList *tmp;
	xmmsv_t *pluginlib, *plugins, *ctl_dict, *enabled, *priority, *ladspa_sec, *ladspa;
	xmmsv_t *plugin_enum;
	gboolean ok;

	plugin_enum = xmmsv_new_list ();

	for (tmp = plugin_libs; tmp; tmp = g_list_next( tmp )) {
		xmmsv_list_append (plugin_enum, xmmsv_new_string (tmp->data));
		g_free(tmp->data);
	}

	xmmsv_list_append (plugin_enum, xmmsv_new_string (""));
	g_list_free (plugin_libs);

	pluginlib = xmms_schema_build_string_all ("Plugin List", "List of all available plugins", "",
	                                          plugin_enum);
	plugins = xmms_schema_build_list ("plugin", "List of intantiated plugins", pluginlib);

	ctl_dict = xmms_schema_build_list ("control", "List containing the parameters for each instanced plugin",
	                                   xmms_schema_build_any ("", ""));
	enabled = xmms_schema_build_string ("enabled", "Enable or disable the plugin", "0");
	priority = xmms_schema_build_dict ("priority", "Set priority for plugin within engine",
	                                   xmms_schema_build_any ("", ""));

	ladspa_sec = xmms_schema_build_dict_entry_types (plugins, ctl_dict, enabled, priority, NULL);
	ladspa = xmms_schema_build_dict ("ladspa", "LADSPA Plugin host", ladspa_sec);

	ok = xmms_xform_plugin_config_register_schema (xform_plugin, ladspa);
	if (!ok) {
		xmms_log_info ("Could not register schema.");
	}
}

static void
xmms_ladspa_reallocate_buffers (ladspa_data_t *priv, gint new_size, gint new_chans)
{
	xmms_xform_t *xform;
	xmms_log_info ("Reallocating ladspa buffers to size %i", new_size);
	/* Reallocating the plugin is a  bit exaggerated, but it's a quick way of
	   reconnecting the plugin to the new buffers this is something that
	   should be rare enough that it doesn't matter */

	xform = priv->xform;
	xmms_ladspa_destroy (xform);
	xmms_ladspa_init (xform);

	/* can we really get away wiht this in practice? */
}

static gint
xmms_ladspa_read (xmms_xform_t *xform, xmms_sample_t *buf, gint len,
              xmms_error_t *error)
{
	ladspa_data_t *priv;
	gint read;
	gint chans;
	gint fmt;
	gint buf_size;
	ladspa_plugin_node_t *plugin_node;
	g_return_val_if_fail (xform, -1);

	priv = xmms_xform_private_data_get (xform);
	g_return_val_if_fail (priv, -1);

	plugin_node = priv->plugin_list;
	read = xmms_xform_read (xform, buf, len, error);
	chans = xmms_xform_indata_get_int (xform, XMMS_STREAM_TYPE_FMT_CHANNELS);
	fmt = xmms_xform_indata_get_int (xform, XMMS_STREAM_TYPE_FMT_FORMAT);
	buf_size = XMMS_DEFAULT_BUFFER_SIZE/(xmms_sample_size_get (fmt) * chans);

	if (!priv->enabled || plugin_node == NULL || read <= 0) {
		return read;
	}
	g_mutex_lock (priv->mutex);

	if (buf_size != priv->buf_size || chans != priv->num_channels) {
		xmms_ladspa_reallocate_buffers (priv, buf_size, chans);
	}
	deinterleave (buf, priv->in_bufs, buf_size, chans, fmt);

	while (plugin_node != NULL) {
		process_plugin_node (plugin_node, priv, buf_size);
		plugin_node = plugin_node->next;
	}
	interleave (priv->out_bufs, buf, buf_size, chans, fmt);

	g_mutex_unlock (priv->mutex);
	return read;
}

static void
xmms_ladspa_allocate_buffers (ladspa_data_t *priv)
{
	gint i;
	const gint num_channels = priv->num_channels;
	const gint buf_size = priv->buf_size;
	priv->in_bufs = g_new0 (gfloat*, num_channels);
	priv->out_bufs = g_new0 (gfloat*, num_channels);
	for (i = 0; i < num_channels; i++) {
		priv->in_bufs[i] = g_new0 (gfloat, buf_size);
		priv->out_bufs[i] = g_new0 (gfloat, buf_size);
	}
}

static void
xmms_ladspa_free_buffers (ladspa_data_t *priv)
{
	gint i;
	if (priv->in_bufs && priv->out_bufs) {
		for (i = 0; i < priv->num_channels; i++) {
			g_free (priv->in_bufs[i]);
			g_free (priv->out_bufs[i]);
		}
		g_free (priv->in_bufs);
		g_free (priv->out_bufs);
	}
}

static void
ladspa_config_changed (xmms_object_t *object, xmmsv_t *data, gpointer userdata)
{
	ladspa_data_t *priv = (ladspa_data_t *) userdata;
	const gchar *name, *str_value;
	gfloat value;
	gint plugin_index, control_index;

	name = xmms_config_property_get_name ((xmms_config_property_t *) object);
	value = xmms_config_property_get_float ((xmms_config_property_t *) object);
	str_value = xmms_config_property_get_string ((xmms_config_property_t *) object);

	if (g_str_has_prefix (name, "ladspa.plugin.")) {
		ladspa_plugin_node_t *node;
		gint index = atoi (strrchr (name, '.') + 1);

		/* TODO check if name hasn't changed and don't re-instantiate */
		node = ladspa_plugin_new_node (str_value, priv->num_channels, priv->buf_size, priv->srate);
		g_mutex_lock (priv->mutex);

		disconnect_param_callbacks (priv, index);
		ladspa_init_node (priv, node, index);
		g_mutex_unlock (priv->mutex);
		clean_control_properties (priv, index, str_value);
		connect_param_callbacks (priv, index);
	} else if (g_str_has_prefix (name, "ladspa.control.") ) {
		ladspa_plugin_node_t *node;
		gchar *param_name;
		plugin_index = (gint) atoi (name + 15);
		param_name = strrchr (name, '.') + 1;
		node = get_plugin_node (priv->plugin_list, plugin_index);
		if (node) {
			control_index = ladspa_plugin_get_index_for_parameter (node, param_name);
			if (control_index > -1 && control_index < priv->plugin_list->num_ctl_in_ports) {
				*(node->ctl_in_ports[control_index]) = value;
			}
		}
	} else if (!strcmp (name, "ladspa.enabled")) {
		gint int_value = xmms_config_property_get_int ((xmms_config_property_t *) object);
		priv->enabled = !!int_value;
	}

	/* TODO validate ladspa plugin parameters with:
	   LADSPA_IS_HINT_BOUNDED_BELOW(x)
	   LADSPA_IS_HINT_BOUNDED_ABOVE(x)
	   #define LADSPA_IS_HINT_TOGGLED(x)
	   LADSPA_IS_HINT_INTEGER(x) */
}

static gboolean
ladspa_init_plugin (ladspa_data_t *priv, const gchar *plugin, gint index)
{
	gint i;
	ladspa_plugin_node_t *node = NULL;
	gchar *path;
	xmmsv_t *value, *empty;

	node = ladspa_plugin_new_node (plugin, priv->num_channels,
	                               priv->buf_size, priv->srate);
	ladspa_init_node (priv, node, index);

	i = 0;
	path = g_strdup_printf ("plugin.%i", i + 1); /* add free config slot after last one */
	value = xmms_xform_config_lookup_value (priv->xform, path);
	if (!value) {
		path = g_strdup_printf ("plugin.%i", i + 1); /* add free config slot after last one */
		empty = xmmsv_new_string ("");
		xmms_xform_config_set_value (priv->xform, path, empty);
		xmms_xform_config_callback_set (priv->xform, path, ladspa_config_changed, priv);
		xmmsv_unref (empty);
	} else {
		xmmsv_unref (value);
	}
	g_free (path);
	path = g_strdup_printf ("control.%i", i + 1);
	value = xmms_xform_config_lookup_value (priv->xform, path);
	if (!value) {
		empty = xmmsv_new_dict ();
		xmms_xform_config_set_value (priv->xform, path, empty);
		xmmsv_unref (empty);
	} else {
		xmmsv_unref (value);
	}
	return TRUE;
}

static void
ladspa_init_node (ladspa_data_t *priv,
                  ladspa_plugin_node_t *node,
                  gint index)
{
	LADSPA_Data default_value;
	const LADSPA_Descriptor *descriptor;
	const LADSPA_PortDescriptor * PortDescriptors;
	const char * const * PortNames;
	const LADSPA_PortRangeHint * PortRangeHints;
	guint out_count, in_count, ctl_in_port_count, ctl_out_port_count;
	guint i, inst;

	if (!node->plugin || node->num_out_channels == 0) {
		return;
	}
	descriptor = node->plugin;
	PortDescriptors = descriptor->PortDescriptors;
	PortNames = descriptor->PortNames;
	PortRangeHints = descriptor->PortRangeHints;

	ctl_in_port_count = 0;
	for (i = 0; i < descriptor->PortCount; i++) {
		if (LADSPA_IS_PORT_CONTROL (PortDescriptors[i])) {
			default_value = ladspa_plugin_get_default_value (PortRangeHints, i);
			register_control_property (priv, node, ctl_in_port_count,
			                           PortNames[i], default_value, index);
			ctl_in_port_count++;
		}
	}
	/* now connect the ports */
	for (inst = 0; inst < node->num_instances; inst++) {
		if (descriptor->activate) {
			descriptor->activate (node->instance[inst]);
		}
		out_count = in_count = ctl_in_port_count = ctl_out_port_count = 0;
		for (i = 0; i < descriptor->PortCount; i++) {
			if (LADSPA_IS_PORT_CONTROL (PortDescriptors[i])) {
				if (LADSPA_IS_PORT_INPUT (PortDescriptors[i])) {
					descriptor->connect_port (node->instance[inst], i,
					                          node->ctl_in_ports[ctl_in_port_count] );
					ctl_in_port_count++;
				} else if (LADSPA_IS_PORT_OUTPUT (PortDescriptors[i])) {
					/* Output control ports are ignored */
					ctl_out_port_count++;
				}
			} else if (LADSPA_IS_PORT_AUDIO (PortDescriptors[i])) {
				if (LADSPA_IS_PORT_INPUT (PortDescriptors[i])) {
					/* all instances share the in buffer */
					switch (node->mode) {
						case LADSPA_MONO:
						case LADSPA_BALANCE:
							descriptor->connect_port (node->instance[inst], i,
							                          priv->in_bufs[inst] );
							break;
						case LADSPA_DIRECT:
							descriptor->connect_port (node->instance[inst], i,
							                          priv->in_bufs[in_count]);
							break;
						default: /* LADSPA_OTHER */
						/* repeat input channels if plugin takes more channels than available in the chain */
						descriptor->connect_port (node->instance[inst], i,
						                          priv->in_bufs[in_count % priv->num_channels]);
					}
					in_count++;
				} else if (LADSPA_IS_PORT_OUTPUT (PortDescriptors[i])) {
					switch (node->mode) {
						case LADSPA_MONO:
							descriptor->connect_port (node->instance[inst], i,
							                          priv->out_bufs[inst] );
							break;
						case LADSPA_DIRECT:
							descriptor->connect_port (node->instance[inst], i,
							                          priv->out_bufs[out_count] );
							break;
						case LADSPA_BALANCE:
							descriptor->connect_port (node->instance[inst], i,
							                          node->temp_bufs[out_count] );
							break;
						default: /* LADSPA_OTHER */
							descriptor->connect_port (node->instance[inst], i,
							                          node->temp_bufs[out_count]);
					}
					out_count++;
				}
			}
		}
	}

	/* insert node in chain */
	if (!priv->plugin_list) { /* first ladspa plugin */
		priv->plugin_list = node;
		xmms_xform_config_callback_set (priv->xform, "plugin.0", ladspa_config_changed, priv);
	} else {
		ladspa_plugin_node_t *parent = get_plugin_node (priv->plugin_list, index - 1);
		ladspa_plugin_node_t *old_node = get_plugin_node (priv->plugin_list, index);
		parent->next = node;
		if (old_node) {
			node->next = old_node->next;
			ladspa_plugin_free_node (old_node);
		}
	}

	XMMS_DBG ("Plugin '%s'. Init OK. Mode %i",
	          descriptor->Name, node->mode);
	return;
}

void
clean_control_properties (ladspa_data_t *priv,
                          gint plugin_index, const gchar *plugin_name)
{
	gchar *path;
	xmmsv_t *value, *control_list;
	ladspa_plugin_node_t *plugin_node;

	/* first check if config paths are sane */
	value = xmms_xform_config_lookup_value (priv->xform, "control");
	if (!value || !xmmsv_is_type (value, XMMSV_TYPE_LIST)) {
		control_list = xmmsv_new_list ();
		while (xmmsv_list_get_size (control_list) < plugin_index) {
			xmmsv_list_append (control_list, xmmsv_new_dict ());
		}
		xmms_xform_config_set_value (priv->xform, "control", control_list);
		xmmsv_unref (control_list);
	}
	if (value) {
		xmmsv_unref (value);
	}
	plugin_node = get_plugin_node (priv->plugin_list, plugin_index);
	/* clean control properties only if plugin name does not match and previous plugin is not empty */
	if (plugin_node) {
		xmmsv_dict_iter_t *it;

		path = g_strdup_printf ("control.%i", plugin_index);
		value = xmms_xform_config_lookup_value (priv->xform, path);
		xmmsv_get_dict_iter (value, &it);
		while (xmmsv_dict_iter_valid (it)) {
			const gchar *key;
			xmmsv_t *dummy_val;
			xmmsv_dict_iter_pair (it, &key, &dummy_val);
			if (ladspa_plugin_get_index_for_parameter (plugin_node, key) < 0) {
				xmmsv_dict_iter_remove (it);
			} else {
				xmmsv_dict_iter_next (it);
			}
		}
		if (value) {
			xmms_xform_config_set_value (priv->xform, path, value);
			xmmsv_unref (value);
		}
	}
}

void
register_control_property (ladspa_data_t *priv, ladspa_plugin_node_t *node,
                           gint ctl_num, const char* port_name,
                           LADSPA_Data default_value, gint index)
{
	gchar *property_path;
	xmmsv_t *value, *float_value;
	gfloat ctl_value;
	gboolean ok;

	property_path = g_strdup_printf ("control.%i.%s", index, port_name);
	value = xmms_xform_config_lookup_value (priv->xform, property_path);
	if (!value) {
		/* Pass the default value to the plugin */
		g_assert (ctl_num < node->num_ctl_in_ports);
		*(node->ctl_in_ports[ctl_num]) = default_value;
		float_value = xmmsv_new_float (default_value);
		xmms_xform_config_set_value (priv->xform, property_path, float_value);
		xmms_xform_config_callback_set (priv->xform, property_path,
		                                ladspa_config_changed, priv);
	}
	if (value) {
		xmmsv_unref (value);
	}

	value = xmms_xform_config_lookup_value (priv->xform, property_path);
	ok = xmmsv_get_float (value, &ctl_value);
	if (ok) {
		*(node->ctl_in_ports[ctl_num]) = ctl_value;
	}
	if (value) {
		xmmsv_unref (value);
	}
}

void
connect_param_callbacks (ladspa_data_t *priv, gint index)
{
	gint j;

	xmmsv_t *plugin_ctls;
	char *ctl_path, *ctl_path2;
	const char *ctl_name;
	xmmsv_dict_iter_t *it;

	ctl_path = g_strdup_printf ("control.%i", index);
	plugin_ctls = xmms_xform_config_lookup_value (priv->xform, ctl_path);
	if (!xmmsv_is_type (plugin_ctls, XMMSV_TYPE_DICT)) {
		return;
	}
	xmmsv_get_dict_iter (plugin_ctls, &it);
	j = 0;
	while (xmmsv_dict_iter_valid (it)) {
		xmmsv_t *dummy_val;
		if (xmmsv_dict_iter_pair (it, &ctl_name, &dummy_val)) {
			ctl_path2 = g_strdup_printf ("%s.%s", ctl_path, ctl_name);
			xmms_xform_config_callback_set (priv->xform, ctl_path2, ladspa_config_changed, priv);
		}
		xmmsv_dict_iter_next (it);
		g_free (ctl_path2);
		j++;
	}
	g_free (ctl_path);
}

void
disconnect_param_callbacks (ladspa_data_t *priv, gint index)
{
	gint j;

	xmmsv_t *plugin_ctls;
	char *ctl_path, *ctl_path2;
	const char *ctl_name;
	xmmsv_dict_iter_t *it;

	ctl_path = g_strdup_printf ("control.%i", index);
	plugin_ctls = xmms_xform_config_lookup_value (priv->xform, ctl_path);
	if (!xmmsv_is_type (plugin_ctls, XMMSV_TYPE_DICT)) {
		return;
	}
	xmmsv_get_dict_iter (plugin_ctls, &it);
	j = 0;
	while (xmmsv_dict_iter_valid (it)) {
		xmmsv_t *dummy_val;
		if (xmmsv_dict_iter_pair (it, &ctl_name, &dummy_val)) {
			ctl_path2 = g_strdup_printf ("%s.%s", ctl_path, ctl_name);
			xmms_xform_config_callback_remove (priv->xform, ctl_path2, ladspa_config_changed, priv);
		}
		xmmsv_dict_iter_next (it);
		g_free (ctl_path2);
		j++;
	}
	g_free (ctl_path);
}

ladspa_plugin_node_t *
get_plugin_node (ladspa_plugin_node_t *first_node,  gint index)
{
	ladspa_plugin_node_t *out_node = first_node;

	while (index-- > 0 && out_node) {
		out_node = out_node->next;
	}
	return out_node;
}
