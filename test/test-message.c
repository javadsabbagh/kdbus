#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <assert.h>
#include <time.h>
#include <stdbool.h>
#include <sys/eventfd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "kdbus-api.h"
#include "kdbus-util.h"
#include "kdbus-enum.h"
#include "kdbus-test.h"

/*
 * maximum number of queued messages wich will not be user accounted.
 * after this value is reached each user will have an individual limit.
 */
#define KDBUS_CONN_MAX_MSGS_UNACCOUNTED		16

/*
 * maximum number of queued messages from the same indvidual user after the
 * the un-accounted value has been hit
 */
#define KDBUS_CONN_MAX_MSGS_PER_USER		16

#define MAX_USER_TOTAL_MSGS  (KDBUS_CONN_MAX_MSGS_UNACCOUNTED + \
				KDBUS_CONN_MAX_MSGS_PER_USER)

/* maximum number of queued messages in a connection */
#define KDBUS_CONN_MAX_MSGS			256

/* maximum number of queued requests waiting for a reply */
#define KDBUS_CONN_MAX_REQUESTS_PENDING		128

/* maximum message payload size */
#define KDBUS_MSG_MAX_PAYLOAD_VEC_SIZE		(2 * 1024UL * 1024UL)

int kdbus_test_message_basic(struct kdbus_test_env *env)
{
	struct kdbus_conn *conn;
	struct kdbus_conn *sender;
	struct kdbus_msg *msg;
	uint64_t cookie = 0x1234abcd5678eeff;
	uint64_t offset;
	int ret;

	sender = kdbus_hello(env->buspath, 0, NULL, 0);
	ASSERT_RETURN(sender != NULL);

	/* create a 2nd connection */
	conn = kdbus_hello(env->buspath, 0, NULL, 0);
	ASSERT_RETURN(conn != NULL);

	ret = kdbus_add_match_empty(conn);
	ASSERT_RETURN(ret == 0);

	ret = kdbus_add_match_empty(sender);
	ASSERT_RETURN(ret == 0);

	/* send over 1st connection */
	ret = kdbus_msg_send(sender, NULL, cookie, 0, 0, 0,
			     KDBUS_DST_ID_BROADCAST);
	ASSERT_RETURN(ret == 0);

	/* Make sure that we do not get our own broadcasts */
	ret = kdbus_msg_recv(sender, NULL, NULL);
	ASSERT_RETURN(ret == -EAGAIN);

	/* ... and receive on the 2nd */
	ret = kdbus_msg_recv_poll(conn, 100, &msg, &offset);
	ASSERT_RETURN(ret == 0);
	ASSERT_RETURN(msg->cookie == cookie);

	kdbus_msg_free(msg);

	/* Msgs that expect a reply must have timeout and cookie */
	ret = kdbus_msg_send(sender, NULL, 0, KDBUS_MSG_EXPECT_REPLY,
			     0, 0, conn->id);
	ASSERT_RETURN(ret == -EINVAL);

	/* Faked replies with a valid reply cookie are rejected */
	ret = kdbus_msg_send_reply(conn, time(NULL) ^ cookie, sender->id);
	ASSERT_RETURN(ret == -EPERM);

	ret = kdbus_free(conn, offset);
	ASSERT_RETURN(ret == 0);

	kdbus_conn_free(sender);
	kdbus_conn_free(conn);

	return TEST_OK;
}

static int msg_recv_prio(struct kdbus_conn *conn,
			 int64_t requested_prio,
			 int64_t expected_prio)
{
	struct kdbus_cmd_recv recv = {
		.size = sizeof(recv),
		.flags = KDBUS_RECV_USE_PRIORITY,
		.priority = requested_prio,
	};
	struct kdbus_msg *msg;
	int ret;

	ret = kdbus_cmd_recv(conn->fd, &recv);
	if (ret < 0) {
		kdbus_printf("error receiving message: %d (%m)\n", -errno);
		return ret;
	}

	msg = (struct kdbus_msg *)(conn->buf + recv.msg.offset);
	kdbus_msg_dump(conn, msg);

	if (msg->priority != expected_prio) {
		kdbus_printf("expected message prio %lld, got %lld\n",
			     (unsigned long long) expected_prio,
			     (unsigned long long) msg->priority);
		return -EINVAL;
	}

	kdbus_msg_free(msg);
	ret = kdbus_free(conn, recv.msg.offset);
	if (ret < 0)
		return ret;

	return 0;
}

int kdbus_test_message_prio(struct kdbus_test_env *env)
{
	struct kdbus_conn *a, *b;
	uint64_t cookie = 0;

	a = kdbus_hello(env->buspath, 0, NULL, 0);
	b = kdbus_hello(env->buspath, 0, NULL, 0);
	ASSERT_RETURN(a && b);

	ASSERT_RETURN(kdbus_msg_send(b, NULL, ++cookie, 0, 0,   25, a->id) == 0);
	ASSERT_RETURN(kdbus_msg_send(b, NULL, ++cookie, 0, 0, -600, a->id) == 0);
	ASSERT_RETURN(kdbus_msg_send(b, NULL, ++cookie, 0, 0,   10, a->id) == 0);
	ASSERT_RETURN(kdbus_msg_send(b, NULL, ++cookie, 0, 0,  -35, a->id) == 0);
	ASSERT_RETURN(kdbus_msg_send(b, NULL, ++cookie, 0, 0, -100, a->id) == 0);
	ASSERT_RETURN(kdbus_msg_send(b, NULL, ++cookie, 0, 0,   20, a->id) == 0);
	ASSERT_RETURN(kdbus_msg_send(b, NULL, ++cookie, 0, 0,  -15, a->id) == 0);
	ASSERT_RETURN(kdbus_msg_send(b, NULL, ++cookie, 0, 0, -800, a->id) == 0);
	ASSERT_RETURN(kdbus_msg_send(b, NULL, ++cookie, 0, 0, -150, a->id) == 0);
	ASSERT_RETURN(kdbus_msg_send(b, NULL, ++cookie, 0, 0,   10, a->id) == 0);
	ASSERT_RETURN(kdbus_msg_send(b, NULL, ++cookie, 0, 0, -800, a->id) == 0);
	ASSERT_RETURN(kdbus_msg_send(b, NULL, ++cookie, 0, 0,  -10, a->id) == 0);

	ASSERT_RETURN(msg_recv_prio(a, -200, -800) == 0);
	ASSERT_RETURN(msg_recv_prio(a, -100, -800) == 0);
	ASSERT_RETURN(msg_recv_prio(a, -400, -600) == 0);
	ASSERT_RETURN(msg_recv_prio(a, -400, -600) == -EAGAIN);
	ASSERT_RETURN(msg_recv_prio(a, 10, -150) == 0);
	ASSERT_RETURN(msg_recv_prio(a, 10, -100) == 0);

	kdbus_printf("--- get priority (all)\n");
	ASSERT_RETURN(kdbus_msg_recv(a, NULL, NULL) == 0);

	kdbus_conn_free(a);
	kdbus_conn_free(b);

	return TEST_OK;
}

static int kdbus_test_notify_kernel_quota(struct kdbus_test_env *env)
{
	int ret;
	unsigned int i;
	struct kdbus_conn *conn;
	struct kdbus_conn *reader;
	struct kdbus_msg *msg = NULL;
	struct kdbus_cmd_recv recv = { .size = sizeof(recv) };

	reader = kdbus_hello(env->buspath, 0, NULL, 0);
	ASSERT_RETURN(reader);

	conn = kdbus_hello(env->buspath, 0, NULL, 0);
	ASSERT_RETURN(conn);

	/* Register for ID signals */
	ret = kdbus_add_match_id(reader, 0x1, KDBUS_ITEM_ID_ADD,
				 KDBUS_MATCH_ID_ANY);
	ASSERT_RETURN(ret == 0);

	ret = kdbus_add_match_id(reader, 0x2, KDBUS_ITEM_ID_REMOVE,
				 KDBUS_MATCH_ID_ANY);
	ASSERT_RETURN(ret == 0);

	/* Each iteration two notifications: add and remove ID */
	for (i = 0; i < KDBUS_CONN_MAX_MSGS / 2; i++) {
		struct kdbus_conn *notifier;

		notifier = kdbus_hello(env->buspath, 0, NULL, 0);
		ASSERT_RETURN(notifier);

		kdbus_conn_free(notifier);
	}

	/*
	 * Now the reader queue is full with kernel notfications,
	 * but as a user we still have room to push our messages.
	 */
	ret = kdbus_msg_send(conn, NULL, 0xdeadbeef, 0, 0, 0, reader->id);
	ASSERT_RETURN(ret == 0);

	/* More ID kernel notifications that will be lost */
	kdbus_conn_free(conn);

	conn = kdbus_hello(env->buspath, 0, NULL, 0);
	ASSERT_RETURN(conn);

	kdbus_conn_free(conn);

	/*
	 * We lost only 3 packets since only signal msgs are
	 * accounted. The connection ID add/remove notification
	 */
	ret = kdbus_cmd_recv(reader->fd, &recv);
	ASSERT_RETURN(ret == 0);
	ASSERT_RETURN(recv.return_flags & KDBUS_RECV_RETURN_DROPPED_MSGS);
	ASSERT_RETURN(recv.dropped_msgs == 3);

	msg = (struct kdbus_msg *)(reader->buf + recv.msg.offset);
	kdbus_msg_free(msg);

	/* Read our queue */
	for (i = 0; i < KDBUS_CONN_MAX_MSGS - 1; i++) {
		memset(&recv, 0, sizeof(recv));
		recv.size = sizeof(recv);

		ret = kdbus_cmd_recv(reader->fd, &recv);
		ASSERT_RETURN(ret == 0);
		ASSERT_RETURN(!(recv.return_flags &
			        KDBUS_RECV_RETURN_DROPPED_MSGS));

		msg = (struct kdbus_msg *)(reader->buf + recv.msg.offset);
		kdbus_msg_free(msg);
	}

	ret = kdbus_msg_recv(reader, NULL, NULL);
	ASSERT_RETURN(ret == 0);

	ret = kdbus_msg_recv(reader, NULL, NULL);
	ASSERT_RETURN(ret == -EAGAIN);

	kdbus_conn_free(reader);

	return 0;
}

/* Return the number of message successfully sent */
static int kdbus_fill_conn_queue(struct kdbus_conn *conn_src,
				 uint64_t dst_id,
				 unsigned int max_msgs)
{
	unsigned int i;
	uint64_t cookie = 0;
	int ret;

	for (i = 0; i < max_msgs; i++) {
		ret = kdbus_msg_send(conn_src, NULL, ++cookie,
				     0, 0, 0, dst_id);
		if (ret < 0)
			break;
	}

	return i;
}

static int kdbus_test_expected_reply_quota(struct kdbus_test_env *env)
{
	int ret;
	unsigned int i, n;
	unsigned int count;
	uint64_t cookie = 0x1234abcd5678eeff;
	struct kdbus_conn *conn;
	struct kdbus_conn *connections[9];

	conn = kdbus_hello(env->buspath, 0, NULL, 0);
	ASSERT_RETURN(conn);

	for (i = 0; i < 9; i++) {
		connections[i] = kdbus_hello(env->buspath, 0, NULL, 0);
		ASSERT_RETURN(connections[i]);
	}

	count = 0;
	/* Send 16 messages to 8 different connections */
	for (i = 0; i < 8; i++) {
		for (n = 0; n < KDBUS_CONN_MAX_MSGS_PER_USER; n++, count++) {
			ret = kdbus_msg_send(conn, NULL, cookie++,
					     KDBUS_MSG_EXPECT_REPLY,
					     100000000ULL, 0,
					     connections[i]->id);
			ASSERT_RETURN(ret == 0);
		}
	}

	ASSERT_RETURN(count == KDBUS_CONN_MAX_REQUESTS_PENDING);

	/*
	 * Now try to send a message to the last connection,
	 * if we have reached KDBUS_CONN_MAX_REQUESTS_PENDING
	 * no further requests are allowed
	 */
	ret = kdbus_msg_send(conn, NULL, cookie++, KDBUS_MSG_EXPECT_REPLY,
			     1000000000ULL, 0, connections[i]->id);
	ASSERT_RETURN(ret == -EMLINK);

	for (i = 0; i < 9; i++)
		kdbus_conn_free(connections[i]);

	kdbus_conn_free(conn);

	return 0;
}

int kdbus_test_pool_quota(struct kdbus_test_env *env)
{
	struct kdbus_conn *a, *b, *c;
	struct kdbus_cmd_send cmd = {};
	struct kdbus_item *item;
	struct kdbus_msg *recv_msg;
	struct kdbus_msg *msg;
	uint64_t cookie = time(NULL);
	uint64_t size;
	unsigned int i;
	char *payload;
	int ret;

	/* just a guard */
	if (POOL_SIZE <= KDBUS_MSG_MAX_PAYLOAD_VEC_SIZE ||
	    POOL_SIZE % KDBUS_MSG_MAX_PAYLOAD_VEC_SIZE != 0)
		return 0;

	payload = calloc(KDBUS_MSG_MAX_PAYLOAD_VEC_SIZE, sizeof(char));
	ASSERT_RETURN_VAL(payload, -ENOMEM);

	a = kdbus_hello(env->buspath, 0, NULL, 0);
	b = kdbus_hello(env->buspath, 0, NULL, 0);
	c = kdbus_hello(env->buspath, 0, NULL, 0);
	ASSERT_RETURN(a && b && c);

	size = sizeof(struct kdbus_msg);
	size += KDBUS_ITEM_SIZE(sizeof(struct kdbus_vec));

	msg = malloc(size);
	ASSERT_RETURN_VAL(msg, -ENOMEM);

	memset(msg, 0, size);
	msg->size = size;
	msg->src_id = a->id;
	msg->dst_id = c->id;
	msg->payload_type = KDBUS_PAYLOAD_DBUS;

	item = msg->items;
	item->type = KDBUS_ITEM_PAYLOAD_VEC;
	item->size = KDBUS_ITEM_HEADER_SIZE + sizeof(struct kdbus_vec);
	item->vec.address = (uintptr_t)payload;
	item->vec.size = KDBUS_MSG_MAX_PAYLOAD_VEC_SIZE;
	item = KDBUS_ITEM_NEXT(item);

	cmd.size = sizeof(cmd);
	cmd.msg_address = (uintptr_t)msg;

	/*
	 * Send 2097248 bytes, a user is only allowed to get 33% of
	 * the free space of the pool, the already used space is
	 * accounted as free space
	 */
	size += KDBUS_MSG_MAX_PAYLOAD_VEC_SIZE;
	for (i = size; i < (POOL_SIZE / 3); i += size) {
		msg->cookie = cookie++;

		ret = kdbus_cmd_send(a->fd, &cmd);
		ASSERT_RETURN_VAL(ret == 0, ret);
	}

	/* Try to get more than 33% */
	msg->cookie = cookie++;
	ret = kdbus_cmd_send(a->fd, &cmd);
	ASSERT_RETURN(ret == -ENOBUFS);

	/* We still can pass small messages */
	ret = kdbus_msg_send(b, NULL, cookie++, 0, 0, 0, c->id);
	ASSERT_RETURN(ret == 0);

	for (i = size; i < (POOL_SIZE/ 3); i += size) {
		ret = kdbus_msg_recv(c, &recv_msg, NULL);
		ASSERT_RETURN(ret == 0);
		ASSERT_RETURN(recv_msg->src_id == a->id);

		kdbus_msg_free(recv_msg);
	}

	ret = kdbus_msg_recv(c, &recv_msg, NULL);
	ASSERT_RETURN(ret == 0);
	ASSERT_RETURN(recv_msg->src_id == b->id);

	kdbus_msg_free(recv_msg);

	ret = kdbus_msg_recv(c, NULL, NULL);
	ASSERT_RETURN(ret == -EAGAIN);

	free(msg);
	free(payload);

	kdbus_conn_free(c);
	kdbus_conn_free(b);
	kdbus_conn_free(a);

	return 0;
}

int kdbus_test_message_quota(struct kdbus_test_env *env)
{
	struct kdbus_conn *a, *b;
	uint64_t cookie = 0;
	int ret;
	int i;

	ret = kdbus_test_notify_kernel_quota(env);
	ASSERT_RETURN(ret == 0);

	ret = kdbus_test_pool_quota(env);
	ASSERT_RETURN(ret == 0);

	ret = kdbus_test_expected_reply_quota(env);
	ASSERT_RETURN(ret == 0);

	a = kdbus_hello(env->buspath, 0, NULL, 0);
	b = kdbus_hello(env->buspath, 0, NULL, 0);

	ret = kdbus_fill_conn_queue(b, a->id, KDBUS_CONN_MAX_MSGS);
	ASSERT_RETURN(ret < KDBUS_CONN_MAX_MSGS);

	ret = kdbus_msg_send(b, NULL, ++cookie, 0, 0, 0, a->id);
	ASSERT_RETURN(ret == -ENOBUFS);

	for (i = 0; i < KDBUS_CONN_MAX_MSGS; ++i) {
		ret = kdbus_msg_recv(a, NULL, NULL);
		if (ret == -EAGAIN)
			break;
		ASSERT_RETURN(ret == 0);
	}

	ret = kdbus_fill_conn_queue(b, a->id, KDBUS_CONN_MAX_MSGS);
	ASSERT_RETURN(ret < KDBUS_CONN_MAX_MSGS);

	ret = kdbus_msg_send(b, NULL, ++cookie, 0, 0, 0, a->id);
	ASSERT_RETURN(ret == -ENOBUFS);

	kdbus_conn_free(a);
	kdbus_conn_free(b);

	return TEST_OK;
}
