/** @file vocoder.c
 *  Vocoder effect plugin
 *
 *  Copyright (C) 2006 XMMS2 Team
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
 */

#include "xmms/xmms_defs.h"
#include "xmms/xmms_xformplugin.h"
#include "xmms/xmms_sample.h"
#include "xmms/xmms_log.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <glib.h>
#include <fftw3.h>

#include "pvocoder.h"

typedef struct {
	pvocoder_t *pvoc;
	
	gint winsize;
	gint channels;
	gint bufsize;

	xmms_sample_t *iobuf;
	pvocoder_sample_t *procbuf;
	GString *outbuf;

	gint speed;
	gboolean enabled;
} xmms_vocoder_data_t;

static gboolean xmms_vocoder_plugin_setup (xmms_xform_plugin_t *xform_plugin);
static gboolean xmms_vocoder_init (xmms_xform_t *xform);
static void xmms_vocoder_destroy (xmms_xform_t *xform);
static void xmms_vocoder_config_changed (xmms_object_t *object, gconstpointer data,
                                         gpointer userdata);
static gint xmms_vocoder_read (xmms_xform_t *xform, xmms_sample_t *buf, gint len,
                               xmms_error_t *err);
static gint64 xmms_vocoder_seek (xmms_xform_t *xform, gint64 offset,
                                 xmms_xform_seek_mode_t whence,
                                 xmms_error_t *err);

/*
 * Plugin header
 */

XMMS_XFORM_PLUGIN ("vocoder",
                   "Vocoder effect", XMMS_VERSION,
                   "Phase vocoder effect plugin",
                   xmms_vocoder_plugin_setup);

static gboolean
xmms_vocoder_plugin_setup (xmms_xform_plugin_t *xform_plugin)
{
	xmms_xform_methods_t methods;

	XMMS_XFORM_METHODS_INIT (methods);
	methods.init = xmms_vocoder_init;
	methods.destroy = xmms_vocoder_destroy;
	methods.read = xmms_vocoder_read;
	methods.seek = xmms_vocoder_seek;

	xmms_xform_plugin_methods_set (xform_plugin, &methods);

	xmms_xform_plugin_config_property_register (xform_plugin, "speed", "100",
	                                            NULL, NULL);

	xmms_xform_plugin_indata_add (xform_plugin,
	                              XMMS_STREAM_TYPE_MIMETYPE,
	                              "audio/pcm",
	                              XMMS_STREAM_TYPE_FMT_FORMAT,
	                              XMMS_SAMPLE_FORMAT_S16,
	                              XMMS_STREAM_TYPE_END);

	return TRUE;
}

static gboolean
xmms_vocoder_init (xmms_xform_t *xform)
{
	xmms_vocoder_data_t *priv;
	xmms_config_property_t *config;

	g_return_val_if_fail (xform, FALSE);

	priv = g_new0 (xmms_vocoder_data_t, 1);
	priv->winsize = 2048;
	priv->channels = xmms_xform_indata_get_int (xform, XMMS_STREAM_TYPE_FMT_CHANNELS);
	priv->bufsize = priv->winsize * priv->channels;

	priv->iobuf = g_malloc (priv->bufsize * sizeof(gint16));
	priv->procbuf = g_malloc (priv->bufsize * sizeof(pvocoder_sample_t));
	priv->outbuf = g_string_new (NULL);

	priv->pvoc = pvocoder_init (priv->winsize, priv->channels);
	g_return_val_if_fail (priv->pvoc, FALSE);

	xmms_xform_private_data_set (xform, priv);
	
	config = xmms_xform_config_lookup (xform, "enabled");
	g_return_val_if_fail (config, FALSE);
	xmms_config_property_callback_set (config, xmms_vocoder_config_changed, priv);
	priv->enabled = !!xmms_config_property_get_int (config);

	config = xmms_xform_config_lookup (xform, "speed");
	g_return_val_if_fail (config, FALSE);
	xmms_config_property_callback_set (config, xmms_vocoder_config_changed, priv);
	priv->speed = xmms_config_property_get_int (config);
	pvocoder_set_scale (priv->pvoc, (gfloat) priv->speed / 100.0);

	xmms_xform_outdata_type_copy (xform);

	return TRUE;
}

static void
xmms_vocoder_destroy (xmms_xform_t *xform)
{
	xmms_vocoder_data_t *data;

	g_return_if_fail (xform);

	data = xmms_xform_private_data_get (xform);
	g_return_if_fail (data);

	if (data->pvoc) {
		pvocoder_close (data->pvoc);
	}

	g_string_free (data->outbuf, TRUE);
	g_free (data->procbuf);
	g_free (data->iobuf);
	g_free (data);
}

static void
xmms_vocoder_config_changed (xmms_object_t *object, gconstpointer data,
                             gpointer userdata)
{
	xmms_config_property_t *val;
	xmms_vocoder_data_t *priv;
	const gchar *name;
	gint value;

	g_return_if_fail (object);
	g_return_if_fail (userdata);

	val = (xmms_config_property_t *) object;
	priv = (xmms_vocoder_data_t *) userdata;

	name = xmms_config_property_get_name (val);
	value = xmms_config_property_get_int (val);

	XMMS_DBG ("config value changed! %s => %d", name, value);
	/* we are passed the full config key, not just the last token,
	 * which makes this code kinda ugly.
	 * fix when bug 97 has been resolved
	 */
	name = strrchr (name, '.') + 1;

	if (!strcmp (name, "enabled")) {
		priv->enabled = !!value;
	} else if (!strcmp (name, "speed")) {
		priv->speed = value;
		pvocoder_set_scale (priv->pvoc, (gfloat) priv->speed / 100.0);
	}
}

static gint
xmms_vocoder_read (xmms_xform_t *xform, xmms_sample_t *buffer, gint len,
                   xmms_error_t *error)
{
	xmms_vocoder_data_t *data;
	guint size;

	g_return_val_if_fail (xform, -1);

	data = xmms_xform_private_data_get (xform);
	g_return_val_if_fail (data, -1);

	size = MIN (data->outbuf->len, len);
	while (size == 0) {
		int i, dpos;
		gint16 *samples = (gint16 *) data->iobuf;

		if (!data->enabled) {
			return xmms_xform_read (xform, buffer, len, error);
		}

		dpos = pvocoder_get_chunk (data->pvoc, data->procbuf);
		while (dpos != 0) {
			int ret, read = 0;

			memset (data->procbuf, 0, data->bufsize *
			                          sizeof (pvocoder_sample_t));
			while (read < data->bufsize * sizeof(gint16)) {
				ret = xmms_xform_read (xform, data->iobuf+read,
				                       data->bufsize*sizeof (gint16)-read, error);
				if (ret <= 0) {
					if (!ret && !read) {
						/* end of file */
						return 0;
					} else if (ret < 0) {
						return ret;
					}
					break;
				}
				read += ret;
			}

			for (i=0; i<data->bufsize; i++) {
				data->procbuf[i] = (pvocoder_sample_t) samples[i] / 33000;
			}
			pvocoder_add_chunk (data->pvoc, data->procbuf);
			dpos = pvocoder_get_chunk (data->pvoc, data->procbuf);
		}

		for (i=0; i<data->bufsize; i++) {
			samples[i] = data->procbuf[i] * 32768;
		}
		g_string_append_len (data->outbuf, data->iobuf, data->bufsize * sizeof(gint16));
		size = MIN (data->outbuf->len, len);
	}

	memcpy (buffer, data->outbuf->str, size);
	g_string_erase (data->outbuf, 0, size);

	return size;
}

static gint64
xmms_vocoder_seek (xmms_xform_t *xform, gint64 offset,
                   xmms_xform_seek_mode_t whence, xmms_error_t *err)
{                 
	return xmms_xform_seek (xform, offset, whence, err);
}       

