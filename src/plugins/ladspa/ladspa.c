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
#include "ladspa.h"

#include <dlfcn.h>
#include <glib.h>
#include <math.h>

static gboolean xmms_ladspa_plugin_setup (xmms_xform_plugin_t *xform_plugin);
static gboolean xmms_ladspa_init (xmms_xform_t *xform);
static void xmms_ladspa_destroy (xmms_xform_t *xform);
static gint xmms_ladspa_read (xmms_xform_t *xform, xmms_sample_t *buf, gint len,
                              xmms_error_t *error);
static gint64 xmms_ladspa_seek (xmms_xform_t *xform, gint64 offset,
                                xmms_xform_seek_mode_t whence, xmms_error_t *err);

struct ladspa_plugin_node_St {
    const LADSPA_Descriptor *plugin;
	LADSPA_Handle instance; /* TODO allow multiple intances for parallel plugins mono->stereo, etc. */
	LADSPA_Data *ctl_in_ports;
	LADSPA_Data *ctl_out_ports;
	guint num_in_channels;
	guint num_out_channels;
	gfloat **in_bufs; /* de-interleaved buffers */
	gfloat **out_bufs;
	struct ladspa_plugin_node_St *next;
};

typedef struct ladspa_plugin_node_St ladspa_plugin_node_t;

#define XMMS_DEFAULT_BUFFER_SIZE 4096

typedef struct ladspa_data_St {
    gboolean enabled;
	guint buf_size; /* size of de-interleaved buffers */
    ladspa_plugin_node_t *plugin_list;
} ladspa_data_t;

static gboolean ladspa_load_lib (ladspa_data_t *priv, const gchar *pluginlib,
                                 const gchar *pluginname, gint srate);
static void ladspa_config_changed (xmms_object_t *object, xmmsv_t *data, gpointer userdata);

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
	xmms_xform_plugin_config_property_register (xform_plugin, "pluginlib",
	                                            "/usr/lib/ladspa/tap_tubewarmth.so",
	                                            NULL, NULL);
	xmms_xform_plugin_config_property_register (xform_plugin, "pluginname", "TAP TubeWarmth",
	                                            NULL, NULL);
	/* TODO remove this */
	xmms_xform_plugin_config_property_register (xform_plugin, "parameter0", "0",
	                                            NULL, NULL);
	xmms_xform_plugin_config_property_register (xform_plugin, "parameter1", "0",
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
	const gchar *pluginlib;
	const gchar *pluginname;
	gint srate;
	gint num_channels;
	gint fmt;
	guint i;
	guint buf_size;
	gboolean ret;

	g_return_val_if_fail (xform, FALSE);

	priv = g_new0 (ladspa_data_t, 1);
	g_return_val_if_fail (priv, FALSE);

	xmms_xform_private_data_set (xform, priv);
	xmms_xform_outdata_type_copy (xform);

	srate = xmms_xform_indata_get_int (xform, XMMS_STREAM_TYPE_FMT_SAMPLERATE);
	num_channels
		= xmms_xform_indata_get_int (xform, XMMS_STREAM_TYPE_FMT_CHANNELS);
	fmt = xmms_xform_indata_get_int (xform, XMMS_STREAM_TYPE_FMT_FORMAT);

	priv->buf_size = XMMS_DEFAULT_BUFFER_SIZE/(xmms_sample_size_get (fmt) * num_channels);

	/* TODO can this be allocated in setup, to avoid allocation here? */
	buf_size = priv->buf_size;

	/* TODO check config options and apply them */
	/* Set callbacks for options that can be changed during playback
	   otherwise check the value and apply here */
	/* Also make sure all config options are cleaned up in xmms_XX_destroy */
	config = xmms_xform_config_lookup (xform, "enabled");
	g_return_val_if_fail (config, FALSE);
	xmms_config_property_callback_set (config, ladspa_config_changed, priv);
	priv->enabled = !!xmms_config_property_get_int (config);

	config = xmms_xform_config_lookup (xform, "pluginlib");
	g_return_val_if_fail (config, FALSE);
	xmms_config_property_callback_set (config, ladspa_config_changed, priv);
	pluginlib = xmms_config_property_get_string (config);

	config = xmms_xform_config_lookup (xform, "pluginname");
	g_return_val_if_fail (config, FALSE);
	xmms_config_property_callback_set (config, ladspa_config_changed, priv);
	pluginname = xmms_config_property_get_string (config);

	config = xmms_xform_config_lookup (xform, "parameter0");
	g_return_val_if_fail (config, FALSE);
	xmms_config_property_callback_set (config, ladspa_config_changed, priv);

	config = xmms_xform_config_lookup (xform, "parameter1");
	g_return_val_if_fail (config, FALSE);
	xmms_config_property_callback_set (config, ladspa_config_changed, priv);

	/* TODO un-hardwire this */
	/* TODO if only a single plugin in a library, load it always and warn */
	/* ret = ladspa_load_lib(priv, "/usr/lib/ladspa/tap_tubewarmth.so", */
	/* 					  "tap_tubewarmth", srate); */
	/* ret = ladspa_load_lib(priv, "/usr/lib/ladspa/gverb_1216.so", */
	/* 					  "gverb", srate); */
	ret = ladspa_load_lib (priv, "/usr/lib/ladspa/amp.so",
	                       "amp_stereo", srate);
	/* ret = ladspa_load_lib(priv, "/usr/lib/ladspa/noise.so", */
	/* 					  "noise_white", srate); */

	return ret;
}

static void
xmms_ladspa_destroy (xmms_xform_t *xform)
{
	xmms_config_property_t *config;
	ladspa_data_t *priv;
	ladspa_plugin_node_t *old_node;
	ladspa_plugin_node_t *plugin_node;
	/* gchar buf[16]; */
	gint i;

	g_return_if_fail (xform);

	priv = (ladspa_data_t *) xmms_xform_private_data_get (xform);
	while (plugin_node != NULL) {
		if (plugin_node->plugin->deactivate) {
			plugin_node->plugin->deactivate (plugin_node->instance);
		}
		plugin_node->plugin->cleanup (plugin_node->instance);
		/* TODO is there a need to free something else from ladspa? */
		old_node = plugin_node;
		plugin_node = plugin_node->next;
		g_free (old_node);
		for (i = 0; i < plugin_node->num_in_channels; i++) {
			g_free (plugin_node->in_bufs[i]);
		}
		for (i = 0; i < plugin_node->num_in_channels; i++) {
			g_free (plugin_node->in_bufs[i]);
		}
		g_free (plugin_node->out_bufs);
		g_free (plugin_node->in_bufs);
	}

	/* Free config options */
	config = xmms_xform_config_lookup (xform, "enabled");
	xmms_config_property_callback_remove (config, ladspa_config_changed, priv);
	config = xmms_xform_config_lookup (xform, "pluginlib");
	xmms_config_property_callback_remove (config, ladspa_config_changed, priv);
	config = xmms_xform_config_lookup (xform, "pluginname");
	xmms_config_property_callback_remove (config, ladspa_config_changed, priv);

	g_free (priv);
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
	gint i;
	gint j;
	ladspa_plugin_node_t *plugin_node;
	ladspa_plugin_node_t *last_node;
	g_return_val_if_fail (xform, -1);

	priv = xmms_xform_private_data_get (xform);
	g_return_val_if_fail (priv, -1);

	plugin_node = priv->plugin_list;

	read = xmms_xform_read (xform, buf, len, error);
	chans = xmms_xform_indata_get_int (xform, XMMS_STREAM_TYPE_FMT_CHANNELS);
	fmt = xmms_xform_indata_get_int (xform, XMMS_STREAM_TYPE_FMT_FORMAT);

	if (!priv->enabled || plugin_node == NULL || read <= 0) {
		return read;
	}

	buf_size = XMMS_DEFAULT_BUFFER_SIZE/(xmms_sample_size_get (fmt) * chans);

	/* if buffer size or number of channels has changed, must reallocate */
	if (buf_size != priv->buf_size) {
		xmms_log_info ("Reallocating ladspa buffers to size %i", buf_size);
		/* TODO reallocate buffers */
		/* TODO need to reconnect ladspa ports when reallocating */
	}
	/* de-interleave input */
	for (i = 0; i < buf_size; i++) {
		for (j = 0; j < chans; j++) {
			/* TODO don't make this check on every sample */
			if (fmt == XMMS_SAMPLE_FORMAT_S16) {
				plugin_node->in_bufs[j][i]
					= (gfloat) ( ((gint16 *) buf)[j + (i*chans)] / 32768.0);
			} else if (fmt == XMMS_SAMPLE_FORMAT_FLOAT) {
				plugin_node->in_bufs[j][i] = ( (gfloat *) buf)[j + (i*chans)];
			}
		}
	}
	while (plugin_node != NULL) {
		plugin_node->plugin->run (plugin_node->instance, buf_size);
		if (plugin_node->next != NULL) { /* Copy output to input for next plugin */
			for (i = 0; i < buf_size; i++) {
				for (j = 0; j < chans; j++) {
					/* TODO must make sure all plugins have the same number of buffers */
					plugin_node->next->in_bufs[j][i] = plugin_node->out_bufs[j][i];
				}
			}
		}
		last_node = plugin_node;
		plugin_node = plugin_node->next;
	}
	/* interleave output */
	for (i = 0; i < buf_size; i++) {
		for (j = 0; j < chans; j++) {
			if (fmt == XMMS_SAMPLE_FORMAT_S16) {
				/* clip output */
				if (last_node->out_bufs[j][i] > 1.0) {
					((gint16 *) buf)[j + (i*chans)] = 32767;
				} else if (last_node->out_bufs[j][i] < -1.0) {
					((gint16 *) buf)[j + (i*chans)] = -32768;
				} else {
					((gint16 *) buf)[j + (i*chans)]
						= (gint16) (last_node->out_bufs[j][i] * 32768.0);
				}
			} else if (fmt == XMMS_SAMPLE_FORMAT_FLOAT) {
				( (gfloat *) buf)[j + (i*chans)] = last_node->out_bufs[j][i];
			}
		}
	}
	return read;
}

static gint64
xmms_ladspa_seek (xmms_xform_t *xform, gint64 offset, xmms_xform_seek_mode_t whence, xmms_error_t *err)
{
	return xmms_xform_seek (xform, offset, whence, err);
}

static void
ladspa_config_changed (xmms_object_t *object, xmmsv_t *data, gpointer userdata)
{
	/* TODO change config on the fly */
	ladspa_data_t *priv = userdata;
	const gchar *name;
	gfloat value;

	name = xmms_config_property_get_name ((xmms_config_property_t *) object);
	value = xmms_config_property_get_float ((xmms_config_property_t *) object);

	xmms_log_info ("property %s", name);
	/* TODO connect parameters properly */
	/* TODO will only be active if xform chain is connected */
	if (!g_ascii_strcasecmp (name, "ladspa.pluginlib")) {
		xmms_log_info ("ladspa.pluginlib change not supported");
	} else if (!g_ascii_strcasecmp (name, "ladspa.pluginname")) {
		xmms_log_info ("ladspa.pluginname change not supported");
	} else if (!g_ascii_strcasecmp (name, "ladspa.parameter0")) {
		xmms_log_info ("value was %f", priv->plugin_list->ctl_in_ports[0]);
		priv->plugin_list->ctl_in_ports[0] = value;
		xmms_log_info ("value set to %f", value);
	} else if (!g_ascii_strcasecmp (name, "ladspa.parameter1")) {
		priv->plugin_list->ctl_in_ports[1] = value;
	}

	/* TODO validate ladspa plugin parameters with:
	   LADSPA_IS_HINT_BOUNDED_BELOW(x)
	   LADSPA_IS_HINT_BOUNDED_ABOVE(x)
	   #define LADSPA_IS_HINT_TOGGLED(x)
	   LADSPA_IS_HINT_INTEGER(x) */
}

static gboolean
ladspa_load_lib (ladspa_data_t *priv, const gchar *pluginlib,
                 const gchar *pluginname, gint srate)
{
	const LADSPA_Descriptor *descriptor;
	LADSPA_Descriptor_Function descriptor_function;
	LADSPA_Handle instance;
	const LADSPA_PortDescriptor * PortDescriptors;
	const char * const * PortNames;
	const LADSPA_PortRangeHint * PortRangeHints;
	ladspa_plugin_node_t *node;
	void *dl;
	guint out_count, in_count, ctl_in_port_count, ctl_out_port_count;
	gulong index;
	guint i;

	/* Load ladspa library */
	if (!priv->plugin_list) { /* first ladspa plugin */
		priv->plugin_list
			= (ladspa_plugin_node_t *) g_malloc (sizeof (ladspa_plugin_node_t) );
		node = priv->plugin_list;
		node->next = NULL;
	} else {
		node = priv->plugin_list;
		while (node->next != NULL) { /* Find last node in list */
			node = node->next;
		}
		node->next = (ladspa_plugin_node_t *) g_malloc (sizeof (ladspa_plugin_node_t) );
		node = node->next;
	}
	/* TODO search in LADSPA_PATH and default known paths */
	dl = dlopen (pluginlib, RTLD_LAZY);
	if (dl == NULL) {
		xmms_log_error ("Couldn't open library %s", pluginlib);
		return FALSE;
	}
	descriptor_function =
		(LADSPA_Descriptor_Function) dlsym (dl, "ladspa_descriptor");
	if (descriptor_function == NULL) {
		xmms_log_error ("Could not find descriptor in library %s", pluginlib);
		return FALSE;
	}
	for (index = 0;; index++) {
		descriptor = descriptor_function (index);
		if (descriptor == NULL) {
			/* TODO Report error finding plugin in library and exit */
			xmms_log_error ("Plugin '%s' not found in library '%s'",
			                pluginname,
			                pluginlib);
			return FALSE;
		}
		if (g_strcmp0 (descriptor->Label,pluginname) == 0
		    || g_strcmp0 (descriptor->Name,pluginname) == 0) {
			break; /* allow matching either Name or Label for convenience */
		}
	}
	node->plugin = descriptor;
	instance = node->instance
		= descriptor->instantiate (descriptor, srate);
	/* examine ports and connect */
	PortDescriptors = descriptor->PortDescriptors;
	PortNames = descriptor->PortNames;
	PortRangeHints = descriptor->PortRangeHints;
	ctl_in_port_count = ctl_out_port_count = out_count = in_count = 0;
	for (i = 0; i < descriptor->PortCount; i++) {
		if (LADSPA_IS_PORT_CONTROL (PortDescriptors[i])) {
			gchar default_value_str[128];
			LADSPA_Data lower, upper, default_value;
			LADSPA_PortRangeHintDescriptor range_hints;
			if (LADSPA_IS_PORT_INPUT (PortDescriptors[i])) {
				ctl_in_port_count++;
			} else if (LADSPA_IS_PORT_OUTPUT (PortDescriptors[i])) {
				ctl_out_port_count++;
			}
			xmms_log_info ("Control Port %i-%s",i, PortNames[i]);
			lower = PortRangeHints[i].LowerBound;
			upper = PortRangeHints[i].UpperBound;
			range_hints = PortRangeHints[i].HintDescriptor;
			default_value = 0.0;
			if (LADSPA_IS_HINT_HAS_DEFAULT (range_hints)) {
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
				sprintf (default_value_str, "%.6f", default_value);
			}
			/* TODO register plugin parameters as server configs, but how from here? */
			/* xmms_xform_plugin_config_property_register ( */
			/* 	(xmms_xform_plugin_t *) (xform->plugin), PortNames[i], */
			/* 	"test", */
			/* 	NULL, NULL);  /\* TODO set callback *\/ */
			/* TODO register config proerties and pass the default values */
			/* xmms_config_property_register (PortNames[i], */
			/* 							   "test", */
			/* 							   NULL, NULL); */
		} else if (LADSPA_IS_PORT_AUDIO (PortDescriptors[i])) {
			xmms_log_info ("Audio Port %i-%s",i, PortNames[i]);
			if (LADSPA_IS_PORT_INPUT (PortDescriptors[i])) {
				in_count++; /* TODO use this information to setup connections and parallel plugins */
			} else if (LADSPA_IS_PORT_OUTPUT (PortDescriptors[i])) {
				out_count++;
			}
		}
	}
	node->ctl_in_ports
		= (LADSPA_Data *) g_malloc (ctl_in_port_count * sizeof (LADSPA_Data));
	node->ctl_out_ports
		= (LADSPA_Data *) g_malloc (ctl_out_port_count * sizeof (LADSPA_Data));
	node->in_bufs
		= (gfloat **) g_malloc (in_count * sizeof (gfloat*) );
	node->out_bufs
		= (gfloat **) g_malloc (out_count * sizeof (gfloat*) );
	for (i = 0; i < in_count; i++) {
		node->in_bufs[i]
			= (gfloat *) g_malloc (priv->buf_size * sizeof (float) );
	}
	for (i = 0; i < out_count; i++) {
		node->out_bufs[i]
			= (gfloat *) g_malloc (priv->buf_size * sizeof (float) );
	}
	/* now connect the ports */
	out_count = in_count = ctl_in_port_count = ctl_out_port_count = 0;
	for (i = 0; i < descriptor->PortCount; i++) {
		if (LADSPA_IS_PORT_CONTROL (PortDescriptors[i])) {
			if (LADSPA_IS_PORT_INPUT (PortDescriptors[i])) {
				descriptor->connect_port (instance, i,
				                          & (node->ctl_in_ports[ctl_in_port_count]) );
				ctl_in_port_count++;
			} else if (LADSPA_IS_PORT_OUTPUT (PortDescriptors[i])) {
				descriptor->connect_port (instance, i,
				                          & (node->ctl_out_ports[ctl_out_port_count]) );
				ctl_out_port_count++;
			}
		} else if (LADSPA_IS_PORT_AUDIO (PortDescriptors[i])) {
			/* TODO duplicate plugin when it is mono and xform chain is stereo */
			if (LADSPA_IS_PORT_INPUT (PortDescriptors[i])) {
				descriptor->connect_port (instance, i,
				                          node->in_bufs[in_count]);
				in_count++;
			} else if (LADSPA_IS_PORT_OUTPUT (PortDescriptors[i])) {
				descriptor->connect_port (instance, i,
				                          node->out_bufs[out_count]);
				out_count++;
			}
		}
	}
	if (descriptor->activate) {
		descriptor->activate (instance);
	}

	xmms_log_info ("Plugin '%s' from '%s'. Init OK.", descriptor->Name, pluginlib);
	return TRUE;
}
