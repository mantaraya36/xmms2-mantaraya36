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
#include <stdlib.h>

static gboolean xmms_ladspa_plugin_setup (xmms_xform_plugin_t *xform_plugin);
static gboolean xmms_ladspa_init (xmms_xform_t *xform);
static void xmms_ladspa_destroy (xmms_xform_t *xform);
static gint xmms_ladspa_read (xmms_xform_t *xform, xmms_sample_t *buf, gint len,
                              xmms_error_t *error);

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
	LADSPA_Data **ctl_in_ports;
	gfloat **temp_bufs; /* temporary buffers for BALANCE and OTHER modes */
	guint num_in_channels;
	guint num_out_channels;
	struct ladspa_plugin_node_St *next;
};

typedef struct ladspa_plugin_node_St ladspa_plugin_node_t;

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

static LADSPA_Data get_ladspa_default_value (const LADSPA_PortRangeHint * PortRangeHints,
                                             gint port_num);
static void xmms_ladspa_reallocate_buffers (ladspa_data_t *priv, gint new_size, gint new_chans);

static gboolean ladspa_init_plugin (ladspa_data_t *priv, const gchar *plugin);
static void ladspa_config_changed (xmms_object_t *object, xmmsv_t *data, gpointer userdata);
static LADSPA_Descriptor_Function ladspa_get_descriptor_function (const gchar* pluginlib);
static const LADSPA_Descriptor *ladspa_get_descriptor (gchar *pluginlib,
                                                       gchar *pluginname);
static ladspa_plugin_node_t *ladspa_new_node (const gchar *plugin,
                                              gint num_channels,
                                              guint buf_size, gint srate);
static gboolean ladspa_init_node (ladspa_data_t *priv,
                                  ladspa_plugin_node_t *node);
static void ladspa_free_node (ladspa_plugin_node_t *node);

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
	gboolean ret;
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
	if (strlen (plugin) != 0) {
		ret = ladspa_init_plugin (priv, plugin);
	} else {
		ret = -1;
	}
	return ret;
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
		ladspa_free_node (old_node);
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
					= (gfloat) ( ((gint16 *) buf)[j + (i*chans)] / 32768.0);
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
					((gint16 *) buf)[j + (i*chans)] = 32767;
				} else if (out_bufs[j][i] < -1.0) {
					((gint16 *) buf)[j + (i*chans)] = -32768;
				} else {
					((gint16 *) buf)[j + (i*chans)]
						= (gint16) (out_bufs[j][i] * 32768.0);
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
	if (strlen (plugin) != 0) {
		node = ladspa_new_node (plugin, priv->num_channels, priv->buf_size, priv->srate);
		g_mutex_lock (priv->mutex);

		old_node = priv->plugin_list;
		priv->plugin_list = node;
		if (node != NULL) {
			ladspa_init_node (priv, node);
		} else {
			xmms_log_info ("Error loading library!");
		}

		g_mutex_unlock (priv->mutex);
		ladspa_free_node (old_node);
	} else { /* no plugin */
	}
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
	buf_size = XMMS_DEFAULT_BUFFER_SIZE/(xmms_sample_size_get (fmt) * chans);

	if (!priv->enabled || plugin_node == NULL || read <= 0) {
		return read;
	}
	g_mutex_lock (priv->mutex);

	if (buf_size != priv->buf_size || chans != priv->num_channels) {
		xmms_ladspa_reallocate_buffers (priv, buf_size, chans);
	}
	deinterleave (buf, priv->in_bufs, buf_size, chans, fmt);
	last_node = plugin_node;
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
	priv->in_bufs
		= (gfloat **) g_new (gfloat*, num_channels);
	priv->out_bufs
		= (gfloat **) g_new (gfloat*, num_channels);
	for (i = 0; i < num_channels; i++) {
		priv->in_bufs[i]
			= (gfloat *) g_new0 (gfloat, buf_size);
		priv->out_bufs[i]
			= (gfloat *) g_new0 (gfloat, buf_size);
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
		node = ladspa_new_node (str_value, priv->num_channels, priv->buf_size, priv->srate);
		if (node != NULL) {
			g_mutex_lock (priv->mutex);

			old_node = priv->plugin_list;
			priv->plugin_list = node;
			ladspa_init_node (priv, node);

			g_mutex_unlock (priv->mutex);
			ladspa_free_node (old_node);
		} else {
			xmms_log_info ("Error loading library!");
		}
	} else if (g_str_has_prefix (name, "ladspa.control.") ) {
		if (priv->plugin_list) {
			control_index = (gulong) g_ascii_strtoull (name+15, NULL, 10); /* Is there a better way to find this number? */
			if (control_index < priv->plugin_list->num_ctl_in_ports) {
				*(priv->plugin_list->ctl_in_ports[control_index]) = value;
			}
		} else {
			xmms_log_info ("No plugin allocated. Change has no effect");
		}
	}

	/* TODO validate ladspa plugin parameters with:
	   LADSPA_IS_HINT_BOUNDED_BELOW(x)
	   LADSPA_IS_HINT_BOUNDED_ABOVE(x)
	   #define LADSPA_IS_HINT_TOGGLED(x)
	   LADSPA_IS_HINT_INTEGER(x) */
}

static LADSPA_Descriptor_Function
ladspa_get_descriptor_function (const gchar *pluginlib)
{
	void *dl;
	LADSPA_Descriptor_Function desc;
	gboolean exists;
	gchar *libname;
	const char * LADSPA_path;
	gchar ** paths, **p;
	gchar default_path[] = "/usr/lib/ladspa"; /* TODO set different for windows */
	g_assert (pluginlib != NULL);
	if (strlen (pluginlib) == 0) {
		return NULL;
	}
	if (pluginlib[0] == '/') { /* absolute path */
		libname = g_strdup (pluginlib);
	} else { /* relative path */
		LADSPA_path = g_getenv ("LADSPA_PATH");
		if (!LADSPA_path || strlen (LADSPA_path) == 0) {
			paths = g_new (gchar *, 2);
			paths[0] = g_new (gchar, (strlen (default_path) + 1) );
			paths[1] = NULL;
			strncpy (paths[0], default_path, strlen (default_path));
		} else {
			gchar separator[] = ":";
			paths = g_strsplit (LADSPA_path, separator, 16); /* 16 paths should be plenty */
		}
		p = paths;
		exists = FALSE;
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
		return FALSE;
	}
	dl = dlopen (libname, RTLD_LAZY);
	g_free (libname);
	if (dl == NULL) {
		xmms_log_error ("Couldn't open library %s", pluginlib);
		return NULL;
	}
	desc =
		(LADSPA_Descriptor_Function) dlsym (dl, "ladspa_descriptor");
	/* TODO use G_MODULE_SUFFIX to find lib name without extension */
	return desc;
}


static const LADSPA_Descriptor *
ladspa_get_descriptor (gchar *pluginlib, gchar *pluginname)
{
	LADSPA_Descriptor_Function descriptor_function;
	const LADSPA_Descriptor *descriptor;
	gint index, plugin_count;
	descriptor_function = ladspa_get_descriptor_function (pluginlib);
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

static gboolean
ladspa_init_plugin (ladspa_data_t *priv, const gchar *plugin)
{
	ladspa_plugin_node_t *node;

	node = ladspa_new_node (plugin, priv->num_channels,
	                        priv->buf_size, priv->srate);
	if (!node) {
		return FALSE;
	}
	if (!ladspa_init_node (priv, node) ) {
		ladspa_free_node (node);
		return FALSE;
	}

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

static ladspa_plugin_node_t *
ladspa_new_node (const gchar *plugin, gint num_channels, guint buf_size, gint srate)
{
	guint out_count, in_count, ctl_in_port_count, ctl_out_port_count;
	ladspa_plugin_node_t *node;
	const LADSPA_Descriptor *descriptor;
	const LADSPA_PortDescriptor * PortDescriptors;
	const char * const * PortNames;
	gchar *pluginlib, *pluginname;
	gchar **plugin_parts;
	guint i;

	g_assert (strlen (plugin) != 0);
	plugin_parts =  g_strsplit (plugin, ":" , 2);
	pluginlib = plugin_parts[0];
	pluginname = plugin_parts[1];
	if (!pluginname) {
		pluginname = g_new0 (gchar, 1);
	}

	descriptor = ladspa_get_descriptor (pluginlib, pluginname);

	node = (ladspa_plugin_node_t *) g_malloc (sizeof (ladspa_plugin_node_t) );
	node->next = NULL;
	node->plugin = descriptor;

	/* Examine ports */
	PortDescriptors = descriptor->PortDescriptors;
	PortNames = descriptor->PortNames;
	ctl_in_port_count = ctl_out_port_count = out_count = in_count = 0;
	for (i = 0; i < descriptor->PortCount; i++) {
		if (LADSPA_IS_PORT_CONTROL (PortDescriptors[i])) {
			if (LADSPA_IS_PORT_INPUT (PortDescriptors[i])) {
				ctl_in_port_count++;
			} else if (LADSPA_IS_PORT_OUTPUT (PortDescriptors[i])) {
				ctl_out_port_count++;
			}
		} else if (LADSPA_IS_PORT_AUDIO (PortDescriptors[i])) {
			if (LADSPA_IS_PORT_INPUT (PortDescriptors[i])) {
				in_count++;
			} else if (LADSPA_IS_PORT_OUTPUT (PortDescriptors[i])) {
				out_count++;
			}
		}
	}
	/* Instantiate depending on the number of I/O channels */
	node->num_in_channels = in_count;
	node->num_out_channels = out_count;
	if (in_count == out_count && out_count == num_channels) {
		node->instance = (LADSPA_Handle *) g_malloc (sizeof (LADSPA_Handle));
		node->instance[0] = descriptor->instantiate (descriptor, srate);
		node->num_instances = 1;
		node->mode = LADSPA_DIRECT; /* LADSPA_DIRECT */
	} else if (in_count == 1 && out_count == 1) {
		node->instance
			= (LADSPA_Handle *) g_new (LADSPA_Handle, num_channels);
		node->num_instances = num_channels;
		node->mode = LADSPA_MONO; /* LADSPA_MONO */
		out_count = num_channels; /* for creation of right number of temp_bufs */
		for (i = 0; i < num_channels; i++) {
			node->instance[i] = descriptor->instantiate (descriptor, srate);
		}
	} else if (in_count == 1 && out_count == 2 && num_channels == 2) {
		node->instance
			= (LADSPA_Handle *) g_new (LADSPA_Handle, 2);;
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
		node->instance
			= (LADSPA_Handle *) g_new (LADSPA_Handle, n);
		for (i = 0; i < n; i++) {
			node->instance[i] = descriptor->instantiate (descriptor, srate);
		}
	}
	node->num_ctl_in_ports = ctl_in_port_count;
	node->ctl_in_ports
		= (LADSPA_Data **) g_new (LADSPA_Data *, ctl_in_port_count);
	for (i = 0; i < ctl_in_port_count; i++) {
		node->ctl_in_ports[i]
			= (LADSPA_Data *) g_new (LADSPA_Data *, buf_size );
	}
	if (node->mode == LADSPA_BALANCE || node->mode == LADSPA_OTHER) {
		node->temp_bufs
			= (gfloat **) g_new (gfloat *, out_count );
		for (i = 0; i < out_count; i++) {
			node->temp_bufs[i]
				= (gfloat *) g_new (gfloat *, buf_size );
		}
	}
	if (pluginname) {
		g_free (pluginname);
	}
	g_strfreev (plugin_parts);
	return node;
}

static LADSPA_Data
get_ladspa_default_value (const LADSPA_PortRangeHint * PortRangeHints, gint port_num)
{
	LADSPA_Data lower, upper, default_value;
	LADSPA_PortRangeHintDescriptor range_hints;

	lower = PortRangeHints[port_num].LowerBound;
	upper = PortRangeHints[port_num].UpperBound;
	range_hints = PortRangeHints[port_num].HintDescriptor;
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
	}
	return default_value;
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
		xmms_config_property_set_data (property_name, port_name);
	}
	g_free (default_value_str);
	g_free (value_property_name);
	g_free (name_property_name);
}

static gboolean
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

	if (node->num_out_channels == 0) {
		xmms_log_error ("Plugin '%s' has no outputs. Not initialized!", node->plugin->Name);
		return FALSE;
	}
	descriptor = node->plugin;
	PortDescriptors = descriptor->PortDescriptors;
	PortNames = descriptor->PortNames;
	PortRangeHints = descriptor->PortRangeHints;

	clean_unused_control_properties (node->num_ctl_in_ports);
	ctl_in_port_count = 0;
	for (i = 0; i < descriptor->PortCount; i++) {
		if (LADSPA_IS_PORT_CONTROL (PortDescriptors[i])) {
			default_value = get_ladspa_default_value (PortRangeHints, i);
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
	return TRUE;
}

static void
ladspa_free_node (ladspa_plugin_node_t *plugin_node)
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
	plugin_node->ctl_in_ports = NULL;
	g_free (plugin_node);
}
