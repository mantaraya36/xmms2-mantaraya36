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


/** @file
 *  Functions to serialize (save/restore) collections.
 */

#include "xmmspriv/xmms_collserial.h"
#include "xmmspriv/xmms_collection.h"
#include "xmmspriv/xmms_medialib.h"


/* Internal helper structures */

typedef struct {
	xmms_medialib_session_t *session;
	guint collid;
	xmms_collection_namespace_id_t nsid;
} coll_dbwrite_t;


static xmmsv_coll_t *xmms_collection_dbread_operator (xmms_medialib_session_t *session, gint id, xmmsv_coll_type_t type);
static guint xmms_collection_dbwrite_operator (xmms_medialib_session_t *session, guint collid, xmmsv_coll_t *coll);

static void dbwrite_operator (void *key, void *value, void *udata);
static void dbwrite_coll_attributes (const char *key, xmmsv_t *value, void *udata);
static void dbwrite_strip_tmpprops (void *key, void *value, void *udata);

static gint value_get_dict_int (xmmsv_t *val, const gchar *key);
static const gchar *value_get_dict_string (xmmsv_t *val, const gchar *key);



/** Save the collection DAG in the database.
 *
 * @param dag  The collection DAG to save.
 */
void
xmms_collection_dag_save (xmms_coll_dag_t *dag)
{
	gint i;
	xmms_medialib_session_t *session;

	session = xmms_medialib_begin_write ();

	/* Empty Collection* tables */
	xmms_medialib_select (session, "DELETE FROM CollectionAttributes", NULL);
	xmms_medialib_select (session, "DELETE FROM CollectionConnections", NULL);
	xmms_medialib_select (session, "DELETE FROM CollectionIdlists", NULL);
	xmms_medialib_select (session, "DELETE FROM CollectionLabels", NULL);
	xmms_medialib_select (session, "DELETE FROM CollectionOperators", NULL);

	/* Write all collections in all namespaces */
	coll_dbwrite_t dbinfos = { session, 1, 0 }; /* ids start at 1 */
	for (i = 0; i < XMMS_COLLECTION_NUM_NAMESPACES; ++i) {
		dbinfos.nsid = i;
		xmms_collection_foreach_in_namespace (dag, i, dbwrite_operator, &dbinfos);
	}

	xmms_collection_foreach_in_namespace (dag, XMMS_COLLECTION_NSID_ALL,
	                                      dbwrite_strip_tmpprops, NULL);

	xmms_medialib_end (session);
}

/** Restore the collection DAG from the database.
 *
 * @param dag  The collection DAG to restore to.
 */
void
xmms_collection_dag_restore (xmms_coll_dag_t *dag)
{
	xmmsv_coll_t *coll = NULL;
	xmms_medialib_session_t *session;
	xmmsv_t *cmdval;
	const gchar *query;
	GList *res;
	gint previd;

	session = xmms_medialib_begin ();

	/* Fetch all label-coll_operator for all namespaces, register in table */
	query = "SELECT op.id AS id, lbl.name AS label, "
	        "       lbl.namespace AS nsid, op.type AS type "
	        "FROM CollectionOperators AS op, CollectionLabels as lbl "
	        "WHERE op.id=lbl.collid "
	        "ORDER BY id";
	res = xmms_medialib_select (session, query, NULL);

	previd = -1;

	while (res) {
		gint id, type, nsid;
		const gchar *label;

		cmdval = (xmmsv_t*) res->data;
		id = value_get_dict_int (cmdval, "id");
		type = value_get_dict_int (cmdval, "type");
		nsid = value_get_dict_int (cmdval, "nsid");
		label = value_get_dict_string (cmdval, "label");

		/* Do not duplicate operator if same id */
		if (previd < 0 || id != previd) {
			coll = xmms_collection_dbread_operator (session, id, type);
			previd = id;
		}
		else {
			xmmsv_coll_ref (coll);  /* New label references the coll */
		}

		xmms_collection_dag_replace (dag, nsid, g_strdup (label), coll);

		xmmsv_unref (cmdval);
		res = g_list_delete_link (res, res);
	}

	xmms_medialib_end (session);

	/* FIXME: validate ? */

	/* Link references in collections to actual operators */
	xmms_collection_apply_to_all_collections (dag, bind_all_references, NULL);
}

/** Given a collection id, query the DB to build the corresponding
 *  collection DAG.
 *
 * @param session  The medialib session connected to the DB.
 * @param id  The id of the collection to create.
 * @param type  The type of the collection operator.
 * @return  The created collection DAG.
 */
static xmmsv_coll_t *
xmms_collection_dbread_operator (xmms_medialib_session_t *session,
                                 gint id, xmmsv_coll_type_t type)
{
	xmmsv_coll_t *coll;
	xmmsv_coll_t *op;
	GList *res;
	GList *n;
	xmmsv_t *cmdval;
	gchar query[256];

	coll = xmmsv_coll_new (type);

	/* Retrieve the attributes */
	g_snprintf (query, sizeof (query),
	            "SELECT attr.key AS key, attr.value AS value "
	            "FROM CollectionOperators AS op, CollectionAttributes AS attr "
	            "WHERE op.id=%d AND attr.collid=op.id", id);

	res = xmms_medialib_select (session, query, NULL);
	for (n = res; n; n = n->next) {
		const gchar *key, *value;

		cmdval = (xmmsv_t*) n->data;
		key = value_get_dict_string (cmdval, "key");
		value = value_get_dict_string (cmdval, "value");
		xmmsv_coll_attribute_set (coll, key, value);

		xmmsv_unref (n->data);
	}
	g_list_free (res);

	/* Retrieve the idlist */
	g_snprintf (query, sizeof (query),
	            "SELECT idl.mid AS mid "
	            "FROM CollectionOperators AS op, CollectionIdlists AS idl "
	            "WHERE op.id=%d AND idl.collid=op.id "
	            "ORDER BY idl.position", id);

	res = xmms_medialib_select (session, query, NULL);
	for (n = res; n; n = n->next) {

		cmdval = (xmmsv_t *) n->data;
		xmmsv_coll_idlist_append (coll, value_get_dict_int (cmdval, "mid"));

		xmmsv_unref (cmdval);
	}
	g_list_free (res);

	/* Retrieve the operands */
	g_snprintf (query, sizeof (query),
	            "SELECT op.id AS id, op.type AS type "
	            "FROM CollectionOperators AS op, CollectionConnections AS conn "
	            "WHERE conn.to_id=%d AND conn.from_id=op.id", id);

	res = xmms_medialib_select (session, query, NULL);
	for (n = res; n; n = n->next) {
		gint _id;
		gint type;

		cmdval = (xmmsv_t *) n->data;
		_id = value_get_dict_int (cmdval, "id");
		type = value_get_dict_int (cmdval, "type");

		op = xmms_collection_dbread_operator (session, _id, type);
		xmmsv_coll_add_operand (coll, op);

		xmmsv_coll_unref (op);
		xmmsv_unref (cmdval);
	}
	g_list_free (res);

	return coll;
}

/** Write the given operator to the database under the given id.
 *
 * @param session  The medialib session connected to the DB.
 * @param collid  The id under which to save the collection.
 * @param coll  The structure of the collection to save.
 * @return  The next free collection id.
 */
static guint
xmms_collection_dbwrite_operator (xmms_medialib_session_t *session,
                                  guint collid, xmmsv_coll_t *coll)
{
	gchar query[128];
	xmms_medialib_entry_t entry;
	xmmsv_list_iter_t *it;
	gint i;
	xmmsv_coll_t *op;
	xmmsv_t *attrs;
	gint newid, nextid;
	coll_dbwrite_t dbwrite_infos = { session, collid, 0 };

	/* Write operator */
	g_snprintf (query, sizeof (query),
	            "INSERT INTO CollectionOperators VALUES(%d, %d)",
	            collid, xmmsv_coll_get_type (coll));

	xmms_medialib_select (session, query, NULL);

	/* Write attributes */
	attrs = xmmsv_coll_attributes_get (coll);
	xmmsv_dict_foreach (attrs, dbwrite_coll_attributes, &dbwrite_infos);
	attrs = NULL; /* no unref needed. */

	/* Write idlist */
	xmmsv_get_list_iter (xmmsv_coll_idlist_get (coll), &it);
	for (xmmsv_list_iter_first (it), i = 0;
	     xmmsv_list_iter_valid (it);
	     xmmsv_list_iter_next (it), i++) {

		xmmsv_list_iter_entry_int (it, &entry);
		g_snprintf (query, sizeof (query),
		            "INSERT INTO CollectionIdlists VALUES(%d, %d, %d)",
		            collid, i, entry);

		xmms_medialib_select (session, query, NULL);
	}
	xmmsv_list_iter_explicit_destroy (it);

	/* Save operands and connections (don't recurse in ref operand) */
	newid = collid + 1;
	if (xmmsv_coll_get_type (coll) != XMMS_COLLECTION_TYPE_REFERENCE) {
		xmmsv_t *tmp;
		xmmsv_list_iter_t *iter;

		xmmsv_get_list_iter (xmmsv_coll_operands_get (coll), &iter);

		for (xmmsv_list_iter_first (iter);
		     xmmsv_list_iter_valid (iter);
		     xmmsv_list_iter_next (iter)) {

			xmmsv_list_iter_entry (iter, &tmp);
			xmmsv_get_coll (tmp, &op);

			nextid = xmms_collection_dbwrite_operator (session, newid, op);
			g_snprintf (query, sizeof (query),
			            "INSERT INTO CollectionConnections VALUES(%d, %d)",
			            newid, collid);
			xmms_medialib_select (session, query, NULL);
			newid = nextid;
		}
		xmmsv_list_iter_explicit_destroy (iter);
	}

	/* return next available id */
	return newid;
}

/* For all label-operator pairs, write the operator and all its
 * operands to the DB recursively. */
static void
dbwrite_operator (void *key, void *value, void *udata)
{
	gchar *query;
	gchar *label = key;
	xmmsv_coll_t *coll = value;
	coll_dbwrite_t *dbinfos = udata;
	gchar *esc_label;
	gint serial_id;

	/* Only serialize each operator once, get previous id if exists */
	if (!xmms_collection_get_int_attr (coll, XMMS_COLLSERIAL_ATTR_ID, &serial_id)) {
		serial_id = dbinfos->collid;
		dbinfos->collid = xmms_collection_dbwrite_operator (dbinfos->session,
		                                                    dbinfos->collid, coll);
		xmms_collection_set_int_attr (coll, XMMS_COLLSERIAL_ATTR_ID, serial_id);
	}

	esc_label = sqlite_prepare_string (label);
	query = g_strdup_printf ("INSERT INTO CollectionLabels VALUES(%d, %d, %s)",
	                         serial_id, dbinfos->nsid, esc_label);
	xmms_medialib_select (dbinfos->session, query, NULL);

	g_free (query);
	g_free (esc_label);
}

/* Write all attributes of a collection to the DB. */
static void
dbwrite_coll_attributes (const char *key, xmmsv_t *value, void *udata)
{
	gchar *query;
	coll_dbwrite_t *dbwrite_infos = udata;
	gchar *esc_key;
	gchar *esc_val;
	const gchar *s;
	int r;

	r = xmmsv_get_string (value, &s);
	g_return_if_fail (r);

	esc_key = sqlite_prepare_string (key);
	esc_val = sqlite_prepare_string (s);
	query = g_strdup_printf ("INSERT INTO CollectionAttributes VALUES(%d, %s, %s)",
	                         dbwrite_infos->collid, esc_key, esc_val);
	xmms_medialib_select (dbwrite_infos->session, query, NULL);

	g_free (query);
	g_free (esc_key);
	g_free (esc_val);
}

/* Remove all temp utility properties used to write collections to the DB. */
static void
dbwrite_strip_tmpprops (void *key, void *value, void *udata)
{
	xmmsv_coll_t *coll = value;
	xmmsv_coll_attribute_remove (coll, XMMS_COLLSERIAL_ATTR_ID);
}


/* Extract the int value out of a xmmsv_t object. */
static gint
value_get_dict_int (xmmsv_t *val, const gchar *key)
{
	gint i;
	xmmsv_dict_entry_get_int (val, key, &i);
	return i;
}

/* Extract the string value out of a xmmsv_t object. */
static const gchar *
value_get_dict_string (xmmsv_t *val, const gchar *key)
{
	const gchar *s;
	xmmsv_dict_entry_get_string (val, key, &s);
	return s;
}
