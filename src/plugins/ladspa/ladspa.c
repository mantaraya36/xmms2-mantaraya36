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

static gboolean xmms_ladspa_plugin_setup (xmms_xform_plugin_t *xform_plugin);
static gboolean xmms_ladspa_init (xmms_xform_t *xform);
static void xmms_ladspa_destroy (xmms_xform_t *xform);
static gint xmms_ladspa_read (xmms_xform_t *xform, xmms_sample_t *buf, gint len,
                          xmms_error_t *error);
static gint64 xmms_ladspa_seek (xmms_xform_t *xform, gint64 offset,
                            xmms_xform_seek_mode_t whence, xmms_error_t *err);

static void ladspa_process (float *buffer);

typedef struct xmms_ladspa_priv_St {
    gboolean enabled;
    LADSPA_Descriptor *plugin;
} xmms_ladspa_data_t;

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
	methods.seek = xmms_ladspa_seek;

	xmms_xform_plugin_methods_set (xform_plugin, &methods);
	/* TODO Set properties */
	/* xmms_xform_plugin_config_property_register (xform_plugin, "bands", "15", */
	/*                                             NULL, NULL); */

	/* TODO Set allowed formats */
	xmms_xform_plugin_indata_add (xform_plugin,
	                              XMMS_STREAM_TYPE_MIMETYPE,
	                              "audio/pcm",
	                              XMMS_STREAM_TYPE_FMT_FORMAT,
	                              XMMS_SAMPLE_FORMAT_S16,
	                              XMMS_STREAM_TYPE_FMT_SAMPLERATE,
	                              48000,
	                              XMMS_STREAM_TYPE_END);

	xmms_xform_plugin_indata_add (xform_plugin,
	                              XMMS_STREAM_TYPE_MIMETYPE,
	                              "audio/pcm",
	                              XMMS_STREAM_TYPE_FMT_FORMAT,
	                              XMMS_SAMPLE_FORMAT_S16,
	                              XMMS_STREAM_TYPE_FMT_SAMPLERATE,
	                              44100,
	                              XMMS_STREAM_TYPE_END);

	xmms_xform_plugin_indata_add (xform_plugin,
	                              XMMS_STREAM_TYPE_MIMETYPE,
	                              "audio/pcm",
	                              XMMS_STREAM_TYPE_FMT_FORMAT,
	                              XMMS_SAMPLE_FORMAT_S16,
	                              XMMS_STREAM_TYPE_FMT_SAMPLERATE,
	                              22050,
	                              XMMS_STREAM_TYPE_END);

	xmms_xform_plugin_indata_add (xform_plugin,
	                              XMMS_STREAM_TYPE_MIMETYPE,
	                              "audio/pcm",
	                              XMMS_STREAM_TYPE_FMT_FORMAT,
	                              XMMS_SAMPLE_FORMAT_S16,
	                              XMMS_STREAM_TYPE_FMT_SAMPLERATE,
	                              11025,
	                              XMMS_STREAM_TYPE_END);

	XMMS_DBG ("LADSPA Host setup success!");
	return TRUE;
}

static gboolean
xmms_ladspa_init (xmms_xform_t *xform)
{
	xmms_ladspa_data_t *priv;
	xmms_config_property_t *config;
	/* gint i, j, srate; */

	g_return_val_if_fail (xform, FALSE);

	priv = g_new0 (xmms_ladspa_data_t, 1);
	g_return_val_if_fail (priv, FALSE);

	xmms_xform_private_data_set (xform, priv);

	/* TODO check config options and apply them */
	/* Set callbacks for options that can be changed during playback
	   otherwise check the value and apply here */
	/* Also make sure all config options are cleaned up in xmms_XX_destroy */
	/* config = xmms_xform_config_lookup (xform, "enabled"); */
	/* g_return_val_if_fail (config, FALSE); */
	/* xmms_config_property_callback_set (config, xmms_ladspa_config_changed, priv); */
	/* priv->enabled = !!xmms_config_property_get_int (config); */

	/* srate = xmms_xform_indata_get_int (xform, XMMS_STREAM_TYPE_FMT_SAMPLERATE); */

	xmms_xform_outdata_type_copy (xform);

	XMMS_DBG ("LADSPA Host init success!");

	return TRUE;
}

static void
xmms_ladspa_destroy (xmms_xform_t *xform)
{
	xmms_config_property_t *config;
	gpointer priv;
	gchar buf[16];
	gint i;

	g_return_if_fail (xform);

	priv = xmms_xform_private_data_get (xform);

	/* Free config options like this */
	/* config = xmms_xform_config_lookup (xform, "enabled"); */
	/* xmms_config_property_callback_remove (config, xmms_ladspa_config_changed, priv); */

	g_free (priv);
}

static gint
xmms_ladspa_read (xmms_xform_t *xform, xmms_sample_t *buf, gint len,
              xmms_error_t *error)
{
	xmms_ladspa_data_t *priv;
	gint read, chan;

	g_return_val_if_fail (xform, -1);

	priv = xmms_xform_private_data_get (xform);
	g_return_val_if_fail (priv, -1);

	read = xmms_xform_read (xform, buf, len, error);
	chan = xmms_xform_indata_get_int (xform, XMMS_STREAM_TYPE_FMT_CHANNELS);
	if (read > 0 && priv->enabled) {
		ladspa_process (read);
	}

	return read;
}

static gint64
xmms_ladspa_seek (xmms_xform_t *xform, gint64 offset, xmms_xform_seek_mode_t whence, xmms_error_t *err)
{
	return xmms_xform_seek (xform, offset, whence, err);
}

static void
ladspa_process (float *buffer)
{
    /* TODO process buffer */
}
