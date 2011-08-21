/*  XMMS2 - X Music Multiplexer System
 *  Copyright (C) 2003-2011 XMMS2 Team
 *
 *  PLUGINS ARE NOT CONSIDERED TO BE DERIVED WORK !!!
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

#include "xcu.h"

#include "xcu.h"

#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>

#include "xmmsc/xmmsc_schema.h"

SETUP (config) {

	return 0;
}

CLEANUP () {
	return 0;
}

CASE (test_big_schema)
{
	xmmsv_t *params;
	xmmsv_t *entry1, *entry2, *entry3;
	xmmsv_t *enum_;
	xmmsv_t *plugins, *pluginlib, *pl_list, *param_dict1, *param_dict2, *param_list;
	xmmsv_t *ladspa, *ladspa_sec, *ctl_dict, *subschema;
	xmmsv_t *value;

	params = xmms_schema_build_dict_entry_types (
	            xmms_schema_build_float ("Gain", "", 1.0),
				NULL);
	entry1 = xmms_schema_build_dict ("amp.so", "", params);

	params = xmms_schema_build_dict_entry_types (
	            xmms_schema_build_float ("Param1", "", 1.0),
	            xmms_schema_build_float_full  (
	                "Param2",
	                "",
	                3.5, 0.0, 5.0, NULL), /* set minimum and maximum values */
	            xmms_schema_build_float_full  (
	                "Param3",
	                "",
	                3.5, 5.0, 10.0, NULL),
	            NULL);
	entry2 = xmms_schema_build_dict ("Plugin2.so", "", params);

	params = xmms_schema_build_dict_entry_types (
	            xmms_schema_build_float ("P1", "", 11.0),
	            xmms_schema_build_float ("P2", "", 12.5),
	            xmms_schema_build_float ("P3", "", 13.5),
	            NULL);

	entry3 = xmms_schema_build_dict ("Plugin3.so", "", params);

	ctl_dict = xmms_schema_build_list ("control", "",
	                                   xmms_schema_build_union (
	                                       "parameters for plugins",
	                                       entry1, entry2, entry3, NULL));

	enum_ = xmmsv_build_list (XMMSV_LIST_ENTRY_STR ("amp.so"),
							  XMMSV_LIST_ENTRY_STR ("Plugin2.so"),
							  XMMSV_LIST_ENTRY_STR ("Plugin3.so"),
							  XMMSV_LIST_END);
	pluginlib = xmms_schema_build_string_all ("title (ignored in list)",
	                                          "description", "",
	                                          enum_);
	plugins = xmms_schema_build_list ("plugin", "", pluginlib);

	ladspa_sec = xmms_schema_build_dict_entry_types (plugins, ctl_dict, NULL);
	ladspa = xmms_schema_build_dict ("ladspa", "LADSPA Plugin host", ladspa_sec);

	/* test setting individual plugins */
	subschema = xmms_schema_get_subschema (ladspa, "ladspa.plugin.0");

	value = xmmsv_new_string ("Plugin2.so");
	CU_ASSERT_TRUE (xmms_schema_validate (subschema, value, NULL));
	xmmsv_unref (value);

	value = xmmsv_new_string ("invalid.so");
	CU_ASSERT_FALSE (xmms_schema_validate (subschema, value, NULL));
	xmmsv_unref (value);

	/* test setting a list of plugins */
	subschema = xmms_schema_get_subschema (ladspa, "ladspa.plugin");
	pl_list = xmmsv_build_list (XMMSV_LIST_ENTRY_STR ("amp.so"),
	                            XMMSV_LIST_ENTRY_STR ("Plugin3.so"),
	                            XMMSV_LIST_END);
	CU_ASSERT_TRUE (xmms_schema_validate (subschema, pl_list, NULL));
	xmmsv_unref (pl_list);

	pl_list = xmmsv_build_list (XMMSV_LIST_ENTRY_STR ("amp.so"),
	                            XMMSV_LIST_ENTRY_STR ("wrong.so"),
	                            XMMSV_LIST_END);
	CU_ASSERT_FALSE (xmms_schema_validate (subschema, pl_list, NULL));
	xmmsv_unref (pl_list);

	pl_list = xmmsv_build_list (XMMSV_LIST_ENTRY_STR ("amp.so"),
	                            XMMSV_LIST_ENTRY_INT (1984),
	                            XMMSV_LIST_END);
	CU_ASSERT_FALSE (xmms_schema_validate (subschema, pl_list, NULL));
	xmmsv_unref (pl_list);

	 /* test setting single parameter values inside union */
	subschema = xmms_schema_get_subschema (ladspa, "ladspa.control.0.Param1");

	value = xmmsv_new_float (1.0);
	CU_ASSERT_TRUE (xmms_schema_validate (subschema, value, NULL));
	xmmsv_unref (value);

	subschema = xmms_schema_get_subschema (ladspa, "ladspa.control.0.Param2");

	value = xmmsv_new_float (1.0);
	CU_ASSERT_TRUE (xmms_schema_validate (subschema, value, NULL));
	xmmsv_unref (value);

	value = xmmsv_new_float (10.0); /* correct type but out of range */
	CU_ASSERT_FALSE (xmms_schema_validate (subschema, value, NULL));
	xmmsv_unref (value);

	value = xmmsv_new_string ("Plugin2.so");
	CU_ASSERT_FALSE (xmms_schema_validate (subschema, value, NULL));
	xmmsv_unref (value);

	/* try seeting various parameters at once inside a union */

	subschema = xmms_schema_get_subschema (ladspa, "ladspa.control.0");
	value = xmmsv_build_dict (XMMSV_DICT_ENTRY_FLOAT ("Param1", 0.5),
	                          XMMSV_DICT_ENTRY_FLOAT ("Param2", 0.5),
	                          XMMSV_DICT_ENTRY_FLOAT ("Param3", 7.0),
	                          XMMSV_DICT_END);
	CU_ASSERT_TRUE (xmms_schema_validate (subschema, value, NULL));
	xmmsv_unref (value);

	subschema = xmms_schema_get_subschema (ladspa, "ladspa.control.0");
	value = xmmsv_build_dict (XMMSV_DICT_ENTRY_FLOAT ("Param1", 0.5),
	                          XMMSV_DICT_ENTRY_FLOAT ("Param2", 6.0), /* out of range */
	                          XMMSV_DICT_ENTRY_FLOAT ("Param3", 7.0),
	                          XMMSV_DICT_END);
	CU_ASSERT_FALSE (xmms_schema_validate (subschema, value, NULL));
	xmmsv_unref (value);

	subschema = xmms_schema_get_subschema (ladspa, "ladspa.control.0");
	value = xmmsv_build_dict (XMMSV_DICT_ENTRY_FLOAT ("Param1", 0.5),
	                          XMMSV_DICT_ENTRY_INT ("Param2", 6), /* wrong type */
	                          XMMSV_DICT_ENTRY_FLOAT ("Param3", 7.0),
	                          XMMSV_DICT_END);
	CU_ASSERT_FALSE (xmms_schema_validate (subschema, value, NULL));
	xmmsv_unref (value);

	subschema = xmms_schema_get_subschema (ladspa, "ladspa.control.0");
	value = xmmsv_build_dict (XMMSV_DICT_ENTRY_FLOAT ("Param1", 0.5),
	                          XMMSV_DICT_ENTRY_INT ("WrongParam2", 6), /* wrong name */
	                          XMMSV_DICT_ENTRY_FLOAT ("Param3", 7.0),
	                          XMMSV_DICT_END);
	CU_ASSERT_FALSE (xmms_schema_validate (subschema, value, NULL));
	xmmsv_unref (value);


	/* try setting the whole thing */

	pl_list = xmmsv_build_list (XMMSV_LIST_ENTRY_STR ("amp.so"),
	                            XMMSV_LIST_ENTRY_STR ("Plugin2.so"),
	                            XMMSV_LIST_END);
	param_dict1 = xmmsv_build_dict (XMMSV_DICT_ENTRY_FLOAT ("Gain", 0.5),
	                                XMMSV_DICT_END);
	param_dict2 = xmmsv_build_dict (XMMSV_DICT_ENTRY_FLOAT ("Param1", 0.5),
	                                XMMSV_DICT_ENTRY_FLOAT ("Param2", 0.5),
	                                XMMSV_DICT_ENTRY_FLOAT ("Param3", 7.0),
	                                XMMSV_DICT_END);
	param_list = xmmsv_build_list (XMMSV_LIST_ENTRY (param_dict1),
	                               XMMSV_LIST_ENTRY (param_dict2),
	                               XMMSV_LIST_END);
	value = xmmsv_build_dict (XMMSV_DICT_ENTRY ("plugin", pl_list),
	                          XMMSV_DICT_ENTRY ("control", param_list),
	                          XMMSV_DICT_END);

	CU_ASSERT_TRUE (xmms_schema_validate (ladspa, value, NULL));

	xmmsv_unref (value);
	xmmsv_unref (ladspa);
}

CASE (test_union)
{
	xmmsv_t *params1, *params2, *entries;
	xmmsv_t *schema, *subschema, *value;

	entries = xmms_schema_build_dict_entry_types (
	            xmms_schema_build_float ("Param1", "", 1.0),
	            xmms_schema_build_float ("Param2", "", 1.0),
				xmms_schema_build_float ("Param3", "", 1.0),
	            NULL);
	params1 = xmms_schema_build_dict ("first set", "",
	                                 entries);

	entries = xmms_schema_build_dict_entry_types (
	            xmms_schema_build_float ("P1", "", 1.0),
	            xmms_schema_build_float ("P2", "", 1.0),
				xmms_schema_build_float ("P3", "", 1.0),
	            NULL);
	params2 = xmms_schema_build_dict ("second set", "",
	                                 entries);


	schema = xmms_schema_build_union ("parameters for plugins",
	                                  params1, params2, NULL);

	value = xmmsv_build_dict (XMMSV_DICT_ENTRY_FLOAT ("Param1", 1.0),
	                          XMMSV_DICT_ENTRY_FLOAT ("Param2", 1.0),
	                          XMMSV_DICT_ENTRY_FLOAT ("Param3", 1.0),
	                          XMMSV_DICT_END);
	CU_ASSERT_TRUE (xmms_schema_validate (schema, value, NULL));
	xmmsv_unref (value);

	value = xmmsv_build_dict (XMMSV_DICT_ENTRY_FLOAT ("Param3", 1.0),
	                          XMMSV_DICT_END);
	CU_ASSERT_TRUE (xmms_schema_validate (schema, value, NULL));
	xmmsv_unref (value);

	value = xmmsv_build_dict (XMMSV_DICT_ENTRY_FLOAT ("P1", 1.0),
	                          XMMSV_DICT_ENTRY_FLOAT ("Param3", 1.0),
	                          XMMSV_DICT_END);
	CU_ASSERT_FALSE (xmms_schema_validate (schema, value, NULL));
	xmmsv_unref (value);

	value = xmmsv_build_dict (XMMSV_DICT_ENTRY_FLOAT ("P1", 1.0),
	                          XMMSV_DICT_ENTRY_FLOAT ("nothing3", 1.0),
	                          XMMSV_DICT_END);
	CU_ASSERT_FALSE (xmms_schema_validate (schema, value, NULL));
	xmmsv_unref (value);

	value = xmmsv_build_dict (XMMSV_DICT_ENTRY_INT ("P1", 1),
	                          XMMSV_DICT_ENTRY_FLOAT ("P2", 1.0),
	                          XMMSV_DICT_END);
	CU_ASSERT_FALSE (xmms_schema_validate (schema, value, NULL));
	xmmsv_unref (value);

	value = xmmsv_new_float (1.0);
	CU_ASSERT_FALSE (xmms_schema_validate (schema, value, NULL));
	xmmsv_unref (value);

	xmmsv_unref (schema);
}

CASE (test_any)
{
	xmmsv_t *enum_, *pluginlib, *plugins, *ctl_dict, *ladspa_sec, *ladspa;
	xmmsv_t *subschema, *value;

	enum_ = xmmsv_build_list (XMMSV_LIST_ENTRY_STR ("amp.so"),
							  XMMSV_LIST_ENTRY_STR ("other.so"),
							  XMMSV_LIST_ENTRY_STR ("onemore.so"),
							  XMMSV_LIST_END);
	pluginlib = xmms_schema_build_string_all ("title (ignored in list)", "description", "",
	                                          enum_);
	plugins = xmms_schema_build_list ("plugin", "", pluginlib);

	ctl_dict = xmms_schema_build_list ("control", "",
	                                   xmms_schema_build_any ("", ""));

	ladspa_sec = xmms_schema_build_dict_entry_types (plugins, ctl_dict, NULL);
	ladspa = xmms_schema_build_dict ("ladspa", "LADSPA Plugin host", ladspa_sec);

	subschema = xmms_schema_get_subschema (ladspa, "ladspa.plugin.0");

	value = xmmsv_new_string ("notvalid.so");
	CU_ASSERT_FALSE (xmms_schema_validate (subschema, value, NULL));
	xmmsv_unref (value);
	value = xmmsv_new_string ("amp.so");
	CU_ASSERT_TRUE (xmms_schema_validate (subschema, value, NULL));
	xmmsv_unref (value);

	subschema = xmms_schema_get_subschema (ladspa, "ladspa.control");

	value = xmmsv_new_string ("valid.so");
	CU_ASSERT_FALSE (xmms_schema_validate (subschema, value, NULL));
	xmmsv_unref (value);
	value = xmmsv_build_list (XMMSV_LIST_ENTRY_INT (6),
	                          XMMSV_LIST_ENTRY_STR ("hello"),
	                          XMMSV_LIST_END);
	CU_ASSERT_TRUE (xmms_schema_validate (subschema, value, NULL));
	xmmsv_unref (value);

	subschema = xmms_schema_get_subschema (ladspa, "ladspa.control.0");

	value = xmmsv_new_string ("valid.so");
	CU_ASSERT_TRUE (xmms_schema_validate (subschema, value, NULL));
	xmmsv_unref (value);
	value = xmmsv_new_int (4);
	CU_ASSERT_TRUE (xmms_schema_validate (subschema, value, NULL));
	xmmsv_unref (value);

	xmmsv_unref (ladspa);
}

CASE (test_dicts_in_lists)
{

	xmmsv_t *params1, *params2, *entries;
	xmmsv_t *schema, *subschema, *value;

	entries = xmms_schema_build_dict_entry_types (
	            xmms_schema_build_float ("Param1", "", 1.0),
	            xmms_schema_build_float ("Param2", "", 1.0),
				xmms_schema_build_float ("Param3", "", 1.0),
	            NULL);
	params1 = xmms_schema_build_dict ("first set", "",
	                                 entries);

	schema = xmms_schema_build_list ("control", "",
	                                 params1);

	subschema = xmms_schema_get_subschema (schema, "control.0.Param1");

	value = xmmsv_new_int (9);
	CU_ASSERT_FALSE (xmms_schema_validate (subschema, value, NULL));
	xmmsv_unref (value);
	value = xmmsv_new_float (1.0);
	CU_ASSERT_TRUE (xmms_schema_validate (subschema, value, NULL));
	xmmsv_unref (value);

	subschema = xmms_schema_get_subschema (schema, "control.0");
	value = xmmsv_build_dict (XMMSV_DICT_ENTRY_FLOAT ("Param1", 1.0),
	                          XMMSV_DICT_ENTRY_FLOAT ("Param2", 1.0),
	                          XMMSV_DICT_ENTRY_FLOAT ("Param3", 1.0),
	                          XMMSV_DICT_END);
	CU_ASSERT_TRUE (xmms_schema_validate (subschema, value, NULL));
	xmmsv_unref (value);
	value = xmmsv_build_dict (XMMSV_DICT_ENTRY_FLOAT ("Param1", 1.0),
	                          XMMSV_DICT_ENTRY_FLOAT ("Param2", 1.0),
	                          XMMSV_DICT_ENTRY_INT ("Param3", 1.0),
	                          XMMSV_DICT_END);
	CU_ASSERT_FALSE (xmms_schema_validate (subschema, value, NULL));
	xmmsv_unref (value);
	value = xmmsv_build_dict (XMMSV_DICT_ENTRY_FLOAT ("Param1", 1.0),
	                          XMMSV_DICT_ENTRY_FLOAT ("WrongParam2", 1.0),
	                          XMMSV_DICT_END);
	CU_ASSERT_FALSE (xmms_schema_validate (subschema, value, NULL));
	xmmsv_unref (value);

	xmmsv_unref (schema);
}

CASE (test_enum)
{
	xmmsv_t *pluginlib;
	xmmsv_t *enum_;
	xmmsv_t *plugins;
	xmmsv_t *value;
	xmmsv_t *subschema;

	enum_ = xmmsv_build_list (XMMSV_LIST_ENTRY_STR ("amp.so"),
							  XMMSV_LIST_ENTRY_STR ("other.so"),
							  XMMSV_LIST_ENTRY_STR ("onemore.so"),
							  XMMSV_LIST_END);
	pluginlib = xmms_schema_build_string_all ("title (ignored in list)", "description", "",
	                                          enum_);
	plugins = xmms_schema_build_list ("plugin", "", pluginlib);

	value = xmmsv_build_list (XMMSV_LIST_ENTRY_STR ("amp.so"),
	                          XMMSV_LIST_END);
	CU_ASSERT_TRUE (xmms_schema_validate (plugins, value, NULL));
	xmmsv_unref (value);

	value = xmmsv_build_list (XMMSV_LIST_ENTRY_STR ("other.so"),
	                          XMMSV_LIST_ENTRY_STR ("amp.so"),
	                          XMMSV_LIST_END);
	CU_ASSERT_TRUE (xmms_schema_validate (plugins, value, NULL));
	xmmsv_unref (value);

	value = xmmsv_build_list (XMMSV_LIST_ENTRY_STR ("other.so"),
	                          XMMSV_LIST_ENTRY_STR ("not valid"),
	                          XMMSV_LIST_END);
	CU_ASSERT_FALSE (xmms_schema_validate (plugins, value, NULL));
	xmmsv_unref (value);

	subschema = xmms_schema_get_subschema (plugins, "plugin.0");

	value = xmmsv_new_string ("amp.so");
	CU_ASSERT_TRUE (xmms_schema_validate (subschema, value, NULL));
	xmmsv_unref (value);

	xmmsv_unref (plugins);
}

CASE (test_simple_validate)
{
	xmmsv_t *lr, *volume, *jack, *schema;
	xmmsv_t *subschema, *value;

	/* "jack.clientname XMMS2" "jack.volume.left 100" "jack.volume.right 100" */

	lr = xmms_schema_build_dict_entry_types (
	            xmms_schema_build_int32 ("left", "",
	                                     100),
	            xmms_schema_build_int32 ("right", "",
	                                     100),
	            NULL);
	volume = xmms_schema_build_dict ("volume", "Level of each channel",
	                                 lr);

	jack = xmms_schema_build_dict_entry_types (xmms_schema_build_string ("clientname", "",
	                                                                     "XMMS"),
	                                           volume,
	                                           NULL);
	schema = xmms_schema_build_dict ("jack", "Jack IO parameters", jack);

	subschema = xmms_schema_get_subschema (schema, "jack.clientname");

	value = xmmsv_new_int (9);
	CU_ASSERT_FALSE (xmms_schema_validate (subschema, value, NULL));
	xmmsv_unref (value);
	value = xmmsv_new_string ("XMMS-new");
	CU_ASSERT_TRUE (xmms_schema_validate (subschema, value, NULL));
	xmmsv_unref (value);

	subschema = xmms_schema_get_subschema (schema, "jack.volume");

	value = xmmsv_new_string ("XMMS-new");
	CU_ASSERT_FALSE (xmms_schema_validate (subschema, value, NULL));
	xmmsv_unref (value);

	value = xmmsv_build_dict (XMMSV_DICT_ENTRY_INT ("left", 80),
	                          XMMSV_DICT_ENTRY_INT ("right", 95),
	                          XMMSV_DICT_END);
	CU_ASSERT_TRUE (xmms_schema_validate (subschema, value, NULL));
	xmmsv_unref (value);

	value = xmmsv_build_dict (XMMSV_DICT_ENTRY_INT ("left", 80),
	                          XMMSV_DICT_ENTRY_FLOAT ("right", 90.5),
	                          XMMSV_DICT_END);
	CU_ASSERT_FALSE (xmms_schema_validate (subschema, value, NULL));
	xmmsv_unref (value);
	xmmsv_unref (schema);
}

CASE (test_simple_schemas)
{
	xmmsv_t *lr, *volume, *jack, *schema;
	xmmsv_t *test;
	const char *name;

	/* "jack.clientname XMMS2" "jack.volume.left 100" "jack.volume.right 100" */

	lr = xmms_schema_build_dict_entry_types (
	            xmms_schema_build_int32 ("left", "",
	                                     100),
	            xmms_schema_build_int32 ("right", "",
	                                     100),
	            NULL);
	volume = xmms_schema_build_dict ("volume", "Level of each channel",
	                                 lr);

	jack = xmms_schema_build_dict_entry_types (
	            xmms_schema_build_string ("clientname", "",
	                                      "XMMS"),
	            volume,
	            NULL);
	schema = xmms_schema_build_dict ("jack", "Jack IO parameters", jack);


	test = xmms_schema_get_subschema (schema, "jack");
	CU_ASSERT_TRUE (xmmsv_dict_entry_get_string (test, "title", &name));
	CU_ASSERT_EQUAL (strcmp (name, "jack"), 0);
	CU_ASSERT_PTR_NOT_NULL (test);
	test = xmms_schema_get_subschema (schema, "jack.volume");
	CU_ASSERT_TRUE (xmmsv_dict_entry_get_string (test, "title", &name));
	CU_ASSERT_EQUAL (strcmp (name, "volume"), 0);
	CU_ASSERT_PTR_NOT_NULL (test);
	test = xmms_schema_get_subschema (schema, "jack.volume.left");
	CU_ASSERT_TRUE (xmmsv_dict_entry_get_string (test, "title", &name));
	CU_ASSERT_EQUAL (strcmp (name, "left"), 0);
	CU_ASSERT_PTR_NOT_NULL (test);

	test = xmms_schema_get_subschema (schema, "badroot");
	CU_ASSERT_PTR_NULL (test);

	test = xmms_schema_get_subschema (schema, "jack.badentry");
	CU_ASSERT_PTR_NULL (test);

	xmmsv_unref (schema);
}

