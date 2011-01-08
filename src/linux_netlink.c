/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2010 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU Lesser General Public License Version 2.1
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <glib.h>
#include <gio/gio.h>
//#include <gio/gsocket.h>
//#include <gio/gunixsocketaddress.h>
//#include <linux/netlink.h>
#include <errno.h>
#include <math.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/connector.h>
#include <linux/cn_proc.h>
#include <string.h>
#include <stdlib.h>
#include "ulatency.h"

#define SEND_MESSAGE_LEN (NLMSG_LENGTH(sizeof(struct cn_msg) + \
	sizeof(enum proc_cn_mcast_op)))
#define RECV_MESSAGE_LEN (NLMSG_LENGTH(sizeof(struct cn_msg) + \
	sizeof(struct proc_event)))

#define SEND_MESSAGE_SIZE (NLMSG_SPACE(SEND_MESSAGE_LEN))
#define RECV_MESSAGE_SIZE (NLMSG_SPACE(RECV_MESSAGE_LEN))

#define BUFF_SIZE (MAX(MAX(SEND_MESSAGE_SIZE, RECV_MESSAGE_SIZE), 1024))
#define MIN_RECV_SIZE (MIN(SEND_MESSAGE_SIZE, RECV_MESSAGE_SIZE))



/**
 * Handle a netlink message.  In the event of PROC_EVENT_UID or PROC_EVENT_GID,
 * we pass the event along to cgre_process_event for further processing.  All
 * other events are ignored.
 * 	@param cn_hdr The netlink message
 * 	@return 0 on success, > 0 on error
 */
static int nl_handle_msg(struct cn_msg *cn_hdr)
{
	/* The event to consider */
	struct proc_event *ev;

	/* Return codes */
	int ret = 0;

	/* Get the event data.  We only care about two event types. */
	ev = (struct proc_event*)cn_hdr->data;
	switch (ev->what) {
	case PROC_EVENT_UID:
		g_debug("UID Event: PID = %d, tGID = %d, rUID = %d,"
				" eUID = %d", ev->event_data.id.process_pid,
				ev->event_data.id.process_tgid,
				ev->event_data.id.r.ruid,
				ev->event_data.id.e.euid);
		//process_update_pid(ev->event_data.id.process_pid);
		process_new(ev->event_data.id.process_pid);
		break;
	case PROC_EVENT_GID:
		g_debug("GID Event: PID = %d, tGID = %d, rGID = %d,"
				" eGID = %d", ev->event_data.id.process_pid,
				ev->event_data.id.process_tgid,
				ev->event_data.id.r.rgid,
				ev->event_data.id.e.egid);
		//process_update_pid(ev->event_data.id.process_pid);
		process_new(ev->event_data.id.process_pid);
		break;
	case PROC_EVENT_FORK:
		g_debug("FORK Event: PARENT = %d PID = %d",
			ev->event_data.fork.parent_pid, ev->event_data.fork.child_pid);
		process_new(ev->event_data.fork.child_pid);
		break;
	case PROC_EVENT_EXIT:
		g_debug("EXIT Event: PID = %d",ev->event_data.exit.process_pid);
		process_remove_by_pid(ev->event_data.exit.process_pid);
		break;
	case PROC_EVENT_EXEC:
		g_debug("EXEC Event: PID = %d, tGID = %d",
				ev->event_data.exec.process_pid,
				ev->event_data.exec.process_tgid);
		process_new(ev->event_data.exec.process_pid);
		break;
	default:
		break;
	}

	return ret;
}


static gboolean
nl_connection_handler (GSocket *socket, GIOCondition condition, gpointer user_data)
{
	GError *error = NULL;
	gsize len;
	gchar buffer[1024];
	gboolean ret = TRUE;
	GMainLoop *loop = (GMainLoop *) user_data;
	char buff[BUFF_SIZE];
	size_t recv_len;
	struct sockaddr_nl from_nla;
	socklen_t from_nla_len;
	struct nlmsghdr *nlh;
	struct sockaddr_nl kern_nla;
	struct cn_msg *cn_hdr;
	int socket_fd;

	kern_nla.nl_family = AF_NETLINK;
	kern_nla.nl_groups = CN_IDX_PROC;
	kern_nla.nl_pid = 1;
	kern_nla.nl_pad = 0;

	memset(buff, 0, sizeof(buff));
	from_nla_len = sizeof(from_nla);
	memcpy(&from_nla, &kern_nla, sizeof(from_nla));

	/* the helper process exited */
	if ((condition & G_IO_HUP) > 0) {
		g_warning ("socket was disconnected");
		g_main_loop_quit (loop);
		ret = FALSE;
		goto out;
	}

	/* there is data */
	if ((condition & G_IO_IN) > 0) {
		//len = g_socket_receive (socket, buffer, 1024, NULL, &error);

		len = g_socket_receive (socket, buff, sizeof(buff), NULL, &error);
	//recv_len = recvfrom(sk_nl, buff, sizeof(buff), 0,
	//	(struct sockaddr *)&from_nla, &from_nla_len);

		if (error != NULL) {
			g_warning ("failed to get data: %s", error->message);
			g_error_free (error);
			ret = FALSE;
			goto out;
		}
		if (len == ENOBUFS) {
			g_warning("ERROR: NETLINK BUFFER FULL, MESSAGE DROPPED!");
			return 0;
		}
		if (len == 0)
			goto out;
		nlh = (struct nlmsghdr *)buff;
		while (NLMSG_OK(nlh, len)) {
			cn_hdr = NLMSG_DATA(nlh);
			if (nlh->nlmsg_type == NLMSG_NOOP) {
				nlh = NLMSG_NEXT(nlh, recv_len);
				continue;
			}
			if ((nlh->nlmsg_type == NLMSG_ERROR) ||
					(nlh->nlmsg_type == NLMSG_OVERRUN))
				break;
			if (nl_handle_msg(cn_hdr) < 0)
				return 1;
			if (nlh->nlmsg_type == NLMSG_DONE)
				break;
			nlh = NLMSG_NEXT(nlh, recv_len);
		}
	}
out:
	return ret;
}


/*
static int cgre_receive_netlink_msg(int sk_nl)
{
	char buff[BUFF_SIZE];
	size_t recv_len;
	struct sockaddr_nl from_nla;
	socklen_t from_nla_len;
	struct nlmsghdr *nlh;
	struct sockaddr_nl kern_nla;
	struct cn_msg *cn_hdr;

	kern_nla.nl_family = AF_NETLINK;
	kern_nla.nl_groups = CN_IDX_PROC;
	kern_nla.nl_pid = 1;
	kern_nla.nl_pad = 0;

	memset(buff, 0, sizeof(buff));
	from_nla_len = sizeof(from_nla);
	memcpy(&from_nla, &kern_nla, sizeof(from_nla));
	recv_len = recvfrom(sk_nl, buff, sizeof(buff), 0,
		(struct sockaddr *)&from_nla, &from_nla_len);
	if (recv_len == ENOBUFS) {
		g_warning("ERROR: NETLINK BUFFER FULL, MESSAGE DROPPED!");
		return 0;
	}
	if (recv_len < 1)
		return 0;

	nlh = (struct nlmsghdr *)buff;
	while (NLMSG_OK(nlh, recv_len)) {
		cn_hdr = NLMSG_DATA(nlh);
		if (nlh->nlmsg_type == NLMSG_NOOP) {
			nlh = NLMSG_NEXT(nlh, recv_len);
			continue;
		}
		if ((nlh->nlmsg_type == NLMSG_ERROR) ||
				(nlh->nlmsg_type == NLMSG_OVERRUN))
			break;
		if (nl_connection_handler(cn_hdr) < 0)
			return 1;
		if (nlh->nlmsg_type == NLMSG_DONE)
			break;
		nlh = NLMSG_NEXT(nlh, recv_len);
	}
	return 0;
}

*/

int init_netlink(GMainLoop *loop) {
	gboolean ret;
	GSocket *gsocket = NULL;
	int socket_fd = 0;
	GSocketAddress *address = NULL;
	GError *error = NULL;
	gsize wrote;
	GSource *source;
	struct sockaddr_nl my_nla;
	struct nlmsghdr *nl_hdr;
	char buff[BUFF_SIZE];
	struct cn_msg *cn_hdr;
	enum proc_cn_mcast_op *mcop_msg;

	g_type_init ();
	loop = g_main_loop_new (NULL, FALSE);

	/* create socket */
	/*
	 * Create an endpoint for communication. Use the kernel user
	 * interface device (PF_NETLINK) which is a datagram oriented
	 * service (SOCK_DGRAM). The protocol used is the connector
	 * protocol (NETLINK_CONNECTOR)
	 */
	socket_fd = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_CONNECTOR);
//	socket = g_socket_new (G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, &error);
//	socket = g_socket_new (PF_NETLINK, SOCK_DGRAM, NETLINK_CONNECTOR, &error);
	if (socket == NULL) {
		g_warning ("failed to create socket: %s", error->message);
		g_error_free (error);
		return 1;
	}

	my_nla.nl_family = AF_NETLINK;
	my_nla.nl_groups = CN_IDX_PROC;
	my_nla.nl_pid = getpid();
	my_nla.nl_pad = 0;

//	g_socket_set_blocking (socket, FALSE);
//	g_socket_set_keepalive (socket, TRUE);
//	socket_fd = g_socket_get_fd(socket);
	if (bind(socket_fd, (struct sockaddr *)&my_nla, sizeof(my_nla)) < 0) {
		g_warning("binding sk_nl error: %s\n", strerror(errno));
		goto out;
	}


	gsocket = g_socket_new_from_fd(socket_fd, NULL);
	if(gsocket == NULL) {
		g_warning("can't create socket");	
		goto out;
	}

	nl_hdr = (struct nlmsghdr *)buff;
	cn_hdr = (struct cn_msg *)NLMSG_DATA(nl_hdr);
	mcop_msg = (enum proc_cn_mcast_op*)&cn_hdr->data[0];
	g_debug("sending proc connector: PROC_CN_MCAST_LISTEN... ");
	memset(buff, 0, sizeof(buff));
	*mcop_msg = PROC_CN_MCAST_LISTEN;

	/* fill the netlink header */
	nl_hdr->nlmsg_len = SEND_MESSAGE_LEN;
	nl_hdr->nlmsg_type = NLMSG_DONE;
	nl_hdr->nlmsg_flags = 0;
	nl_hdr->nlmsg_seq = 0;
	nl_hdr->nlmsg_pid = getpid();

	/* fill the connector header */
	cn_hdr->id.idx = CN_IDX_PROC;
	cn_hdr->id.val = CN_VAL_PROC;
	cn_hdr->seq = 0;
	cn_hdr->ack = 0;
	cn_hdr->len = sizeof(enum proc_cn_mcast_op);
	g_debug("sending netlink message len=%d, cn_msg len=%d\n",
		nl_hdr->nlmsg_len, (int) sizeof(struct cn_msg));
	if (send(socket_fd, nl_hdr, nl_hdr->nlmsg_len, 0) != nl_hdr->nlmsg_len) {
		g_warning("failed to send proc connector mcast ctl op!: %s\n",
			strerror(errno));
	}
	g_debug("sent\n");

	/* connect to it */
	
/*	address = g_unix_socket_address_new_with_type (socket_filename, -1, G_UNIX_SOCKET_ADDRESS_PATH);
	ret = g_socket_connect (socket, address, NULL, &error);
	if (!ret) {
		g_warning ("failed to connect to socket: %s", error->message);
		g_error_free (error);
		goto out;
	}
*/
	/* socket has data */
	source = g_socket_create_source (gsocket, G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL, NULL);
	g_source_set_callback (source, (GSourceFunc) nl_connection_handler, loop, NULL);
	g_source_attach (source, NULL);

	return 0;
out:
	return 1;
	/* send some data */
/*	wrote = g_socket_send (socket, buffer, 5, NULL, &error);
	if (wrote != 5) {
		g_warning ("failed to write 5 bytes");
		goto out;
	}
*/

}

#if 0
gint
main (void)
{
	gboolean ret;
	GSocket *gsocket = NULL;
	int socket_fd = 0;
	GSocketAddress *address = NULL;
	GError *error = NULL;
	gsize wrote;
	GSource *source;
	GMainLoop *loop;
	struct sockaddr_nl my_nla;
	struct nlmsghdr *nl_hdr;
	char buff[BUFF_SIZE];
	struct cn_msg *cn_hdr;
	enum proc_cn_mcast_op *mcop_msg;

	g_type_init ();
	loop = g_main_loop_new (NULL, FALSE);

	init_netlink(loop);

	g_debug ("running main loop");
	g_main_loop_run (loop);
out:
	if (loop != NULL)
		g_main_loop_unref (loop);
	if (socket != NULL)
		g_object_unref (socket);
	if (address != NULL)
		g_object_unref (address);
	return 0;
}

#endif