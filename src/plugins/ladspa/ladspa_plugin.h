/** @file ladspa_plugin.h
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

#include <glib.h>

#include "ladspa.h"

typedef enum {
	LADSPA_DIRECT, /* Plugin inputs and outputs match */
	LADSPA_MONO, /* Plugin is mono and has been instantiated for each input channel */
	LADSPA_BALANCE, /* Plugin input is mono, output stereo and chain is stereo */
	LADSPA_OTHER, /* any other configuration, plugin is instantiated to match inputs and
	                 the outputs are put one after the other discarding the last ones if
	                 needed */
	LADSPA_NONE /* don't run the plugin (plugin invalid/non existent) */
} LADSPA_Mode;

struct ladspa_plugin_node_St {
    const LADSPA_Descriptor *plugin;
	gchar *pluginstring;
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

ladspa_plugin_node_t *ladspa_plugin_new_node (const gchar *plugin,
                                              gint num_channels,
                                              guint buf_size, gint srate);
void ladspa_plugin_free_node (ladspa_plugin_node_t *node);

LADSPA_Data ladspa_plugin_get_default_value (const LADSPA_PortRangeHint * PortRangeHints,
                                             gint port_num);


gint ladspa_plugin_get_index_for_parameter (ladspa_plugin_node_t *plugin, const gchar *param_name);
