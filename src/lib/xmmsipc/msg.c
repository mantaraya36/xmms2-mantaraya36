/*  XMMS2 - X Music Multiplexer System
 *  Copyright (C) 2003	Peter Alm, Tobias Rundstr�m, Anders Gustafsson
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


#include <glib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <errno.h>
#include <time.h>
#include "xmmsc/xmmsc_ipc_transport.h"
#include "xmmsc/xmmsc_ipc_msg.h"

typedef union {
	struct {
		guint32 object;
		guint32 cmd;
		guint32 cid;
		guint32 length;
		guint8 data[0];
	} header;
	guint8 rawdata[0];
} xmms_ipc_msg_data_t;


struct xmms_ipc_msg_St {
	xmms_ipc_msg_data_t *data;
	guint32 get_pos;
	guint32 size;
	guint32 xfered;
};

xmms_ipc_msg_t *
xmms_ipc_msg_alloc (void)
{
	xmms_ipc_msg_t *msg;
	
	msg = g_new0 (xmms_ipc_msg_t, 1);
	msg->data = g_malloc0 (XMMS_IPC_MSG_DEFAULT_SIZE);
	msg->size = XMMS_IPC_MSG_DEFAULT_SIZE;
	return msg;
}

xmms_ipc_msg_t *
xmms_ipc_msg_new (guint32 object, guint32 cmd)
{
	xmms_ipc_msg_t *msg = xmms_ipc_msg_alloc ();

	xmms_ipc_msg_set_cmd (msg, cmd);
	xmms_ipc_msg_set_object (msg, object);

	return msg;
}

void
xmms_ipc_msg_destroy (xmms_ipc_msg_t *msg)
{
	g_return_if_fail (msg);
	g_free (msg->data);
	g_free (msg);
}

void
xmms_ipc_msg_set_length (xmms_ipc_msg_t *msg, guint32 len)
{
	g_return_if_fail (msg);
	msg->data->header.length = g_htonl (len);
}

guint32
xmms_ipc_msg_get_length (const xmms_ipc_msg_t *msg)
{
	g_return_val_if_fail (msg, 0);

	return g_ntohl (msg->data->header.length);
}

guint32
xmms_ipc_msg_get_object (const xmms_ipc_msg_t *msg)
{
	g_return_val_if_fail (msg, 0);

	return g_ntohl (msg->data->header.object);
}

void
xmms_ipc_msg_set_object (xmms_ipc_msg_t *msg, guint32 object)
{
	g_return_if_fail (msg);

	msg->data->header.object = g_htonl (object);
}


guint32
xmms_ipc_msg_get_cmd (const xmms_ipc_msg_t *msg)
{
	g_return_val_if_fail (msg, 0);

	return g_ntohl (msg->data->header.cmd);
}

void
xmms_ipc_msg_set_cmd (xmms_ipc_msg_t *msg, guint32 cmd)
{
	g_return_if_fail (msg);

	msg->data->header.cmd = g_htonl (cmd);
}

void
xmms_ipc_msg_set_cid (xmms_ipc_msg_t *msg, guint32 cid)
{
	msg->data->header.cid = g_htonl (cid);
}

guint32
xmms_ipc_msg_get_cid (const xmms_ipc_msg_t *msg)
{
	g_return_val_if_fail (msg, 0);

	return g_ntohl (msg->data->header.cid);
}

/**
 * Try to write message to transport. If full message isn't written
 * the message will keep track of the amount of data written and not
 * write already written data next time.
 *
 * @returns TRUE if full message was written, FALSE otherwise.
 *               disconnected is set if transport was disconnected
 */
gboolean
xmms_ipc_msg_write_transport (xmms_ipc_msg_t *msg, xmms_ipc_transport_t *transport, gboolean *disconnected)
{
	guint ret, len;

	g_return_val_if_fail (msg, FALSE);
	g_return_val_if_fail (msg->data, FALSE);
	g_return_val_if_fail (transport, FALSE);

	len = xmms_ipc_msg_get_length (msg) + XMMS_IPC_MSG_HEAD_LEN;
	
	g_return_val_if_fail (len > msg->xfered, TRUE);
	
	ret = xmms_ipc_transport_write (transport, 
					msg->data->rawdata + msg->xfered,
					len - msg->xfered);
	if (ret == -1) {
		if (errno == EAGAIN || errno == EINTR)
			return FALSE;
	} else if (ret == 0) {
		if (disconnected)
			*disconnected = TRUE;
	} else {
		msg->xfered += ret;
	}

	return len == msg->xfered;
}

/**
 * Try to read message from transport into msg.
 *
 * @returns TRUE if message is fully read.
 */
gboolean
xmms_ipc_msg_read_transport (xmms_ipc_msg_t *msg, xmms_ipc_transport_t *transport, gboolean *disconnected)
{
	guint ret, len;

	g_return_val_if_fail (msg, FALSE);
	g_return_val_if_fail (transport, FALSE);

	while (TRUE) {
		len = XMMS_IPC_MSG_HEAD_LEN;
		if (msg->xfered >= XMMS_IPC_MSG_HEAD_LEN) {
			len += xmms_ipc_msg_get_length (msg);
			if (len > msg->size) {
				msg->size = len;
				msg->data = g_realloc (msg->data, msg->size);
			}
			if (msg->xfered == len)
				return TRUE;
		}


		g_return_val_if_fail (msg->xfered < len, FALSE);

		ret = xmms_ipc_transport_read (transport, 
					       msg->data->rawdata + msg->xfered,
					       len - msg->xfered);
		
		if (ret == -1) {
			if (errno == EAGAIN || errno == EINTR)
				return FALSE;
		} else if (ret == 0) {
			if (disconnected)
				*disconnected = TRUE;
			return FALSE;
		} else {
			msg->xfered += ret;
		}
	}
}


static gpointer
xmms_ipc_msg_put_data (xmms_ipc_msg_t *msg, gconstpointer data, guint len)
{
	g_return_val_if_fail (msg, NULL);
	
	if ((xmms_ipc_msg_get_length (msg) + XMMS_IPC_MSG_HEAD_LEN + len) > msg->size) {
		gint reallocsize = XMMS_IPC_MSG_DEFAULT_SIZE;

		if (len > XMMS_IPC_MSG_DEFAULT_SIZE) 
			reallocsize = len;

		/* Realloc data portion */
		msg->data = g_realloc (msg->data, msg->size + reallocsize);
		msg->size += reallocsize;
	}
	memcpy (&msg->data->header.data[xmms_ipc_msg_get_length (msg)], data, len);
	xmms_ipc_msg_set_length (msg, xmms_ipc_msg_get_length (msg) + len);

	return &msg->data->rawdata[xmms_ipc_msg_get_length (msg) - len];
}

gpointer
xmms_ipc_msg_put_uint32 (xmms_ipc_msg_t *msg, guint32 v)
{
	v = g_htonl (v);
	return xmms_ipc_msg_put_data (msg, &v, sizeof (v));
}

gpointer
xmms_ipc_msg_put_int32 (xmms_ipc_msg_t *msg, gint32 v)
{
	v = g_htonl (v);
	return xmms_ipc_msg_put_data (msg, &v, sizeof (v));
}

gpointer
xmms_ipc_msg_put_float (xmms_ipc_msg_t *msg, gfloat v)
{
	/** @todo do we need to convert ? */
	return xmms_ipc_msg_put_data (msg, &v, sizeof (v));
}

gpointer
xmms_ipc_msg_put_string (xmms_ipc_msg_t *msg, const char *str)
{
	if (!msg)
		return NULL;

	if (!str)
		return xmms_ipc_msg_put_uint32 (msg, 0);
	
	xmms_ipc_msg_put_uint32 (msg, strlen (str) + 1);

	return xmms_ipc_msg_put_data (msg, str, strlen (str) + 1);
}

void
xmms_ipc_msg_get_reset (xmms_ipc_msg_t *msg)
{
	msg->get_pos = 0;
}

static gboolean
xmms_ipc_msg_get_data (xmms_ipc_msg_t *msg, gpointer buf, guint len)
{
	if (!msg || ((msg->get_pos + len) > xmms_ipc_msg_get_length (msg)))
		return FALSE;

	if (buf)
		memcpy (buf, &msg->data->header.data[msg->get_pos], len);
	msg->get_pos += len;

	return TRUE;
}

gboolean
xmms_ipc_msg_get_uint32 (xmms_ipc_msg_t *msg, guint32 *v)
{
	gboolean ret;
	ret = xmms_ipc_msg_get_data (msg, v, sizeof (*v));
	if (v)
		*v = g_ntohl (*v);
	return ret;
}

gboolean
xmms_ipc_msg_get_int32 (xmms_ipc_msg_t *msg, gint32 *v)
{
	gboolean ret;
	ret = xmms_ipc_msg_get_data (msg, v, sizeof (*v));
	if (v)
		*v = g_ntohl (*v);
	return ret;
}

gboolean
xmms_ipc_msg_get_float (xmms_ipc_msg_t *msg, gfloat *v)
{
	gboolean ret;
	ret = xmms_ipc_msg_get_data (msg, v, sizeof (*v));
	/** @todo do we need to convert? */
	return ret;
}

gboolean
xmms_ipc_msg_get_string_alloc (xmms_ipc_msg_t *msg, char **buf, guint *len)
{
	if (!xmms_ipc_msg_get_uint32 (msg, len))
		return FALSE;
	
	*buf = g_malloc0 (*len+1);

	if (!*buf)
		return FALSE;

	if (!xmms_ipc_msg_get_data (msg, *buf, *len))
		return FALSE;

	(*buf)[*len] = '\0';

	return TRUE;
}

gboolean
xmms_ipc_msg_get_string (xmms_ipc_msg_t *msg, char *buf, guint maxlen)
{
	guint32 len;

	if (buf) {
		buf[maxlen - 1] = '\0';
		maxlen--;
	}
	if (!xmms_ipc_msg_get_uint32 (msg, &len))
		return FALSE;
	if (!xmms_ipc_msg_get_data (msg, buf, MIN (maxlen, len)))
		return FALSE;
	if (maxlen < len) {
		xmms_ipc_msg_get_data (msg, NULL, len - maxlen);
	}
	return TRUE;
}

gboolean
xmms_ipc_msg_get (xmms_ipc_msg_t *msg, ...)
{
	va_list ap;
	void *dest;
	xmms_ipc_msg_arg_type_t type;
	gint len;

	va_start (ap, msg);

	while (42) {
		type = va_arg (ap, xmms_ipc_msg_arg_type_t);
		switch (type){
			case XMMS_IPC_MSG_ARG_TYPE_UINT32:
				dest = va_arg (ap, guint32 *);
				if (!xmms_ipc_msg_get_uint32 (msg, dest)) {
					return FALSE;
				}
				break;
			case XMMS_IPC_MSG_ARG_TYPE_INT32:
				dest = va_arg (ap, gint32 *);
				if (!xmms_ipc_msg_get_int32 (msg, dest)) {
					return FALSE;
				}
				break;
			case XMMS_IPC_MSG_ARG_TYPE_FLOAT:
				dest = va_arg (ap, gfloat *);
				if (!xmms_ipc_msg_get_float (msg, dest)) {
					return FALSE;
				}
				break;
			case XMMS_IPC_MSG_ARG_TYPE_STRING:
				len = va_arg (ap, gint);
				dest = va_arg (ap, char *);
				if (!xmms_ipc_msg_get_string (msg, dest, len)) {
					return FALSE;
				}
				break;
			case XMMS_IPC_MSG_ARG_TYPE_DATA:
				len = va_arg (ap, gint);
				dest = va_arg (ap, void *);
				if (!xmms_ipc_msg_get_data (msg, dest, len)) {
					return FALSE;
				}
				break;
			case XMMS_IPC_MSG_ARG_TYPE_END:
				va_end (ap);
				return TRUE;
		}
		
	}
}
