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

typedef enum {
	LADSPA_DIRECT, /* Plugin inputs and outputs match */
	LADSPA_MONO, /* Plugin is mono and has been instantiated for each input channel */
	LADSPA_BALANCE, /* Plugin input is mono, output stereo and chain is stereo */
	LADSPA_OTHER /* any other configuration, plugin is instantiated to match inputs and
	                 the outputs are put one after the other discarding the last ones if
	                 needed */
} LADSPA_Mode;

struct ladspa_plugin_node_St {
    const LADSPA_Descriptor *plugin;
	gint num_instances;
	LADSPA_Mode mode;
	float balance; /* Used only for mode LADSPA_BALANCE */
	LADSPA_Handle *instance;
	guint num_ctl_in_ports;
	guint num_ctl_out_ports;
	LADSPA_Data *ctl_in_ports;
	LADSPA_Data *ctl_out_ports;
	gfloat **temp_bufs; /* all plugins have independent output buffers */
	guint num_in_channels;
	guint num_out_channels;
	struct ladspa_plugin_node_St *next;
};

typedef struct ladspa_plugin_node_St ladspa_plugin_node_t;

#define XMMS_DEFAULT_BUFFER_SIZE 4096

typedef struct ladspa_data_St {
    gboolean enabled;
	gint num_channels; /* number of channels in the xform chain */
	gfloat **in_bufs; /* de-interleaved buffers */
	gfloat **out_bufs;
	guint buf_size; /* size of de-interleaved buffers */
    ladspa_plugin_node_t *plugin_list;
} ladspa_data_t;

static gboolean ladspa_init_plugin (ladspa_data_t *priv, const gchar *pluginlib,
                                 const gchar *pluginname, gint srate);
static void ladspa_config_changed (xmms_object_t *object, xmmsv_t *data, gpointer userdata);
static void ladspa_allocate_buffers (ladspa_data_t *priv, gint num_channels, gint buf_size);
static void *ladspa_load_lib (const gchar* pluginlib);

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
	/* TODO change defaults */
	/* xmms_xform_plugin_config_property_register (xform_plugin, "pluginlib", */
	/*                                             "/usr/lib/ladspa/tap_tubewarmth.so", */
	/*                                             NULL, NULL); */
	/* xmms_xform_plugin_config_property_register (xform_plugin, "pluginname", "TAP TubeWarmth", */
	/*                                             NULL, NULL); */
	xmms_xform_plugin_config_property_register (xform_plugin, "pluginlib",
	                                            "amp.so",
	                                            NULL, NULL);
	xmms_xform_plugin_config_property_register (xform_plugin, "pluginname", "amp_stereo",
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
	gint srate, num_channels, fmt;
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

	priv->num_channels = num_channels;
	priv->buf_size = XMMS_DEFAULT_BUFFER_SIZE/(xmms_sample_size_get (fmt) * num_channels);
	/* TODO can this be allocated in setup, to avoid allocation here? */
	buf_size = priv->buf_size;
	ladspa_allocate_buffers(priv, num_channels, buf_size);

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

	ret = ladspa_init_plugin(priv, pluginlib, pluginname, srate);

	return ret;
}

static void
xmms_ladspa_destroy (xmms_xform_t *xform)
{
	xmms_config_property_t *config;
	ladspa_data_t *priv;
	ladspa_plugin_node_t *old_node;
	ladspa_plugin_node_t *plugin_node;
	gchar *property_name;
	gint i;

	g_return_if_fail (xform);

	priv = (ladspa_data_t *) xmms_xform_private_data_get (xform);
	if (priv->in_bufs && priv->out_bufs) {
		for (i = 0; i < priv->num_channels; i++) {
			g_free (priv->in_bufs[i]);
			g_free (priv->out_bufs[i]);
		}
		g_free(priv->in_bufs);
		g_free(priv->out_bufs);
	}
	plugin_node = priv->plugin_list;
	while (plugin_node != NULL) {
		for (i = 0; i < plugin_node->num_instances; i++) {
			if (plugin_node->plugin->deactivate) {
				plugin_node->plugin->deactivate (plugin_node->instance[i]);
			}
			plugin_node->plugin->cleanup (plugin_node->instance[i]);
			if (plugin_node->temp_bufs) {
				for (i = 0; i < priv->num_channels; i++) {
					g_free (plugin_node->temp_bufs[i]);
				}
				g_free(plugin_node->temp_bufs);
			}
		}
		g_free (plugin_node->ctl_in_ports);
		g_free (plugin_node->ctl_out_ports);
		old_node = plugin_node;
		plugin_node = plugin_node->next;
		g_free (old_node);
	}

	/* Free global config options */
	config = xmms_xform_config_lookup (xform, "enabled");
	xmms_config_property_callback_remove (config, ladspa_config_changed, priv);
	config = xmms_xform_config_lookup (xform, "pluginlib");
	xmms_config_property_callback_remove (config, ladspa_config_changed, priv);
	config = xmms_xform_config_lookup (xform, "pluginname");
	xmms_config_property_callback_remove (config, ladspa_config_changed, priv);

	/* Free any remaining plugin config */
	i = 0;
	property_name = g_strdup_printf ("control.%i", i);;
	config = xmms_xform_config_lookup (xform, property_name);
	g_free (property_name);
	while (config != NULL) {
		/* xmms_config_property_destroy (config); */
		/* property_name = g_strdup_printf ("controlname.%i", i);; */
		/* config = xmms_xform_config_lookup (xform, property_name); */
		/* xmms_config_property_destroy (config); */
	}
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
	gint i, j, max_out;
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
	if (buf_size != priv->buf_size || chans != priv->num_channels) {
		xmms_log_info ("Reallocating ladspa buffers to size %i", buf_size);
		ladspa_allocate_buffers (priv, chans, buf_size);
		priv->buf_size = buf_size;
		priv->num_channels = chans;
		/* TODO need to reconnect ladspa ports when reallocating */
	}
	/* de-interleave input */
	if (fmt == XMMS_SAMPLE_FORMAT_S16) {
		for (i = 0; i < buf_size; i++) {
			for (j = 0; j < chans; j++) {
				priv->in_bufs[j][i]
					= (gfloat) ( ((gint16 *) buf)[j + (i*chans)] / 32768.0);
			}
		}
	} else if (fmt == XMMS_SAMPLE_FORMAT_FLOAT) {
		for (i = 0; i < buf_size; i++) {
			for (j = 0; j < chans; j++) {
				priv->in_bufs[j][i] = ( (gfloat *) buf)[j + (i*chans)];
			}
		}
	}
	while (plugin_node != NULL) {
		switch (plugin_node->mode) {
			case LADSPA_DIRECT:
				plugin_node->plugin->run (plugin_node->instance[0], buf_size);
				break;
			case LADSPA_MONO:
				g_assert(plugin_node->num_instances == priv->num_channels);
				for (i = 0; i < plugin_node->num_instances; i++) {
					plugin_node->plugin->run (plugin_node->instance[i], buf_size);
					/* no need to copy the temp buffers */
				}
				break;
			case LADSPA_BALANCE:
				g_assert(priv->num_channels == 2 && plugin_node->num_out_channels == 2);
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
		}
		if (plugin_node->next != NULL) { /* Copy output to input for next plugin */
			for (i = 0; i < buf_size; i++) {
				for (j = 0; j < plugin_node->num_out_channels; j++) {
					priv->in_bufs[j][i] = priv->out_bufs[j][i];
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
				if (last_node->temp_bufs[j][i] > 1.0) {
					((gint16 *) buf)[j + (i*chans)] = 32767;
				} else if (last_node->temp_bufs[j][i] < -1.0) {
					((gint16 *) buf)[j + (i*chans)] = -32768;
				} else {
					((gint16 *) buf)[j + (i*chans)]
						= (gint16) (last_node->temp_bufs[j][i] * 32768.0);
				}
			} else if (fmt == XMMS_SAMPLE_FORMAT_FLOAT) {
				( (gfloat *) buf)[j + (i*chans)] = last_node->temp_bufs[j][i];
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

	/* TODO connect parameters properly */
	/* TODO will only be active if xform chain is connected */
	if (!g_ascii_strcasecmp (name, "ladspa.pluginlib")) {
		/* TODO allow changing plugin while chain exists */
		xmms_log_info ("ladspa.pluginlib change not supported!");
	} else if (!g_ascii_strcasecmp (name, "ladspa.pluginname")) {
		xmms_log_info ("ladspa.pluginname change not supported!");
	} else if (g_str_has_prefix (name, "ladspa.control.") ) {
		xmms_log_info ("Change control %lu", (gulong) g_ascii_strtoull (name+15, NULL, 10) );
		/* xmms_log_info ("value was %f", priv->plugin_list->ctl_in_ports[0]); */
		/* priv->plugin_list->ctl_in_ports[0] = value; */
		/* xmms_log_info ("value set to %f", value); */
	}
	xmms_log_info ("Change control %lu", (gulong) g_ascii_strtoull (name+15, NULL, 10) );

	/* TODO validate ladspa plugin parameters with:
	   LADSPA_IS_HINT_BOUNDED_BELOW(x)
	   LADSPA_IS_HINT_BOUNDED_ABOVE(x)
	   #define LADSPA_IS_HINT_TOGGLED(x)
	   LADSPA_IS_HINT_INTEGER(x) */
}

static void *
ladspa_load_lib (const gchar *pluginlib)
{
	void *dl;
	gboolean exists;
	char *libname;
	const char * LADSPA_path;
	gchar ** paths, **p;
	gchar default_path[] = "/usr/lib/ladspa"; /* TODO set different for windows */
	g_assert(pluginlib != NULL);
	if (strlen (pluginlib) == 0) {
		return NULL;
	}
	if (pluginlib[0] == '/') { /* absolute path */
		libname = g_strdup (pluginlib);
	} else { /* relative path */
		LADSPA_path = g_getenv ("LADSPA_PATH");
		if (!LADSPA_path || strlen (LADSPA_path) == 0) {
			paths = g_new(gchar *, 2);
			paths[0] = g_new(gchar, (strlen(default_path) + 1) );
			paths[1] = NULL;
			strncpy(paths[0], default_path, strlen(default_path));
		} else {
			gchar separator[] = ":";
			paths = g_strsplit( LADSPA_path, separator, 16); /* 16 paths should be plenty */
		}
		p = paths;
		exists = FALSE;
		while (p != NULL) {
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
			g_free(libname);
			p++;
		}
		g_strfreev(paths);
	}
	xmms_log_info("Attempting to load library %s", libname);
	dl = dlopen (libname, RTLD_LAZY);
	g_free(libname);
	/* TODO use G_MODULE_SUFFIX to find lib name without extension */
	return dl;
}

static gboolean
ladspa_init_plugin (ladspa_data_t *priv, const gchar *pluginlib,
                 const gchar *pluginname, gint srate)
{
	const LADSPA_Descriptor *descriptor;
	LADSPA_Descriptor_Function descriptor_function;
	const LADSPA_PortDescriptor * PortDescriptors;
	const char * const * PortNames;
	const LADSPA_PortRangeHint * PortRangeHints;
	ladspa_plugin_node_t *node;
	xmms_config_property_t *property;
	gchar *default_value_str;
	gchar * property_name;
	void *dl;
	guint out_count, in_count, ctl_in_port_count, ctl_out_port_count;
	gulong index;
	guint i, inst, plugin_count;

	/* Find last plugin node and allocate */
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

	/* Load ladspa library */
	dl = ladspa_load_lib (pluginlib);
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
	if (plugin_count == 1) {
		descriptor = descriptor_function (0);
		xmms_log_info("Auto-loaded plugin %s from %s", descriptor->Label, pluginlib);
	} else {
		for (index = 0;; index++) {
			descriptor = descriptor_function (index);
			if (descriptor == NULL) {
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
	}
	node->plugin = descriptor;
	/* Examine ports */
	PortDescriptors = descriptor->PortDescriptors;
	PortNames = descriptor->PortNames;
	PortRangeHints = descriptor->PortRangeHints;
	ctl_in_port_count = ctl_out_port_count = out_count = in_count = 0;
	for (i = 0; i < descriptor->PortCount; i++) {
		if (LADSPA_IS_PORT_CONTROL (PortDescriptors[i])) {
			LADSPA_Data lower, upper, default_value;
			LADSPA_PortRangeHintDescriptor range_hints;
			xmms_log_info ("Port %i(Control)-%s",i, PortNames[i]);
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
				if (LADSPA_IS_PORT_INPUT (PortDescriptors[i])) {
					default_value_str = g_strdup_printf ("%.6f", default_value);
					property_name = g_strdup_printf ("ladspa.control.%i",
					                                 ctl_in_port_count, NULL);
					property = xmms_config_property_register (property_name,
					                                          default_value_str,
					                                          ladspa_config_changed,
					                                          priv);
					xmms_config_property_callback_set (property, ladspa_config_changed, priv);
					/* TODO release these config callbacks */
					g_free (default_value_str);
					g_free (property_name);
					property_name = g_strdup_printf ("ladspa.controlname.%i",
					                                 ctl_in_port_count, NULL);
					property = xmms_config_property_register (property_name,
					                                          PortNames[i],
					                                          ladspa_config_changed,
					                                          priv);
					xmms_config_property_callback_set (property, ladspa_config_changed, priv);
					/* TODO release these config callbacks */
					g_free (property_name);
					ctl_in_port_count++;
				} else if (LADSPA_IS_PORT_OUTPUT (PortDescriptors[i])) {
					default_value_str = g_strdup_printf ("%.6f", default_value);
					property_name = g_strdup_printf ("ladspa.display.%i",
					                                 ctl_out_port_count, NULL);
					property = xmms_config_property_register (property_name,
					                                          default_value_str,
					                                          ladspa_config_changed,
					                                          priv);
					g_free (default_value_str);
					g_free (property_name);
					property_name = g_strdup_printf ("ladspa.displayname.%i",
					                                 ctl_out_port_count, NULL);
					property = xmms_config_property_register (property_name,
					                                          PortNames[i],
					                                          ladspa_config_changed,
					                                          priv);
					g_free (property_name);
					ctl_out_port_count++;
				}
			}
		} else if (LADSPA_IS_PORT_AUDIO (PortDescriptors[i])) {
			xmms_log_info ("Port %i (Audio)-%s",i, PortNames[i]);
			if (LADSPA_IS_PORT_INPUT (PortDescriptors[i])) {
				in_count++; /* TODO use this information to setup connections and parallel plugins */
			} else if (LADSPA_IS_PORT_OUTPUT (PortDescriptors[i])) {
				out_count++;
			}
		}
	}
	/* Instantiate depending on the number of I/O channels */
	node->num_in_channels = in_count;
	node->num_out_channels = out_count;
	if (in_count == out_count && out_count == priv->num_channels) {
		node->instance = (LADSPA_Handle *) g_malloc (sizeof(LADSPA_Handle));
		node->instance[0] = descriptor->instantiate (descriptor, srate);
		node->num_instances = 1;
		node->mode = LADSPA_DIRECT; /* LADSPA_DIRECT */
	}
	else if (in_count == 1 && out_count == 1) {
		node->instance
			= (LADSPA_Handle *) g_new (LADSPA_Handle, priv->num_channels);
		node->num_instances = priv->num_channels;
		node->mode = LADSPA_MONO; /* LADSPA_MONO */
		out_count = priv->num_channels; /* To force creation of temp_bufs */
		for (i = 0; i < priv->num_channels; i++) {
			node->instance[i] = descriptor->instantiate (descriptor, srate);
		}
	}
	else if (in_count == 1 && out_count == 2 && priv->num_channels == 2) {
		node->instance
			= (LADSPA_Handle *) g_new (LADSPA_Handle, priv->num_channels);;
		node->num_instances = priv->num_channels;
		node->mode = LADSPA_BALANCE; /* LADSPA_MONO */
		for (i = 0; i < priv->num_channels; i++) {
			node->instance[i] = descriptor->instantiate (descriptor, srate);
		}
	}
	else { /* LADSPA_OTHER */
		gint nin, nout, n;
		/* determine the minimum number of instances needed */
		nin = (priv->num_channels > in_count ?
		       ceil (priv->num_channels/in_count) : 1 );
		nout = (priv->num_channels > out_count ?
		       ceil (priv->num_channels/out_count) : 1 );
		n = (nin > nout ? nin : nout);
		node->num_instances = n;
		node->mode = LADSPA_OTHER;
		node->instance
			= (LADSPA_Handle *) g_new (LADSPA_Handle, n);
		for (i = 0; i < n; i++) {
			node->instance[i] = descriptor->instantiate (descriptor, srate);
		}
	}
	node->ctl_in_ports
		= (LADSPA_Data *) g_new (LADSPA_Data, ctl_in_port_count);
	node->num_ctl_in_ports = ctl_in_port_count;
	node->ctl_out_ports
		= (LADSPA_Data *) g_new (LADSPA_Data, ctl_out_port_count);
	node->num_ctl_out_ports = ctl_out_port_count;
	node->temp_bufs
		= (gfloat **) g_new (gfloat *, out_count );
	for (i = 0; i < out_count; i++) {
		node->temp_bufs[i]
			= (gfloat *) g_new (gfloat *, priv->buf_size );
	}
	/* now connect the ports */
	for (inst = 0; inst < node->num_instances; inst++) {
		out_count = in_count = ctl_in_port_count = ctl_out_port_count = 0;
		for (i = 0; i < descriptor->PortCount; i++) {
			if (LADSPA_IS_PORT_CONTROL (PortDescriptors[i])) {
				if (LADSPA_IS_PORT_INPUT (PortDescriptors[i])) {
					descriptor->connect_port (node->instance[inst], i,
					                          & (node->ctl_in_ports[ctl_in_port_count]) );
					ctl_in_port_count++;
				} else if (LADSPA_IS_PORT_OUTPUT (PortDescriptors[i])) {
					descriptor->connect_port (node->instance[inst], i,
					                          & (node->ctl_out_ports[ctl_out_port_count]) );
					ctl_out_port_count++;
				}
			} else if (LADSPA_IS_PORT_AUDIO (PortDescriptors[i])) {
				if (LADSPA_IS_PORT_INPUT (PortDescriptors[i])) {
					/* all instances share the in buffer */
					if (node->mode == LADSPA_MONO) {
						descriptor->connect_port (node->instance[inst], i,
						                          priv->in_bufs[inst] );
					} else {
						descriptor->connect_port (node->instance[inst], i,
						                          priv->in_bufs[in_count % priv->num_channels]);
					}
					in_count++;
				} else if (LADSPA_IS_PORT_OUTPUT (PortDescriptors[i])) {
					if (node->mode == LADSPA_MONO) {
						descriptor->connect_port (node->instance[inst], i,
						                          node->temp_bufs[inst] );
					} else {
						descriptor->connect_port (node->instance[inst], i,
						                          node->temp_bufs[out_count]);
					}
					out_count++;
				}
			}
		}
		if (descriptor->activate) {
			descriptor->activate (node->instance[inst]);
		}
	}
	if (out_count == 0) {
		xmms_log_error("Plugin '%s' has no outputs. Not enabled!", descriptor->Name);
		return FALSE;
	}
	xmms_log_info ("Plugin '%s' from '%s'. Init OK. Mode %i",
	               descriptor->Name, pluginlib, node->mode);
	return TRUE;
}

static void
ladspa_allocate_buffers(ladspa_data_t *priv, gint num_channels, gint buf_size)
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
	priv->in_bufs
		= (gfloat **) g_new (gfloat*, num_channels);
	priv->out_bufs
		= (gfloat **) g_new (gfloat*, num_channels);
	for (i = 0; i < num_channels; i++) {
		priv->in_bufs[i]
			= (gfloat *) g_new (gfloat, buf_size);
		priv->out_bufs[i]
			= (gfloat *) g_new (gfloat, buf_size);
	}
}
