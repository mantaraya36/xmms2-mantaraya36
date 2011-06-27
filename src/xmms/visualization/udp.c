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

#include <stdlib.h>
#include "common.h"

static gboolean
udpwatcher (GIOChannel *src, GIOCondition cond, xmms_visualization_t *vis)
{
	struct sockaddr_storage from;
	socklen_t sl = sizeof (from);
	xmmsc_vis_udp_timing_t packet_d;
	char* packet = packet_init_timing (&packet_d);
	if ((recvfrom (vis->socket, packet, packet_d.size, 0, (struct sockaddr *)&from, &sl)) > 0) {
		if (*packet_d.__unaligned_type == 'H') {
			xmms_vis_client_t *c;
			int32_t id;

			XMMSC_VIS_UNALIGNED_READ (id, packet_d.__unaligned_id, int32_t);
			id = ntohl (id);

			/* debug code starts
			char adrb[INET6_ADDRSTRLEN];
			struct sockaddr_in6 *a = (struct sockaddr_in6 *)&from;
			printf ("Client address: %s:%d, %d\n", inet_ntop (AF_INET6, &a->sin6_addr,
					adrb, INET6_ADDRSTRLEN), a->sin6_port, id);
			 debug code ends */
			g_mutex_lock (vis->clientlock);
			c = get_client (id);
			if (!c || c->type != VIS_UDP) {
				g_mutex_unlock (vis->clientlock);
				return TRUE;
			}
			/* save client address according to id */
			memcpy (&c->transport.udp.addr, &from, sizeof (from));
			c->transport.udp.socket[0] = 1;
			c->transport.udp.grace = 2000;
			g_mutex_unlock (vis->clientlock);
		} else if (*packet_d.__unaligned_type == 'T') {
			struct timeval time;
			xmms_vis_client_t *c;
			int32_t id;

			XMMSC_VIS_UNALIGNED_READ (id, packet_d.__unaligned_id, int32_t);
			id = ntohl (id);

			g_mutex_lock (vis->clientlock);
			c = get_client (id);
			if (!c || c->type != VIS_UDP) {
				g_mutex_unlock (vis->clientlock);
				free (packet);
				return TRUE;
			}
			c->transport.udp.grace = 2000;
			g_mutex_unlock (vis->clientlock);

			/* give pong */
			gettimeofday (&time, NULL);

			struct timeval cts, sts;

			XMMSC_VIS_UNALIGNED_READ (cts.tv_sec, &packet_d.__unaligned_clientstamp[0], int32_t);
			XMMSC_VIS_UNALIGNED_READ (cts.tv_usec, &packet_d.__unaligned_clientstamp[1], int32_t);
			cts.tv_sec = ntohl (cts.tv_sec);
			cts.tv_usec = ntohl (cts.tv_usec);

			sts.tv_sec = time.tv_sec - cts.tv_sec;
			sts.tv_usec = time.tv_usec - cts.tv_usec;
			if (sts.tv_usec < 0) {
				sts.tv_sec--;
				sts.tv_usec += 1000000;
			}

			XMMSC_VIS_UNALIGNED_WRITE (&packet_d.__unaligned_serverstamp[0],
			                           (int32_t)htonl (sts.tv_sec), int32_t);
			XMMSC_VIS_UNALIGNED_WRITE (&packet_d.__unaligned_serverstamp[1],
			                           (int32_t)htonl (sts.tv_usec), int32_t);

			sendto (vis->socket, packet, packet_d.size, 0, (struct sockaddr *)&from, sl);

			/* new debug:
			printf ("Timings: local %f, remote %f, diff %f\n", tv2ts (&time), net2ts (packet_d.clientstamp), net2ts (packet_d.clientstamp) - tv2ts (&time));
			 ends */
		} else {
			xmms_log_error ("Received invalid UDP package!");
		}
	}
	free (packet);
	return TRUE;
}

int32_t
init_udp (xmms_visualization_t *vis, int32_t id, xmms_error_t *err)
{
	// TODO: we need the currently used port, not only the default one! */
	int32_t port = XMMS_DEFAULT_TCP_PORT;
	xmms_vis_client_t *c;

	// setup socket if needed
	if (!xmms_socket_valid (vis->socket)) {
		struct addrinfo hints;
		struct addrinfo *result, *rp;
		int s;

		memset (&hints, 0, sizeof (hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_DGRAM;
		hints.ai_flags = AI_PASSIVE;
		hints.ai_protocol = 0;

		if ((s = getaddrinfo (NULL, G_STRINGIFY (XMMS_DEFAULT_TCP_PORT), &hints, &result)) != 0)
		{
			xmms_log_error ("Could not setup socket! getaddrinfo: %s", gai_strerror (s));
			xmms_error_set (err, XMMS_ERROR_NO_SAUSAGE, "Could not setup socket!");
			return -1;
		}

		for (rp = result; rp != NULL; rp = rp->ai_next) {
			vis->socket = socket (rp->ai_family, rp->ai_socktype, rp->ai_protocol);
			if (!xmms_socket_valid (vis->socket)) {
				continue;
			}
			if (bind (vis->socket, rp->ai_addr, rp->ai_addrlen) != -1) {
				break;
			} else {
				close (vis->socket);
			}
		}
		if (rp == NULL) {
			xmms_log_error ("Could not bind socket!");
			xmms_error_set (err, XMMS_ERROR_NO_SAUSAGE, "Could not bind socket!");
			freeaddrinfo (result);
			return -1;
		}
		freeaddrinfo (result);

		/* register into mainloop: */
/* perhaps needed, perhaps not .. #ifdef __WIN32__
		vis->socketio = g_io_channel_win32_new_socket (vis->socket);
#else */
		vis->socketio = g_io_channel_unix_new (vis->socket);
/*#endif */
		g_io_channel_set_encoding (vis->socketio, NULL, NULL);
		g_io_channel_set_buffered (vis->socketio, FALSE);
		g_io_add_watch (vis->socketio, G_IO_IN, (GIOFunc) udpwatcher, vis);
	}

	/* set up client structure */
	x_fetch_client (id);
	c->type = VIS_UDP;
	memset (&c->transport.udp.addr, 0, sizeof (c->transport.udp.addr));
	c->transport.udp.socket[0] = 0;
	x_release_client ();

	xmms_log_info ("Visualization client %d initialised using UDP", id);
	return port;
}

void
cleanup_udp (xmmsc_vis_udp_t *t, xmms_socket_t socket)
{
	socklen_t sl = sizeof (t->addr);
	char packet = 'K';
	sendto (socket, &packet, 1, 0, (struct sockaddr *)&t->addr, sl);
}

gboolean
write_udp (xmmsc_vis_udp_t *t, xmms_vis_client_t *c, int32_t id, struct timeval *time, int channels, int size, short *buf, int socket)
{
	xmmsc_vis_udp_data_t packet_d;
	xmmsc_vischunk_t *__unaligned_dest;
	short res;
	int offset;
	char* packet;

	/* first check if the client is still there */
	if (t->grace == 0) {
		delete_client (id);
		return FALSE;
	}
	if (t->socket == 0) {
		return FALSE;
	}

	packet = packet_init_data (&packet_d);
	t->grace--;
	XMMSC_VIS_UNALIGNED_WRITE (packet_d.__unaligned_grace, htons (t->grace), uint16_t);
	__unaligned_dest = packet_d.__unaligned_data;

	XMMSC_VIS_UNALIGNED_WRITE (&__unaligned_dest->timestamp[0],
	                           (int32_t)htonl (time->tv_sec), int32_t);
	XMMSC_VIS_UNALIGNED_WRITE (&__unaligned_dest->timestamp[1],
	                           (int32_t)htonl (time->tv_usec), int32_t);


	XMMSC_VIS_UNALIGNED_WRITE (&__unaligned_dest->format, (uint16_t)htons (c->format), uint16_t);
	res = fill_buffer (__unaligned_dest->data, &c->prop, channels, size, buf);
	XMMSC_VIS_UNALIGNED_WRITE (&__unaligned_dest->size, (uint16_t)htons (res), uint16_t);

	offset = ((char*)&__unaligned_dest->data - (char*)__unaligned_dest);

	sendto (socket, packet, XMMS_VISPACKET_UDP_OFFSET + offset + res * sizeof (int16_t), 0, (struct sockaddr *)&t->addr, sizeof (t->addr));
	free (packet);


	return TRUE;
}
