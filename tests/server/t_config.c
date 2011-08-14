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

#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <xmmspriv/xmms_config.h>
#include <xmmspriv/xmms_ipc.h>

static xmms_config_t *config;

SETUP (config) {
	xmmsv_t *value;
	g_thread_init (NULL);
	xmms_ipc_init ();

	/* TODO don't write to disk */
	config = xmms_config_new ("test.conf");
	value = xmmsv_new_dict ();
	/* clear previously stored conf */
	xmms_config_set (config, "", value);
	xmmsv_unref (value);

	return 0;
}

CLEANUP () {
	xmms_object_unref (config);
	return 0;
}

CASE (test_register)
{
	xmmsv_t *value, *value2;
	gfloat f;
	gint32 i;
	const gchar *s;

	value = xmmsv_build_list (XMMSV_LIST_ENTRY_FLOAT (0.5),
	                          XMMSV_LIST_ENTRY_STR ("Yes sir."),
                              XMMSV_LIST_ENTRY_INT (6),
                              XMMSV_LIST_END);
	value2 = xmms_config_schema_register (NULL,  "schema_test", value, NULL, NULL);
	CU_ASSERT_PTR_NOT_NULL (value2);
	xmmsv_unref (value);
	xmmsv_unref (value2);

	value = xmms_config_get (config, "schema_test");
	CU_ASSERT_PTR_NOT_NULL (value);
	CU_ASSERT_TRUE (xmmsv_is_type (value, XMMSV_TYPE_LIST));
	CU_ASSERT_TRUE (xmmsv_list_get (value, 0, &value2));
	CU_ASSERT_TRUE (xmmsv_get_float (value2, &f));
	CU_ASSERT_EQUAL (f, 0.5f);

	CU_ASSERT_TRUE (xmmsv_list_get (value, 1, &value2));
	CU_ASSERT_TRUE (xmmsv_get_string (value2, &s));
	CU_ASSERT_EQUAL (strcmp (s, "Yes sir."), 0);

	CU_ASSERT_TRUE (xmmsv_list_get (value, 2, &value2));
	CU_ASSERT_TRUE (xmmsv_get_int (value2, &i));
	CU_ASSERT_EQUAL (i, 6);
	xmmsv_unref (value);

	/* the values here should not be applied as the properties already exist */
	value = xmmsv_build_list (XMMSV_LIST_ENTRY_FLOAT (2.5),
	                          XMMSV_LIST_ENTRY_STR ("No sir."),
                              XMMSV_LIST_ENTRY_INT (66),
                              XMMSV_LIST_END);
	value2 = xmms_config_schema_register (NULL,  "schema_test", value, NULL, NULL);
	CU_ASSERT_PTR_NOT_NULL (value2);
	xmmsv_unref (value);
	xmmsv_unref (value2);

	value = xmms_config_get (config, "schema_test");
	CU_ASSERT_PTR_NOT_NULL (value);
	CU_ASSERT_TRUE (xmmsv_is_type (value, XMMSV_TYPE_LIST));
	CU_ASSERT_TRUE (xmmsv_list_get (value, 0, &value2));
	CU_ASSERT_TRUE (xmmsv_get_float (value2, &f));
	CU_ASSERT_EQUAL (f, 0.5f);

	CU_ASSERT_TRUE (xmmsv_list_get (value, 1, &value2));
	CU_ASSERT_TRUE (xmmsv_get_string (value2, &s));
	CU_ASSERT_EQUAL (strcmp (s, "Yes sir."), 0);

	CU_ASSERT_TRUE (xmmsv_list_get (value, 2, &value2));
	CU_ASSERT_TRUE (xmmsv_get_int (value2, &i));
	CU_ASSERT_EQUAL (i, 6);
	xmmsv_unref (value);

	/* should be consistent */
	value = xmmsv_build_list (XMMSV_LIST_ENTRY_FLOAT (2.5),
	                          XMMSV_LIST_ENTRY_STR ("No sir."),
	                          XMMSV_LIST_ENTRY_INT (66),
	                          XMMSV_LIST_ENTRY_INT (90),
	                          XMMSV_LIST_END);

	value2 = xmms_config_schema_register (NULL,  "schema_test", value, NULL, NULL);
	CU_ASSERT_PTR_NOT_NULL (value2);
	xmmsv_unref (value);
	xmmsv_unref (value2);

	/* should also be consistent */
	value = xmmsv_build_list (XMMSV_LIST_ENTRY_FLOAT (2.5),
	                          XMMSV_LIST_ENTRY_STR ("No sir."),
	                          XMMSV_LIST_END);

	value2 = xmms_config_schema_register (NULL,  "schema_test", value, NULL, NULL);
	CU_ASSERT_PTR_NOT_NULL (value2);
	xmmsv_unref (value);
	xmmsv_unref (value2);

	/* but this is not consistent */
	value = xmmsv_build_list (XMMSV_LIST_ENTRY_STR ("No sir."),
	                          XMMSV_LIST_ENTRY_FLOAT (2.5),
	                          XMMSV_LIST_END);

	value2 = xmms_config_schema_register (NULL,  "schema_test", value, NULL, NULL);
	CU_ASSERT_PTR_NULL (value2);
	xmmsv_unref (value);
}

CASE (test_file_io)
{
	xmmsv_t *value, *value2;
	gint32 i;
	gfloat f;
	const gchar *s;

	value = xmmsv_new_dict ();
	/* clear previously stored conf */
	xmms_config_set (config, "", value);
	xmmsv_unref (value);

	value = xmmsv_build_list (XMMSV_LIST_ENTRY_FLOAT (0.5),
	                          XMMSV_LIST_ENTRY_STR ("Yes sir."),
                              XMMSV_LIST_ENTRY_INT (6),
                              XMMSV_LIST_END);
	xmms_config_set (config, "test", value);
	xmmsv_unref (value);

	/* save and free config */
	xmms_config_save ();
	xmms_object_unref (config);
	/* read what was just written */
	config = xmms_config_new ("test.conf");
	value = xmms_config_get (config, "test");
	CU_ASSERT_PTR_NOT_NULL (value);
	CU_ASSERT_TRUE (xmmsv_is_type (value, XMMSV_TYPE_LIST));
	CU_ASSERT_TRUE (xmmsv_list_get (value, 0, &value2));
	CU_ASSERT_TRUE (xmmsv_get_float (value2, &f));
	CU_ASSERT_EQUAL (f, 0.5f);

	CU_ASSERT_TRUE (xmmsv_list_get (value, 1, &value2));
	CU_ASSERT_TRUE (xmmsv_get_string (value2, &s));
	CU_ASSERT_EQUAL (strcmp (s, "Yes sir."), 0);

	CU_ASSERT_TRUE (xmmsv_list_get (value, 2, &value2));
	CU_ASSERT_TRUE (xmmsv_get_int (value2, &i));
	CU_ASSERT_EQUAL (i, 6);
	xmmsv_unref (value);
}

CASE (test_old_api)
{
	gint32 i;
	const gchar *s;
	gchar *s2;
	double f;
	xmmsv_t *val1, *val2;

	val1 = xmmsv_new_dict ();
	/* clear previously stored conf */
	xmms_config_set (config, "", val1);
	xmmsv_unref (val1);

	xmms_config_property_t *prop;
	prop = xmms_config_property_register ("old_api.test", "10", NULL, NULL);
	CU_ASSERT_PTR_NOT_NULL (prop);
	i = xmms_config_property_get_int (prop);
	CU_ASSERT_EQUAL (i, 10);
	f = xmms_config_property_get_float (prop);
	CU_ASSERT_EQUAL (f, 10);
	s2 = xmms_config_property_get_string (prop);
	CU_ASSERT_PTR_NOT_NULL (s2);
	CU_ASSERT_EQUAL (strcmp (s2, "10"), 0);
	g_free (s2);
	xmms_config_property_set_data (prop, "20");
	i = xmms_config_property_get_int (prop);
	CU_ASSERT_EQUAL (i, 20);
	f = xmms_config_property_get_float (prop);
	CU_ASSERT_EQUAL (f, 20);
	s2 = xmms_config_property_get_string (prop);
	CU_ASSERT_PTR_NOT_NULL (s2);
	CU_ASSERT_EQUAL (strcmp (s2, "20"), 0);
	g_free (s2);
	xmms_object_unref (prop);

	/* non existent properties */
	prop = xmms_config_lookup ("I'm not here!");
	s2 = xmms_config_property_get_string (prop);
	xmms_object_unref (prop);
	CU_ASSERT_PTR_NULL (s2);

	/* compatibility with new api */
	val1 = xmms_config_get (config, "old_api.test");
	CU_ASSERT_FALSE (xmmsv_get_int (val1, &i));
	CU_ASSERT_TRUE (xmmsv_get_string (val1, &s));
	CU_ASSERT_EQUAL (strcmp (s, "20"), 0);
	xmmsv_unref (val1);
	val1 = xmms_config_get (config, "old_api");
	CU_ASSERT_TRUE (xmmsv_is_type (val1, XMMSV_TYPE_DICT));
	CU_ASSERT_TRUE (xmmsv_dict_get (val1, "test", &val2));
	CU_ASSERT_TRUE (xmmsv_get_string (val2, &s));
	CU_ASSERT_EQUAL (strcmp (s, "20"), 0);
	xmmsv_unref (val1);

	xmms_object_unref (prop);
	prop = xmms_config_property_register ("old_api.list.0", "-1", NULL, NULL);
	CU_ASSERT_PTR_NOT_NULL (prop);
	xmms_object_unref (prop);
	prop = xmms_config_property_register ("old_api.list.1", "-2", NULL, NULL);
	CU_ASSERT_PTR_NOT_NULL (prop);
	xmms_object_unref (prop);
	val1 = xmms_config_get (config, "old_api.list.0");
	CU_ASSERT_TRUE (xmmsv_get_string (val1, &s));
	CU_ASSERT_EQUAL (strcmp (s, "-1"), 0);
	xmmsv_unref (val1);
	val1 = xmms_config_get (config, "old_api.list.1");
	CU_ASSERT_TRUE (xmmsv_get_string (val1, &s));
	CU_ASSERT_EQUAL (strcmp (s, "-2"), 0);
	xmmsv_unref (val1);
	val1 = xmms_config_get (config, "old_api.list");
	CU_ASSERT_TRUE (xmmsv_list_get (val1, 0, &val2));
	CU_ASSERT_TRUE (xmmsv_get_string (val2, &s));
	CU_ASSERT_EQUAL (strcmp (s, "-1"), 0);

	CU_ASSERT_TRUE (xmmsv_list_get (val1, 1, &val2));
	CU_ASSERT_TRUE (xmmsv_get_string (val2, &s));
	CU_ASSERT_EQUAL (strcmp (s, "-2"), 0);
	xmmsv_unref (val1);
}

CASE (test_complex_paths)
{
	xmmsv_t *val1, *val2;
	gint i;
	float exp_f1, exp_f2;

	val2 = xmmsv_new_int (15);
	CU_ASSERT_TRUE (xmms_config_set (config, "long_test.key", val2));
	val1 = xmms_config_get (config, "long_test.key");
	CU_ASSERT_TRUE (xmmsv_get_int (val1, &i));
	CU_ASSERT_EQUAL (i, 15);
	xmmsv_unref (val1);
	xmmsv_unref (val2);

	val2 = xmmsv_new_int (10);
	CU_ASSERT_TRUE (xmms_config_set (config, "long_test.part.0.test.0", val2));
	xmmsv_unref (val2);

	exp_f1 = -8.234;
	exp_f2 = -88.5;
	val1 = xmmsv_build_list (XMMSV_LIST_ENTRY_FLOAT (exp_f1),
	                         XMMSV_LIST_ENTRY_STR ("Yes sir."),
                             XMMSV_LIST_ENTRY_INT (6),
                             XMMSV_LIST_END);
	xmms_config_set (config, "list_test", val1);
	xmmsv_unref (val1);

	val1 = xmmsv_build_dict (XMMSV_DICT_ENTRY_FLOAT ("key2", exp_f2),
	                         XMMSV_DICT_ENTRY_STR ("another2", "No sir."),
                             XMMSV_DICT_ENTRY_INT ("one more2", -10),
                             XMMSV_DICT_END);
	val2 = xmmsv_build_list (XMMSV_LIST_ENTRY_STR ("Yes sir!"),
                             XMMSV_LIST_ENTRY_INT (66),
                             XMMSV_LIST_END);
	xmmsv_dict_set (val1, "list inside", val2);
	CU_ASSERT_TRUE (xmms_config_set (config, "list_test.3", val1));
	xmmsv_unref (val1);
	xmmsv_unref (val2);
	val1 = xmms_config_get (config, "list_test.3.list inside.1");
	CU_ASSERT_TRUE (xmmsv_get_int (val1, &i));
	CU_ASSERT_EQUAL (i, 66);
	xmmsv_unref (val1);
}

CASE (test_list_config)
{
	xmmsv_t *val1, *val2;
	gint i;
	float f, exp_f1;
	const char *s;

	exp_f1 = 3.234;
	val1 = xmmsv_build_list (XMMSV_LIST_ENTRY_FLOAT (exp_f1),
	                         XMMSV_LIST_ENTRY_STR ("Yes sir."),
                             XMMSV_LIST_ENTRY_INT (6),
                             XMMSV_LIST_END);
	xmms_config_set (config, "first_list", val1);
	xmmsv_unref (val1);

	/* values should be deep copies */
	val2 = xmms_config_get (config, "first_list");
	CU_ASSERT_TRUE (xmmsv_is_type (val2, XMMSV_TYPE_LIST));
	CU_ASSERT_PTR_NOT_NULL (val2);
	CU_ASSERT_TRUE (xmmsv_list_get_float (val2, 0, &f));
	CU_ASSERT_EQUAL (f, exp_f1);
	CU_ASSERT_TRUE (xmmsv_list_get_string (val2, 1, &s));
	CU_ASSERT_EQUAL (strcmp (s, "Yes sir."), 0);
	CU_ASSERT_TRUE (xmmsv_list_get_int (val2, 2, &i));
	CU_ASSERT_EQUAL (i, 6);
	xmmsv_unref (val2);
	val2 = xmms_config_get (config, "first_list.0");
	CU_ASSERT_PTR_NOT_NULL (val2);
	CU_ASSERT_TRUE (xmmsv_get_float (val2, &f));
	CU_ASSERT_EQUAL (f, exp_f1);
	xmmsv_unref (val2);
	val2 = xmms_config_get (config, "first_list.1");
	CU_ASSERT_TRUE (xmmsv_get_string (val2, &s));
	CU_ASSERT_PTR_NOT_NULL (val2);
	CU_ASSERT_EQUAL (strcmp (s, "Yes sir."), 0);
	xmmsv_unref (val2);
	val2 = xmms_config_get (config, "first_list.2");
	CU_ASSERT_TRUE (xmmsv_get_int (val2, &i));
	CU_ASSERT_PTR_NOT_NULL (val2);
	CU_ASSERT_EQUAL (i, 6);
	xmmsv_unref (val2);
}

CASE (test_dict_config)
{
	xmmsv_t *val1, *val2;
	gint i;
	float f, exp_f1, exp_f2;
	const char *s;

	exp_f1 = 3.234;
	val1 = xmmsv_build_dict (XMMSV_DICT_ENTRY_FLOAT ("key", exp_f1),
                              XMMSV_DICT_ENTRY_STR ("another", "Yes sir."),
                              XMMSV_DICT_ENTRY_INT ("one more", 6),
                              XMMSV_DICT_END);
	xmms_config_set (config, "first", val1);
	xmmsv_unref (val1);

	exp_f2 = -343.324;
	val1 = xmmsv_build_dict (XMMSV_DICT_ENTRY_FLOAT ("key2", exp_f2),
	                         XMMSV_DICT_ENTRY_STR ("another2", "No sir."),
                             XMMSV_DICT_ENTRY_INT ("one more2", -10),
                             XMMSV_DICT_END);
	xmms_config_set (config, "second", val1);
	xmmsv_unref (val1);
	val1 = xmms_config_get (config, "first");
	CU_ASSERT_PTR_NOT_NULL (val1);
	CU_ASSERT_TRUE (xmmsv_is_type (val1, XMMSV_TYPE_DICT));
	xmmsv_unref (val1);
	/* values should be deep copies */
	val2 = xmms_config_get (config, "first");
	CU_ASSERT_PTR_NOT_NULL (val2);
	CU_ASSERT_TRUE (xmmsv_dict_entry_get_float (val2, "key", &f));
	CU_ASSERT_EQUAL (f, exp_f1);
	CU_ASSERT_TRUE (xmmsv_dict_entry_get_string (val2, "another", &s));
	CU_ASSERT_EQUAL (strcmp (s, "Yes sir."), 0);
	CU_ASSERT_TRUE (xmmsv_dict_entry_get_int (val2, "one more", &i));
	CU_ASSERT_EQUAL (i, 6);
	xmmsv_unref (val2);
	val2 = xmms_config_get (config, "second");
	CU_ASSERT_PTR_NOT_NULL (val2);
	CU_ASSERT_TRUE (xmmsv_dict_entry_get_float (val2, "key2", &f));
	CU_ASSERT_EQUAL (f, exp_f2);
	CU_ASSERT_TRUE (xmmsv_dict_entry_get_string (val2, "another2", &s));
	CU_ASSERT_EQUAL (strcmp (s, "No sir."), 0);
	CU_ASSERT_TRUE (xmmsv_dict_entry_get_int (val2, "one more2", &i));
	CU_ASSERT_EQUAL (i, -10);
	xmmsv_unref (val2);

	val2 = xmms_config_get (config, "first.key");
	CU_ASSERT_PTR_NOT_NULL (val2);
	CU_ASSERT_TRUE (xmmsv_get_float (val2, &f));
	CU_ASSERT_EQUAL (f, exp_f1);
	xmmsv_unref (val2);
	val2 = xmms_config_get (config, "first.another");
	CU_ASSERT_TRUE (xmmsv_get_string (val2, &s));
	CU_ASSERT_EQUAL (strcmp (s, "Yes sir."), 0);
	xmmsv_unref (val2);
	val2 = xmms_config_get (config, "first.one more");
	CU_ASSERT_TRUE (xmmsv_get_int (val2, &i));
	CU_ASSERT_EQUAL (i, 6);
	xmmsv_unref (val2);
}

CASE (test_consistency_errors)
{
	xmmsv_t *val1, *val2;

	val1 = xmmsv_new_list ();
	val2 = xmmsv_new_int (10);
	xmmsv_list_append (val1, val2);
	xmmsv_unref (val2);
	val2 = xmmsv_new_float (3.5765);
	xmmsv_list_append (val1, val2);
	xmmsv_unref (val2);
	val2 = xmmsv_new_string ("hiya hiya");
	xmmsv_list_append (val1, val2);
	xmmsv_unref (val2);
	val2 = xmmsv_build_list (XMMSV_LIST_ENTRY_STR ("hello there"),
	                         XMMSV_LIST_ENTRY_INT (35),
                             XMMSV_LIST_ENTRY_FLOAT (6.543),
                             XMMSV_LIST_END);
	xmmsv_list_append (val1, val2);
	xmmsv_unref (val2);
	val2 = xmmsv_build_dict (XMMSV_DICT_ENTRY_FLOAT ("key", 3.22123),
	                         XMMSV_DICT_ENTRY_STR ("another", "Yes sir."),
	                         XMMSV_DICT_ENTRY_INT ("one more", 6),
	                         XMMSV_LIST_END);
	xmmsv_list_append (val1, val2);
	xmmsv_unref (val2);
	CU_ASSERT_TRUE (xmms_config_set (config, "consistent node", val1));
	/*FIXME this is leaking! */
	CU_ASSERT_TRUE (xmms_config_set (config, "consistent node", val1));
	val2 = xmmsv_new_int (24);
	xmmsv_list_append (val1, val2);
	xmmsv_unref (val2);
	CU_ASSERT_TRUE (xmms_config_set (config, "consistent node", val1));
	CU_ASSERT_TRUE (xmmsv_list_remove (val1, 5));
	CU_ASSERT_TRUE (xmmsv_list_remove (val1, 4));
	CU_ASSERT_TRUE (xmms_config_set (config, "consistent node", val1));
	CU_ASSERT_TRUE (xmmsv_list_remove (val1, 0));
	/* should be inconsistent now */
	CU_ASSERT_TRUE (xmms_config_set (config, "consistent node", val1));
	CU_ASSERT_TRUE (xmmsv_list_clear (val1));
	/* but empty lists/dicts should work to clear the node */
	CU_ASSERT_TRUE (xmms_config_set (config, "consistent node", val1));
	xmmsv_unref (val1);
	/* setting an empty node will allow setting a new type for the node */
	val1 = xmmsv_new_none ();
	CU_ASSERT_TRUE (xmms_config_set (config, "consistent node",
	                                 val1));
	xmmsv_unref (val1);
	val2 = xmmsv_new_int (199);
	CU_ASSERT_TRUE (xmms_config_set (config, "consistent node",
	                                 val2));
	xmmsv_unref (val2);
}

CASE (test_config_get)
{
	xmmsv_t *val;
	int32_t i;
	const char *s;

	val = xmms_config_get (config, "shouldn't be here");
	CU_ASSERT_PTR_NULL (val);
	xmmsv_unref (val);

	val = xmms_config_get (config, "test_int");
	CU_ASSERT_PTR_NOT_NULL (val);
	CU_ASSERT_TRUE (xmmsv_get_int (val, &i));
	CU_ASSERT_EQUAL (i, 4242);
	xmmsv_unref (val);

	val = xmms_config_get (config, "test_string");
	CU_ASSERT_PTR_NOT_NULL (val);
	CU_ASSERT_TRUE (xmmsv_get_string (val, &s));
	CU_ASSERT_PTR_NOT_NULL_FATAL (s);
	CU_ASSERT_EQUAL (strcmp (s, "hello string"), 0);
	xmmsv_unref (val);

	/* get root */
	val = xmms_config_get (config, "");
	CU_ASSERT_PTR_NOT_NULL (val);
	CU_ASSERT_TRUE (xmmsv_is_type (val, XMMSV_TYPE_DICT));
	xmmsv_unref (val);
}

CASE (test_config_set)
{
	xmmsv_t *val;

	val = xmmsv_new_int (4242);
	CU_ASSERT_TRUE (xmms_config_set (config, "test_int", val));
	xmmsv_unref (val);
	val = xmmsv_new_float (218.234);
	CU_ASSERT_TRUE (xmms_config_set (config, "test_float", val));
	xmmsv_unref (val);
	val = xmmsv_new_string ("hello string");
	CU_ASSERT_TRUE (xmms_config_set (config, "test_string", val));
	xmmsv_unref (val);
	val = xmmsv_new_list ();
	xmmsv_list_append_int (val, 2424);
	CU_ASSERT_TRUE (xmms_config_set (config, "test_list", val));
	xmmsv_unref (val);
	val = xmmsv_new_dict ();
	xmmsv_dict_set_int (val, "int_key", 242424);
	CU_ASSERT_TRUE (xmms_config_set (config, "test_dict", val));

	xmmsv_unref (val);
}

