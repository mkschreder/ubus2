/*
 * Copyright (C) 2011-2014 Felix Fietkau <nbd@openwrt.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2.1
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#ifdef FreeBSD
#include <sys/param.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#include <blobpack/blobpack.h>
#include <libusys/uloop.h>
#include <libusys/usock.h>
#include <libutype/list.h>

#include "ubusd.h"

static struct uloop uloop; 

static struct ubusd_msg_buf *ubusd_msg_ref(struct ubusd_msg_buf *ub)
{
	if (ub->refcount == ~0)
		return ubusd_msg_new(ub->data, ub->len, false);

	ub->refcount++;
	return ub;
}

struct ubusd_msg_buf *ubusd_msg_new(void *data, int len, bool shared)
{
	struct ubusd_msg_buf *ub;
	int buflen = sizeof(*ub);

	if (!shared)
		buflen += len;

	ub = calloc(1, buflen);
	if (!ub)
		return NULL;

	ub->fd = -1;

	if (shared) {
		ub->refcount = ~0;
		ub->data = data;
	} else {
		ub->refcount = 1;
		ub->data = (void *) (ub + 1);
		if (data)
			memcpy(ub + 1, data, len);
	}

	ub->len = len;
	return ub;
}

void ubusd_msg_free(struct ubusd_msg_buf *ub)
{
	switch (ub->refcount) {
	case 1:
	case ~0:
		if (ub->fd >= 0)
			close(ub->fd);

		free(ub);
		break;
	default:
		ub->refcount--;
		break;
	}
}

static int ubusd_msg_writev(int fd, struct ubusd_msg_buf *ub, int offset)
{
	static struct iovec iov[2];
	static struct {
		struct cmsghdr h;
		int fd;
	} fd_buf = {
		.h = {
			.cmsg_len = sizeof(fd_buf),
			.cmsg_level = SOL_SOCKET,
			.cmsg_type = SCM_RIGHTS,
		},
	};
	struct msghdr msghdr = {
		.msg_iov = iov,
		.msg_iovlen = ARRAY_SIZE(iov),
		.msg_control = &fd_buf,
		.msg_controllen = sizeof(fd_buf),
	};

	fd_buf.fd = ub->fd;
	if (ub->fd < 0) {
		msghdr.msg_control = NULL;
		msghdr.msg_controllen = 0;
	}

	if (offset < sizeof(ub->hdr)) {
		iov[0].iov_base = ((char *) &ub->hdr) + offset;
		iov[0].iov_len = sizeof(ub->hdr) - offset;
		iov[1].iov_base = (char *) ub->data;
		iov[1].iov_len = ub->len;

		return sendmsg(fd, &msghdr, 0);
	} else {
		offset -= sizeof(ub->hdr);
		return write(fd, ((char *) ub->data) + offset, ub->len - offset);
	}
}

static void ubusd_msg_enqueue(struct ubusd_client *cl, struct ubusd_msg_buf *ub)
{
	if (cl->tx_queue[cl->txq_tail])
		return;

	cl->tx_queue[cl->txq_tail] = ubusd_msg_ref(ub);
	cl->txq_tail = (cl->txq_tail + 1) % ARRAY_SIZE(cl->tx_queue);
}

/* takes the msgbuf reference */
void ubusd_msg_send(struct ubusd_client *cl, struct ubusd_msg_buf *ub, bool free)
{
	int written;

	if (!cl->tx_queue[cl->txq_cur]) {
		written = ubusd_msg_writev(cl->sock.fd, ub, 0);
		if (written >= ub->len + sizeof(ub->hdr))
			goto out;

		if (written < 0)
			written = 0;

		cl->txq_ofs = written;

		/* get an event once we can write to the socket again */
		uloop_add_fd(&uloop, &cl->sock, ULOOP_READ | ULOOP_WRITE | ULOOP_EDGE_TRIGGER);
	}
	ubusd_msg_enqueue(cl, ub);

out:
	if (free)
		ubusd_msg_free(ub);
}

static struct ubusd_msg_buf *ubusd_msg_head(struct ubusd_client *cl)
{
	return cl->tx_queue[cl->txq_cur];
}

static void ubusd_msg_dequeue(struct ubusd_client *cl)
{
	struct ubusd_msg_buf *ub = ubusd_msg_head(cl);

	if (!ub)
		return;

	ubusd_msg_free(ub);
	cl->txq_ofs = 0;
	cl->tx_queue[cl->txq_cur] = NULL;
	cl->txq_cur = (cl->txq_cur + 1) % ARRAY_SIZE(cl->tx_queue);
}

static void handle_client_disconnect(struct ubusd_client *cl)
{
	while (ubusd_msg_head(cl))
		ubusd_msg_dequeue(cl);

	ubusd_proto_free_client(cl);
	if (cl->pending_msg_fd >= 0)
		close(cl->pending_msg_fd);
	uloop_remove_fd(&uloop, &cl->sock);
	close(cl->sock.fd);
	free(cl);
}

static void client_cb(struct uloop_fd *sock, unsigned int events)
{
	struct ubusd_client *cl = container_of(sock, struct ubusd_client, sock);
	struct ubusd_msg_buf *ub;
	static struct iovec iov;
	static struct {
		struct cmsghdr h;
		int fd;
	} fd_buf = {
		.h = {
			.cmsg_type = SCM_RIGHTS,
			.cmsg_level = SOL_SOCKET,
			.cmsg_len = sizeof(fd_buf),
		}
	};
	struct msghdr msghdr = {
		.msg_iov = &iov,
		.msg_iovlen = 1,
	};

	/* first try to tx more pending data */
	while ((ub = ubusd_msg_head(cl))) {
		int written;

		written = ubusd_msg_writev(sock->fd, ub, cl->txq_ofs);
		if (written < 0) {
			switch(errno) {
			case EINTR:
			case EAGAIN:
				break;
			default:
				goto disconnect;
			}
			break;
		}

		cl->txq_ofs += written;
		if (cl->txq_ofs < ub->len + sizeof(ub->hdr))
			break;

		ubusd_msg_dequeue(cl);
	}

	/* prevent further ULOOP_WRITE events if we don't have data
	 * to send anymore */
	if (!ubusd_msg_head(cl) && (events & ULOOP_WRITE))
		uloop_add_fd(&uloop, sock, ULOOP_READ | ULOOP_EDGE_TRIGGER);

retry:
	if (!sock->eof && cl->pending_msg_offset < sizeof(cl->hdrbuf)) {
		int offset = cl->pending_msg_offset;
		int bytes;

		fd_buf.fd = -1;

		iov.iov_base = &cl->hdrbuf + offset;
		iov.iov_len = sizeof(cl->hdrbuf) - offset;

		if (cl->pending_msg_fd < 0) {
			msghdr.msg_control = &fd_buf;
			msghdr.msg_controllen = sizeof(fd_buf);
		} else {
			msghdr.msg_control = NULL;
			msghdr.msg_controllen = 0;
		}

		bytes = recvmsg(sock->fd, &msghdr, 0);
		if (bytes < 0)
			goto out;

		if (fd_buf.fd >= 0)
			cl->pending_msg_fd = fd_buf.fd;

		cl->pending_msg_offset += bytes;
		if (cl->pending_msg_offset < sizeof(cl->hdrbuf))
			goto out;

		if (blob_attr_pad_len(&cl->hdrbuf.data) > UBUS_MAX_MSGLEN)
			goto disconnect;

		cl->pending_msg = ubusd_msg_new(NULL, blob_attr_raw_len(&cl->hdrbuf.data), false);
		if (!cl->pending_msg)
			goto disconnect;

		memcpy(&cl->pending_msg->hdr, &cl->hdrbuf.hdr, sizeof(cl->hdrbuf.hdr));
		memcpy(cl->pending_msg->data, &cl->hdrbuf.data, sizeof(cl->hdrbuf.data));
	}

	ub = cl->pending_msg;
	if (ub) {
		int offset = cl->pending_msg_offset - sizeof(ub->hdr);
		int len = blob_attr_raw_len(ub->data) - offset;
		int bytes = 0;

		if (len > 0) {
			bytes = read(sock->fd, (char *) ub->data + offset, len);
			if (bytes <= 0)
				goto out;
		}

		if (bytes < len) {
			cl->pending_msg_offset += bytes;
			goto out;
		}

		/* accept message */
		ub->fd = cl->pending_msg_fd;
		cl->pending_msg_fd = -1;
		cl->pending_msg_offset = 0;
		cl->pending_msg = NULL;
		ubusd_proto_receive_message(cl, ub);
		goto retry;
	}

out:
	if (!sock->eof || ubusd_msg_head(cl))
		return;

disconnect:
	handle_client_disconnect(cl);
}

static bool get_next_connection(int fd)
{
	struct ubusd_client *cl;
	int client_fd;

	client_fd = accept(fd, NULL, 0);
	if (client_fd < 0) {
		switch (errno) {
		case ECONNABORTED:
		case EINTR:
			return true;
		default:
			return false;
		}
	}

	cl = ubusd_proto_new_client(client_fd, client_cb);
	if (cl)
		uloop_add_fd(&uloop, &cl->sock, ULOOP_READ | ULOOP_EDGE_TRIGGER);
	else
		close(client_fd);

	return true;
}

static void server_cb(struct uloop_fd *fd, unsigned int events)
{
	bool next;

	do {
		next = get_next_connection(fd->fd);
	} while (next);
}

static struct uloop_fd server_fd = {
	.cb = server_cb,
};

static int usage(const char *progname)
{
	fprintf(stderr, "Usage: %s [<options>]\n"
		"Options: \n"
		"  -s <socket>:		Set the unix domain socket to listen on\n"
		"\n", progname);
	return 1;
}

void ubusd_obj_init(); 
void ubusd_proto_init(); 

int main(int argc, char **argv)
{
	const char *ubusd_socket = UBUS_UNIX_SOCKET;
	int ret = 0;
	int ch;

	signal(SIGPIPE, SIG_IGN);

	printf("initializing uloop\n"); 

	ubusd_obj_init(); 
	ubusd_proto_init(); 

	uloop_init(&uloop);

	while ((ch = getopt(argc, argv, "s:")) != -1) {
		switch (ch) {
		case 's':
			ubusd_socket = optarg;
			break;
		default:
			return usage(argv[0]);
		}
	}

	printf("preparing ubus sockets\n"); 

	unlink(ubusd_socket);
	umask(0177);
	server_fd.fd = usock(USOCK_UNIX | USOCK_SERVER | USOCK_NONBLOCK, ubusd_socket, NULL);
	if (server_fd.fd < 0) {
		perror("usock");
		ret = -1;
		goto out;
	}
	uloop_add_fd(&uloop, &server_fd, ULOOP_READ | ULOOP_EDGE_TRIGGER);

	uloop_run(&uloop);
	unlink(ubusd_socket);

out:
	uloop_destroy(&uloop);
	return ret;
}
