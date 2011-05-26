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

static gboolean xmms_ladspa_plugin_setup (xmms_xform_plugin_t *xform_plugin);
static gboolean xmms_ladspa_init (xmms_xform_t *xform);
static void xmms_ladspa_destroy (xmms_xform_t *xform);
static gint xmms_ladspa_read (xmms_xform_t *xform, xmms_sample_t *buf, gint len,
                          xmms_error_t *error);
static gint64 xmms_ladspa_seek (xmms_xform_t *xform, gint64 offset,
                            xmms_xform_seek_mode_t whence, xmms_error_t *err);

struct ladspa_plugin_node_St {
    const LADSPA_Descriptor *plugin;
	LADSPA_Handle instance;
	struct ladspa_plugin_node_St *next;
};

typedef struct ladspa_plugin_node_St ladspa_plugin_node_t;

#define XMMS_DEFAULT_BUFFER_SIZE 4096

typedef struct ladspa_data_St {
    gboolean enabled;
    ladspa_plugin_node_t *plugin_list;
	guint num_channels;
	guint buf_size; /* size of de-interleaved buffers */
	gfloat **in_bufs; /* de-interleaved buffers */
	gfloat **out_bufs;
} ladspa_data_t;

static void ladspa_process (ladspa_plugin_node_t *plugin_node, guint nsamps);
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
	                                            NULL, NULL);  /* TODO set callback */
	xmms_xform_plugin_config_property_register (xform_plugin, "pluginname", "TAP TubeWarmth",
	                                            NULL, NULL);  /* TODO set callback */

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

	xmms_log_info("LADSPA Host setup OK.");
	XMMS_DBG ("LADSPA Host setup success!");
	return TRUE;
}

static gboolean
xmms_ladspa_init (xmms_xform_t *xform)
{
	ladspa_data_t *priv;
	xmms_config_property_t *config;
	const LADSPA_Descriptor *descriptor;
	LADSPA_Descriptor_Function descriptor_function;
	void *dl;
	const gchar *pluginlib;
	const gchar *pluginname;
	gint srate;
	gint num_channels;
	gint fmt;
	gulong index;
	guint i;
	guint buf_size;

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

	/* TODO this could be allocated in setup, to avoid allocation here */
	buf_size = priv->buf_size;
	priv->in_bufs = (gfloat **) g_malloc (num_channels * sizeof (gfloat*) );
	priv->out_bufs = (gfloat **) g_malloc (num_channels * sizeof (gfloat*) );
	for (i = 0; i < num_channels; i++) {
		priv->in_bufs[i] = (gfloat *) g_malloc (buf_size * sizeof (float) );
		priv->out_bufs[i] = (gfloat *) g_malloc (buf_size* sizeof (float) );
	}

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

	priv->plugin_list = (ladspa_plugin_node_t *)
		g_malloc (sizeof (ladspa_plugin_node_t) );

	/* TODO load from config */
	/* dl = dlopen (pluginlib, RTLD_LAZY); */
	dl = dlopen ("/usr/lib/ladspa/tap_tubewarmth.so", RTLD_LAZY);
	if (dl == NULL) {
		xmms_log_error("Couldn't open library %s", pluginlib);
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
		/* TODO un hard-wire this */
		/* if (strcmp(descriptor->Label, pluginname)) { */
		if (strcmp (descriptor->Label, "tap_tubewarmth") == 0) {
			break;
		}
	}

	priv->plugin_list->plugin = descriptor;
	priv->plugin_list->instance =
		descriptor->instantiate (descriptor, srate);

	xmms_log_info ("Plugin name %s. Init OK.", descriptor->Label);

	return TRUE;
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
		plugin_node->plugin->deactivate (plugin_node->instance);
		plugin_node->plugin->cleanup (plugin_node->instance);
		/* TODO is there a need to free something from ladspa? */
		old_node = plugin_node;
		plugin_node = plugin_node->next;
		g_free (old_node);
	}
	for(i = 0; i < priv->num_channels; i++) {
		g_free (priv->in_bufs[i]);
		g_free (priv->out_bufs[i]);
	}
	g_free (priv->out_bufs);
	g_free (priv->in_bufs);

	/* Free config options like this */
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
	g_return_val_if_fail (xform, -1);

	priv = xmms_xform_private_data_get (xform);
	g_return_val_if_fail (priv, -1);

	plugin_node = priv->plugin_list;

	read = xmms_xform_read (xform, buf, len, error);
	chans = xmms_xform_indata_get_int (xform, XMMS_STREAM_TYPE_FMT_CHANNELS);
	fmt = xmms_xform_indata_get_int (xform, XMMS_STREAM_TYPE_FMT_FORMAT);

	if (!priv->enabled) {
		return read;
	}

	buf_size = XMMS_DEFAULT_BUFFER_SIZE/(xmms_sample_size_get (fmt) * chans);

	/* if buffer size or number of channels has changed, must reallocate */
	if (buf_size != priv->buf_size) {
		xmms_log_info("Reallocating ladspa buffers to size %i", buf_size);
		priv->buf_size = buf_size;
		priv->in_bufs
			= (gfloat **) g_realloc (priv->in_bufs,
									 chans * sizeof (gfloat*) );
		priv->out_bufs
			= (gfloat **) g_realloc (priv->out_bufs,
									 chans * sizeof (gfloat*) );
		for (i = 0; i < chans; i++) {
			priv->in_bufs[i]
				= (gfloat *) g_realloc (priv->in_bufs[i],
										buf_size * sizeof (float) );
			priv->out_bufs[i]
				= (gfloat *) g_realloc (priv->out_bufs[i],
										buf_size* sizeof (float) );
		}
		/* TODO need to reconnect ladspa ports when reallocating */
	}
	/* de-interleave input */
	for (i = 0; i < buf_size; i++) {
		for (j = 0; j < chans; j++) {
			if (fmt == XMMS_SAMPLE_FORMAT_S16) {
				priv->in_bufs[j][i]
					= (gfloat) ( ((gint16 *) buf)[j + (i*chans)] / 32768.0);
			}
			else if (fmt == XMMS_SAMPLE_FORMAT_FLOAT) {
				priv->in_bufs[j][i] = ( (gfloat *) buf)[j + (i*chans)];
			}
		}
	}
	while (plugin_node == NULL || plugin_node->plugin == NULL) {
		if (read > 0) {
			ladspa_process (plugin_node, len);
		}
		plugin_node = plugin_node->next;
		if (plugin_node != NULL) { 	/* Copy output to input for next plugin */
			for (i = 0; i < buf_size; i++) {
				for (j = 0; j < chans; j++) {
					priv->in_bufs[j][i] = priv->out_bufs[j][i];
				}
			}
		}
	}
	/* interleave output */
	for (i = 0; i < buf_size; i++) {
		for (j = 0; j < chans; j++) {
			if (fmt == XMMS_SAMPLE_FORMAT_S16) {
				/* clip output */
				if (priv->out_bufs[j][i] > 1.0) {
					((gint16 *) buf)[j + (i*chans)] = 32767;
				}
				else if (priv->out_bufs[j][i] < -1.0) {
					((gint16 *) buf)[j + (i*chans)] = -32768;
				}
				else {
					((gint16 *) buf)[j + (i*chans)]
						= (gint16) (priv->in_bufs[j][i] * 32768.0);
				}
			}
			else if (fmt == XMMS_SAMPLE_FORMAT_FLOAT) {
				( (gfloat *) buf)[j + (i*chans)] = priv->out_bufs[j][i];
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

}

static void
ladspa_process (ladspa_plugin_node_t *plugin_node, guint nsamps)
{
	while (plugin_node != NULL) {
		plugin_node->plugin->run(plugin_node->instance, nsamps);
		plugin_node = plugin_node->next;
	}
}
