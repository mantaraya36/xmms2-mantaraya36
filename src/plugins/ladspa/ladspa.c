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
};

typedef struct ladspa_data_St ladspa_data_t;

#define XMMS_DEFAULT_BUFFER_SIZE 4096

/* Allocate global buffers for host */
static void xmms_ladspa_allocate_buffers (ladspa_data_t *priv);
static void xmms_ladspa_free_buffers (ladspa_data_t *priv);

static void xmms_ladspa_reallocate_buffers (ladspa_data_t *priv, gint new_size, gint new_chans);

static gboolean ladspa_init_plugin (ladspa_data_t *priv, const gchar *plugin);
static void ladspa_init_node (ladspa_data_t *priv,
                                  ladspa_plugin_node_t *node);
static void ladspa_config_changed (xmms_object_t *object, xmmsv_t *data, gpointer userdata);

void deinterleave (xmms_sample_t *buf, gfloat **in_bufs,
                   gint buf_size, gint chans, gint fmt);
void interleave (gfloat **out_bufs, xmms_sample_t *buf,
                 gint buf_size, gint chans, gint fmt);
void clean_unused_control_properties (gint num_ctl_ports);
void set_control_property (ladspa_data_t *priv, ladspa_plugin_node_t *node,
                           gint ctl_num,  const char* port_name,
                           LADSPA_Data default_value);

XMMS_XFORM_PLUGIN ("ladspa",
                   "LADSPA Plugin Host",
                   XMMS_VERSION,
                   "LADSPA Plugin Host",
                   xmms_ladspa_plugin_setup);

static gboolean
xmms_ladspa_plugin_setup (xmms_xform_plugin_t *xform_plugin)
{
	xmms_xform_methods_t methods;

	XMMS_XFORM_METHODS_INIT (methods);

	methods.init = xmms_ladspa_init;
	methods.destroy = xmms_ladspa_destroy;
	methods.read = xmms_ladspa_read;
	methods.seek = xmms_xform_seek; /* Not needed */

	xmms_xform_plugin_methods_set (xform_plugin, &methods);
	xmms_xform_plugin_config_property_register (xform_plugin, "plugin", "",
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

	xmms_log_info ("LADSPA Host xform setup OK.");

	return TRUE;
}

static gboolean
xmms_ladspa_init (xmms_xform_t *xform)
{
	xmms_config_property_t *config;
	ladspa_data_t *priv;
	const gchar *plugin;
	gint srate, num_channels, fmt;
	guint buf_size;
	gint i;
	gchar *value_property_name;

	g_return_val_if_fail (xform, FALSE);

	priv = g_new0 (ladspa_data_t, 1);
	g_return_val_if_fail (priv, FALSE);

	priv->mutex = g_mutex_new ();
	priv->plugin_list = NULL;

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

	config = xmms_xform_config_lookup (xform, "enabled");
	g_return_val_if_fail (config, FALSE);
	xmms_config_property_callback_set (config, ladspa_config_changed, priv);
	priv->enabled = !!xmms_config_property_get_int (config);

	config = xmms_xform_config_lookup (xform, "plugin");
	g_return_val_if_fail (config, FALSE);
	xmms_config_property_callback_set (config, ladspa_config_changed, priv);
	plugin = xmms_config_property_get_string (config);

	i = 0;
	value_property_name = g_strdup_printf ("ladspa.control.%i",
	                                       i, NULL);
	config = xmms_config_lookup (value_property_name);
	while (config) {
		/* This is a bit of a kludge since the xmms2 config system was not designed
		   for mutable properties. The idea is to set the callbacks for all existing
		   properties here, so they are set only once and set whenever a new
		   property has to be defined because the plugin has more properties than
		   the ones currently available.
		*/
		xmms_config_property_callback_set (config, ladspa_config_changed, priv);
		g_free (value_property_name);
		i++;
		value_property_name = g_strdup_printf ("ladspa.control.%i",
		                                       i, NULL);
		config = xmms_config_lookup (value_property_name);
	}

	g_free (value_property_name);
	if (!ladspa_init_plugin (priv, plugin)) {
		xmms_log_error ("Plugin init error");
	}
	return TRUE;
}

static void
xmms_ladspa_destroy (xmms_xform_t *xform)
{
	ladspa_data_t *priv;
	xmms_config_property_t *config;
	ladspa_plugin_node_t *old_node;
	ladspa_plugin_node_t *plugin_node;
	gint i;
	gchar *value_property_name;

	g_return_if_fail (xform);

	priv = (ladspa_data_t *) xmms_xform_private_data_get (xform);

	g_mutex_lock (priv->mutex); /* force wait until current playback buffer is processed */
	plugin_node = priv->plugin_list;
	while (plugin_node != NULL) {
		old_node = plugin_node;
		plugin_node = plugin_node->next;
		ladspa_plugin_free_node (old_node);
	}
	priv->plugin_list = NULL;

	/* Free global config options */
	config = xmms_xform_config_lookup (xform, "enabled");
	xmms_config_property_callback_remove (config, ladspa_config_changed, priv);
	config = xmms_xform_config_lookup (xform, "plugin");
	xmms_config_property_callback_remove (config, ladspa_config_changed, priv);

	i = 0;
	value_property_name = g_strdup_printf ("ladspa.control.%i",
	                                       i, NULL);
	config = xmms_config_lookup (value_property_name);
	while (config) {
		/* This is a bit of a kludge since the xmms2 config system was not designed
		   for mutable properties. The idea is to set the callbacks for all existing
		   properties here, so they are set only once and set whenever a new
		   property has to be defined because the plugin has more properties than
		   the ones currently available.
		*/
		xmms_config_property_callback_remove (config, ladspa_config_changed, priv);
		g_free (value_property_name);
		i++;
		value_property_name = g_strdup_printf ("ladspa.control.%i",
		                                       i, NULL);
		config = xmms_config_lookup (value_property_name);
	}

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

static void
xmms_ladspa_reallocate_buffers (ladspa_data_t *priv, gint new_size, gint new_chans)
{
	const gchar *plugin;
	xmms_config_property_t *property;
	ladspa_plugin_node_t *node, *old_node;
	xmms_log_info ("Reallocating ladspa buffers to size %i", new_size);
	xmms_ladspa_free_buffers (priv);
	priv->buf_size = new_size;
	priv->num_channels = new_chans;
	xmms_ladspa_allocate_buffers (priv);
	/* Reallocating the plugin is a  bit exaggerated, but it's a quick way of
	   reconnecting the plugin to the new buffers this is something that
	   should be rare enough that it doesn't matter */
	property = xmms_config_lookup ("ladspa.plugin");
	g_assert (property); /* Should be available if we got this far */
	plugin = xmms_config_property_get_string (property);

	node = ladspa_plugin_new_node (plugin, priv->num_channels, priv->buf_size, priv->srate);
	g_mutex_lock (priv->mutex);

	old_node = priv->plugin_list;
	priv->plugin_list = node;
	if (node != NULL) {
		ladspa_init_node (priv, node);
	} else {
		xmms_log_info ("Error loading library!");
	}

	g_mutex_unlock (priv->mutex);
	ladspa_plugin_free_node (old_node);
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
	gint i, j, max_out;
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
				for (j = 0; j < plugin_node->num_out_channels; j++) {
					priv->in_bufs[j][i] = priv->out_bufs[j][i];
				}
			}
		}
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
	gulong control_index;
	ladspa_plugin_node_t *node, *old_node;

	name = xmms_config_property_get_name ((xmms_config_property_t *) object);
	value = xmms_config_property_get_float ((xmms_config_property_t *) object);
	str_value = xmms_config_property_get_string ((xmms_config_property_t *) object);

	if (!g_ascii_strcasecmp (name, "ladspa.plugin")) {
		node = ladspa_plugin_new_node (str_value, priv->num_channels, priv->buf_size, priv->srate);
		g_assert (node != NULL);

		g_mutex_lock (priv->mutex);

		old_node = priv->plugin_list;
		priv->plugin_list = node;
		ladspa_init_node (priv, node);

		g_mutex_unlock (priv->mutex);
		if (old_node) {
			ladspa_plugin_free_node (old_node);
		}
	} else if (g_str_has_prefix (name, "ladspa.control.") ) {
		if (priv->plugin_list) {
			control_index = (gulong) g_ascii_strtoull (name+15, NULL, 10); /* Is there a better way to find this number? */
			if (control_index < priv->plugin_list->num_ctl_in_ports) {
				*(priv->plugin_list->ctl_in_ports[control_index]) = value;
			}
		}
	}

	/* TODO validate ladspa plugin parameters with:
	   LADSPA_IS_HINT_BOUNDED_BELOW(x)
	   LADSPA_IS_HINT_BOUNDED_ABOVE(x)
	   #define LADSPA_IS_HINT_TOGGLED(x)
	   LADSPA_IS_HINT_INTEGER(x) */
}


static gboolean
ladspa_init_plugin (ladspa_data_t *priv, const gchar *plugin)
{
	ladspa_plugin_node_t *node = NULL;

	node = ladspa_plugin_new_node (plugin, priv->num_channels,
	                               priv->buf_size, priv->srate);
	ladspa_init_node (priv, node);

	/* Find last plugin node and put node created here */
	if (!priv->plugin_list) { /* first ladspa plugin */
		priv->plugin_list = node;
	} else {
		ladspa_plugin_node_t *n = priv->plugin_list;
		while (n->next != NULL) { /* Find last node in list */
			n = n->next;
		}
		n->next = node;
	}
	return TRUE;
}

static void
ladspa_init_node (ladspa_data_t *priv,
                  ladspa_plugin_node_t *node)
{
	LADSPA_Data default_value;
	const LADSPA_Descriptor *descriptor;
	const LADSPA_PortDescriptor * PortDescriptors;
	const char * const * PortNames;
	const LADSPA_PortRangeHint * PortRangeHints;
	guint out_count, in_count, ctl_in_port_count, ctl_out_port_count;
	guint i, inst;

	if (!node->plugin || node->num_out_channels == 0) {
		clean_unused_control_properties (0);
		return;
	}
	descriptor = node->plugin;
	PortDescriptors = descriptor->PortDescriptors;
	PortNames = descriptor->PortNames;
	PortRangeHints = descriptor->PortRangeHints;

	clean_unused_control_properties (node->num_ctl_in_ports);
	ctl_in_port_count = 0;
	for (i = 0; i < descriptor->PortCount; i++) {
		if (LADSPA_IS_PORT_CONTROL (PortDescriptors[i])) {
			default_value = ladspa_plugin_get_default_value (PortRangeHints, i);
			set_control_property (priv, node, ctl_in_port_count,
			                      PortNames[i], default_value);
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
	xmms_log_info ("Plugin '%s'. Init OK. Mode %i",
	               descriptor->Name, node->mode);
	return;
}

void
clean_unused_control_properties (gint num_ctl_ports)
{
	gint i = num_ctl_ports;
	gchar *value_property_name, *name_property_name;
	xmms_config_property_t *property;

	name_property_name = g_strdup_printf ("ladspa.controlname.%i",
	                                      i, NULL);
	value_property_name = g_strdup_printf ("ladspa.control.%i",
	                                      i, NULL);
	property = xmms_config_lookup (name_property_name);
	g_free (name_property_name);
	while (property) {
		xmms_config_property_set_data (property, "");
		property = xmms_config_lookup (value_property_name);
		xmms_config_property_set_data (property, ""); /* clear both name and value */
		i++;
		name_property_name = g_strdup_printf ("ladspa.controlname.%i",
		                                      i, NULL);
		property = xmms_config_lookup (name_property_name);
		g_free (name_property_name);
	}
}

void
set_control_property (ladspa_data_t *priv, ladspa_plugin_node_t *node,
                      gint ctl_num, const char* port_name,
                      LADSPA_Data default_value)
{
	gchar *default_value_str, *value_property_name, *name_property_name;
	xmms_config_property_t *property, *property_name;

	default_value_str = g_strdup_printf ("%.6f", default_value);
	value_property_name = g_strdup_printf ("ladspa.control.%i",
	                                       ctl_num, NULL);
	name_property_name = g_strdup_printf ("ladspa.controlname.%i",
	                                      ctl_num, NULL);
	property = xmms_config_lookup (value_property_name);
	property_name = xmms_config_lookup (name_property_name);
	if (!property || !property_name) {
		property = xmms_config_property_register (value_property_name,
		                                          default_value_str,
		                                          NULL, NULL);
		xmms_config_property_set_data (property, default_value_str);
		property_name = xmms_config_property_register (name_property_name,
		                                               port_name,
		                                               NULL, NULL);
		xmms_config_property_callback_set (property, ladspa_config_changed, priv);
		/* Pass the default value to the plugin */
		g_assert (ctl_num < node->num_ctl_in_ports);
		*(node->ctl_in_ports[ctl_num]) = default_value;
	} else { /* If property exists check if it matches and don't set default */
		const gchar *name = xmms_config_property_get_string (property_name);
		g_assert (ctl_num < node->num_ctl_in_ports);
		if (g_strcmp0 (port_name, name) != 0) { /* only set value if name does not match, to keep old value */
			xmms_config_property_set_data (property, default_value_str);
			*(node->ctl_in_ports[ctl_num]) = default_value;
		} else {
			gfloat value = xmms_config_property_get_float (property);
			*(node->ctl_in_ports[ctl_num]) = value;
		}
	}
	xmms_config_property_set_data (property_name, port_name);
	g_free (default_value_str);
	g_free (value_property_name);
	g_free (name_property_name);
}
