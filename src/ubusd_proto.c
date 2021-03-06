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

#include <arpa/inet.h>
#include <unistd.h>

#include "ubusd.h"

static struct ubusd_msg_buf *retmsg;
static int *retmsg_data;
static struct avl_tree clients;

typedef int (*ubusd_cmd_cb)(struct ubusd_client *cl, struct ubusd_msg_buf *ub, struct blob_attr **attr);

static const struct blob_attr_policy ubusd_policy[UBUS_ATTR_MAX] = {
	[UBUS_ATTR_SIGNATURE] = { .type = BLOB_ATTR_ARRAY },
	[UBUS_ATTR_OBJTYPE] = { .type = BLOB_ATTR_INT32 },
	[UBUS_ATTR_OBJPATH] = { .type = BLOB_ATTR_STRING },
	[UBUS_ATTR_OBJID] = { .type = BLOB_ATTR_INT32 },
	[UBUS_ATTR_STATUS] = { .type = BLOB_ATTR_INT32 },
	[UBUS_ATTR_METHOD] = { .type = BLOB_ATTR_STRING },
};

static void ubusd_msg_close_fd(struct ubusd_msg_buf *ub)
{
	if (ub->fd < 0)
		return;

	close(ub->fd);
	ub->fd = -1;
}

static void ubusd_msg_init(struct ubusd_msg_buf *ub, uint8_t type, uint16_t seq, uint32_t peer)
{
	ub->hdr.version = 0;
	ub->hdr.type = type;
	ub->hdr.seq = seq;
	ub->hdr.peer = peer;
}

static struct ubusd_msg_buf *ubusd_msg_from_blob(bool shared)
{
	return ubusd_msg_new(blob_buf_head(&b), blob_buf_size(&b), shared);
}

static struct ubusd_msg_buf *ubusd_reply_from_blob(struct ubusd_msg_buf *ub, bool shared)
{
	struct ubusd_msg_buf *new;

	new = ubusd_msg_from_blob(shared);
	if (!new)
		return NULL;

	ubusd_msg_init(new, UBUS_MSG_DATA, ub->hdr.seq, ub->hdr.peer);
	return new;
}

static void
ubusd_send_msg_from_blob(struct ubusd_client *cl, struct ubusd_msg_buf *ub,
			uint8_t type)
{
	ub = ubusd_reply_from_blob(ub, true);
	if (!ub)
		return;

	ub->hdr.type = type;
	ubusd_msg_send(cl, ub, true);
}

static bool ubusd_send_hello(struct ubusd_client *cl)
{
	struct ubusd_msg_buf *ub;

	blob_buf_reset(&b);
	ub = ubusd_msg_from_blob(true);
	if (!ub)
		return false;

	ubusd_msg_init(ub, UBUS_MSG_HELLO, 0, cl->id.id);
	ubusd_msg_send(cl, ub, true);
	return true;
}

static int ubusd_send_pong(struct ubusd_client *cl, struct ubusd_msg_buf *ub, struct blob_attr **attr)
{
	ub->hdr.type = UBUS_MSG_DATA;
	ubusd_msg_send(cl, ub, false);
	return 0;
}

static int ubusd_handle_remove_object(struct ubusd_client *cl, struct ubusd_msg_buf *ub, struct blob_attr **attr)
{
	struct ubusd_object *obj;

	if (!attr[UBUS_ATTR_OBJID])
		return UBUS_STATUS_INVALID_ARGUMENT;

	obj = ubusd_find_object(blob_attr_get_u32(attr[UBUS_ATTR_OBJID]));
	if (!obj)
		return UBUS_STATUS_NOT_FOUND;

	if (obj->client != cl)
		return UBUS_STATUS_PERMISSION_DENIED;

	blob_buf_reset(&b);
	blob_buf_put_i32(&b, obj->id.id);

	/* check if we're removing the object type as well */
	if (obj->type && obj->type->refcount == 1)
		blob_buf_put_i32(&b, obj->type->id.id);

	ubusd_free_object(obj);
	ubusd_send_msg_from_blob(cl, ub, UBUS_MSG_DATA);

	return 0;
}

static int ubusd_handle_add_object(struct ubusd_client *cl, struct ubusd_msg_buf *ub, struct blob_attr **attr)
{
	struct ubusd_object *obj;

	obj = ubusd_create_object(cl, attr);
	if (!obj)
		return UBUS_STATUS_INVALID_ARGUMENT;

	blob_buf_reset(&b);
	blob_buf_put_i32(&b, obj->id.id);
	if (attr[UBUS_ATTR_SIGNATURE])
		blob_buf_put_i32(&b, obj->type->id.id);

	ubusd_send_msg_from_blob(cl, ub, UBUS_MSG_DATA);
	return 0;
}

static void ubusd_send_obj(struct ubusd_client *cl, struct ubusd_msg_buf *ub, struct ubusd_object *obj)
{
	struct ubusd_method *m;
	void *s;

	blob_buf_reset(&b);

	blob_offset_t tbl = blob_buf_open_table(&b); 
		blob_buf_put_string(&b, "id"); 
		blob_buf_put_i32(&b, obj->id.id);
		blob_buf_put_string(&b, "client"); 
		blob_buf_put_i32(&b, obj->client->id.id); 

		if (obj->path.key) {
			blob_buf_put_string(&b, "path"); 
			blob_buf_put_string(&b, obj->path.key);
		}
		blob_buf_put_string(&b, "type"); 
		blob_buf_put_i32(&b, obj->type->id.id);
		
		blob_buf_put_string(&b, "methods"); 
		s = blob_buf_open_table(&b);
		list_for_each_entry(m, &obj->type->methods, list)
			blob_buf_put_attr(&b, m->data);
		blob_buf_close_table(&b, s);
	blob_buf_close_table(&b, tbl); 

	ubusd_send_msg_from_blob(cl, ub, UBUS_MSG_DATA);
}

static int ubusd_handle_lookup(struct ubusd_client *cl, struct ubusd_msg_buf *ub, struct blob_attr **attr)
{
	struct ubusd_object *obj;
	char *objpath;
	bool found = false;
	int len;

	if (!attr[UBUS_ATTR_OBJPATH]) {
		avl_for_each_element(&path, obj, path)
			ubusd_send_obj(cl, ub, obj);
		return 0;
	}

	objpath = blob_attr_data(attr[UBUS_ATTR_OBJPATH]);
	len = strlen(objpath);
	if (objpath[len - 1] != '*') {
		obj = avl_find_element(&path, objpath, obj, path);
		if (!obj)
			return UBUS_STATUS_NOT_FOUND;

		ubusd_send_obj(cl, ub, obj);
		return 0;
	}

	objpath[--len] = 0;

	obj = avl_find_ge_element(&path, objpath, obj, path);
	if (!obj)
		return UBUS_STATUS_NOT_FOUND;

	while (!strncmp(objpath, obj->path.key, len)) {
		found = true;
		ubusd_send_obj(cl, ub, obj);
		if (obj == avl_last_element(&path, obj, path))
			break;
		obj = avl_next_element(obj, path);
	}

	if (!found)
		return UBUS_STATUS_NOT_FOUND;

	return 0;
}

static void
ubusd_forward_invoke(struct ubusd_object *obj, const char *method,
		     struct ubusd_msg_buf *ub, struct blob_attr *data)
{
	blob_buf_put_i32(&b, obj->id.id);
	blob_buf_put_string(&b, method);
	if (data)
		blob_buf_put_attr(&b, data);

	ubusd_send_msg_from_blob(obj->client, ub, UBUS_MSG_INVOKE);
}

static int ubusd_handle_invoke(struct ubusd_client *cl, struct ubusd_msg_buf *ub, struct blob_attr **attr)
{
	struct ubusd_object *obj = NULL;
	struct ubusd_id *id;
	const char *method;

	if (!attr[UBUS_ATTR_METHOD] || !attr[UBUS_ATTR_OBJID])
		return UBUS_STATUS_INVALID_ARGUMENT;

	id = ubusd_find_id(&objects, blob_attr_get_u32(attr[UBUS_ATTR_OBJID]));
	if (!id)
		return UBUS_STATUS_NOT_FOUND;

	obj = container_of(id, struct ubusd_object, id);

	method = blob_attr_data(attr[UBUS_ATTR_METHOD]);

	if (!obj->client)
		return obj->recv_msg(cl, method, attr[UBUS_ATTR_DATA]);

	ub->hdr.peer = cl->id.id;
	blob_buf_reset(&b);
	ubusd_forward_invoke(obj, method, ub, attr[UBUS_ATTR_DATA]);
	ubusd_msg_free(ub);

	return -1;
}

static int ubusd_handle_notify(struct ubusd_client *cl, struct ubusd_msg_buf *ub, struct blob_attr **attr)
{
	struct ubusd_object *obj = NULL;
	struct ubusd_subscription *s;
	struct ubusd_id *id;
	const char *method;
	bool no_reply = false;
	void *c;

	if (!attr[UBUS_ATTR_METHOD] || !attr[UBUS_ATTR_OBJID])
		return UBUS_STATUS_INVALID_ARGUMENT;

	if (attr[UBUS_ATTR_NO_REPLY])
		no_reply = blob_attr_get_i8(attr[UBUS_ATTR_NO_REPLY]);

	id = ubusd_find_id(&objects, blob_attr_get_u32(attr[UBUS_ATTR_OBJID]));
	if (!id)
		return UBUS_STATUS_NOT_FOUND;

	obj = container_of(id, struct ubusd_object, id);
	if (obj->client != cl)
		return UBUS_STATUS_PERMISSION_DENIED;

	if (!no_reply) {
		blob_buf_reset(&b);
		blob_buf_put_i32(&b, id->id);
		c = blob_buf_open_array(&b);
		list_for_each_entry(s, &obj->subscribers, list) {
			blob_buf_put_i32(&b, s->subscriber->id.id);
		}
		blob_buf_close_array(&b, c);
		blob_buf_put_i32(&b, 0);
		ubusd_send_msg_from_blob(cl, ub, UBUS_MSG_STATUS);
	}

	ub->hdr.peer = cl->id.id;
	method = blob_attr_data(attr[UBUS_ATTR_METHOD]);
	list_for_each_entry(s, &obj->subscribers, list) {
		blob_buf_reset(&b);
		if (no_reply)
			blob_buf_put_i8(&b, 1);
		ubusd_forward_invoke(s->subscriber, method, ub, attr[UBUS_ATTR_DATA]);
	}
	ubusd_msg_free(ub);

	return -1;
}

static struct ubusd_client *ubusd_get_client_by_id(uint32_t id)
{
	struct ubusd_id *clid;

	clid = ubusd_find_id(&clients, id);
	if (!clid)
		return NULL;

	return container_of(clid, struct ubusd_client, id);
}

static int ubusd_handle_response(struct ubusd_client *cl, struct ubusd_msg_buf *ub, struct blob_attr **attr)
{
	struct ubusd_object *obj;

	if (!attr[UBUS_ATTR_OBJID] ||
	    (ub->hdr.type == UBUS_MSG_STATUS && !attr[UBUS_ATTR_STATUS]) ||
	    (ub->hdr.type == UBUS_MSG_DATA && !attr[UBUS_ATTR_DATA]))
		goto error;

	obj = ubusd_find_object(blob_attr_get_u32(attr[UBUS_ATTR_OBJID]));
	if (!obj)
		goto error;

	if (cl != obj->client)
		goto error;

	cl = ubusd_get_client_by_id(ub->hdr.peer);
	if (!cl)
		goto error;

	ub->hdr.peer = blob_attr_get_u32(attr[UBUS_ATTR_OBJID]);
	ubusd_msg_send(cl, ub, true);
	return -1;

error:
	ubusd_msg_free(ub);
	return -1;
}

static int ubusd_handle_add_watch(struct ubusd_client *cl, struct ubusd_msg_buf *ub, struct blob_attr **attr)
{
	struct ubusd_object *obj, *target;

	if (!attr[UBUS_ATTR_OBJID] || !attr[UBUS_ATTR_TARGET])
		return UBUS_STATUS_INVALID_ARGUMENT;

	obj = ubusd_find_object(blob_attr_get_u32(attr[UBUS_ATTR_OBJID]));
	if (!obj)
		return UBUS_STATUS_NOT_FOUND;

	if (cl != obj->client)
		return UBUS_STATUS_INVALID_ARGUMENT;

	target = ubusd_find_object(blob_attr_get_u32(attr[UBUS_ATTR_TARGET]));
	if (!target)
		return UBUS_STATUS_NOT_FOUND;

	if (cl == target->client)
		return UBUS_STATUS_INVALID_ARGUMENT;

	ubusd_subscribe(obj, target);
	return 0;
}

static int ubusd_handle_remove_watch(struct ubusd_client *cl, struct ubusd_msg_buf *ub, struct blob_attr **attr)
{
	struct ubusd_object *obj;
	struct ubusd_subscription *s;
	uint32_t id;

	if (!attr[UBUS_ATTR_OBJID] || !attr[UBUS_ATTR_TARGET])
		return UBUS_STATUS_INVALID_ARGUMENT;

	obj = ubusd_find_object(blob_attr_get_u32(attr[UBUS_ATTR_OBJID]));
	if (!obj)
		return UBUS_STATUS_NOT_FOUND;

	if (cl != obj->client)
		return UBUS_STATUS_INVALID_ARGUMENT;

	id = blob_attr_get_u32(attr[UBUS_ATTR_TARGET]);
	list_for_each_entry(s, &obj->target_list, target_list) {
		if (s->target->id.id != id)
			continue;

		ubusd_unsubscribe(s);
		return 0;
	}

	return UBUS_STATUS_NOT_FOUND;
}

static const ubusd_cmd_cb handlers[__UBUS_MSG_LAST] = {
	[UBUS_MSG_PING] = ubusd_send_pong,
	[UBUS_MSG_ADD_OBJECT] = ubusd_handle_add_object,
	[UBUS_MSG_REMOVE_OBJECT] = ubusd_handle_remove_object,
	[UBUS_MSG_LOOKUP] = ubusd_handle_lookup,
	[UBUS_MSG_INVOKE] = ubusd_handle_invoke,
	[UBUS_MSG_STATUS] = ubusd_handle_response,
	[UBUS_MSG_DATA] = ubusd_handle_response,
	[UBUS_MSG_SUBSCRIBE] = ubusd_handle_add_watch,
	[UBUS_MSG_UNSUBSCRIBE] = ubusd_handle_remove_watch,
	[UBUS_MSG_NOTIFY] = ubusd_handle_notify,
};

void ubusd_proto_receive_message(struct ubusd_client *cl, struct ubusd_msg_buf *ub)
{
	ubusd_cmd_cb cb = NULL;
	int ret;

	retmsg->hdr.seq = ub->hdr.seq;
	retmsg->hdr.peer = ub->hdr.peer;

	printf("IN %s seq=%d peer=%08x: ", ubus_message_types[ub->hdr.type], ub->hdr.seq, ub->hdr.peer);
	blob_attr_dump_json(ub->data); 

	if (ub->hdr.type < __UBUS_MSG_LAST)
		cb = handlers[ub->hdr.type];

	if (ub->hdr.type != UBUS_MSG_STATUS)
		ubusd_msg_close_fd(ub);

	struct blob_attr *attrbuf[UBUS_ATTR_MAX]; 

	
	ubus_message_parse(ub->hdr.type, ub->data, attrbuf); 

	if (cb)
		ret = cb(cl, ub, attrbuf);
	else
		ret = UBUS_STATUS_INVALID_COMMAND;

	if (ret == -1)
		return;

	ubusd_msg_free(ub);

	*retmsg_data = htonl(ret);
	ubusd_msg_send(cl, retmsg, false);
}

struct ubusd_client *ubusd_proto_new_client(int fd)
{
	struct ubusd_client *cl = ubusd_client_new(fd); 
	
	if (!cl)
		return NULL;

	if (!ubusd_alloc_id(&clients, &cl->id, 0))
		goto free;

	if (!ubusd_send_hello(cl))
		goto delete;

	return cl;

delete:
	ubusd_free_id(&clients, &cl->id);
free:
	ubusd_client_delete(&cl);
	return NULL;
}

void ubusd_proto_free_client(struct ubusd_client *cl)
{
	struct ubusd_object *obj;

	while (!list_empty(&cl->objects)) {
		obj = list_first_entry(&cl->objects, struct ubusd_object, list);
		ubusd_free_object(obj);
	}

	ubusd_free_id(&clients, &cl->id);
}

void ubusd_notify_subscription(struct ubusd_object *obj)
{
	bool active = !list_empty(&obj->subscribers);
	struct ubusd_msg_buf *ub;

	blob_buf_reset(&b);
	blob_buf_put_i32(&b, obj->id.id);
	blob_buf_put_i8(&b, active);

	ub = ubusd_msg_from_blob(false);
	if (!ub)
		return;

	ubusd_msg_init(ub, UBUS_MSG_NOTIFY, ++obj->invoke_seq, 0);
	ubusd_msg_send(obj->client, ub, true);
}

void ubusd_notify_unsubscribe(struct ubusd_subscription *s)
{
	struct ubusd_msg_buf *ub;

	blob_buf_reset(&b);
	blob_buf_put_i32(&b, s->subscriber->id.id);
	blob_buf_put_i32(&b, s->target->id.id);

	ub = ubusd_msg_from_blob(false);
	if (ub != NULL) {
		ubusd_msg_init(ub, UBUS_MSG_UNSUBSCRIBE, ++s->subscriber->invoke_seq, 0);
		ubusd_msg_send(s->subscriber->client, ub, true);
	}

	ubusd_unsubscribe(s);
}

void ubusd_proto_init(void)
{
	ubusd_init_id_tree(&clients);

	blob_buf_reset(&b);
	blob_buf_put_i32(&b, 0);

	retmsg = ubusd_msg_from_blob(false);
	if (!retmsg)
		exit(1);

	retmsg->hdr.type = UBUS_MSG_STATUS;
	retmsg_data = blob_attr_data(blob_attr_data(retmsg->data));
}
