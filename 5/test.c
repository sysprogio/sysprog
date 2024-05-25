#include "unit.h"
#include "chat.h"
#include "chat_client.h"
#include "chat_server.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>

enum {
	TEST_MSG_ID_LEN = 64,
};

struct test_msg {
	uint32_t len;
	uint32_t size;
	char *data;
};

static struct test_msg *
test_msg_new(uint32_t len)
{
	len += TEST_MSG_ID_LEN;
	uint32_t size = len + 1;
	struct test_msg *res = malloc(sizeof(*res) + size);
	res->data = (void *)(res + 1);
	res->len = len;
	res->size = size;
	memset(res->data, '0', TEST_MSG_ID_LEN);
	for (uint32_t i = TEST_MSG_ID_LEN; i < len; ++i)
		res->data[i] = 'a' + i % ('z' - 'a' + 1);
	res->data[len] = '\n';
	return res;
}

static void
test_msg_set_id(struct test_msg *msg, int cli_id, int msg_id)
{
	unit_fail_if(msg->len < TEST_MSG_ID_LEN);
	memset(msg->data, '0', TEST_MSG_ID_LEN);
	int rc = sprintf(msg->data, "cli_%d_msg_%d ", cli_id, msg_id);
	// Ignore terminating zero.
	msg->data[rc] = '0';
}

static void
test_msg_clear_id(struct test_msg *msg)
{
	unit_fail_if(msg->len < TEST_MSG_ID_LEN);
	memset(msg->data, '0', TEST_MSG_ID_LEN);
}

static void
test_msg_check_data(const struct test_msg *msg, const char *data)
{
	uint32_t len = strlen(data);
	unit_fail_if(len != msg->len);
	unit_fail_if(len < TEST_MSG_ID_LEN);
	unit_fail_if(memcmp(msg->data, data, len) != 0);
}

static void
test_msg_delete(struct test_msg *msg)
{
	free(msg);
}

static void
chat_message_extract_id(struct chat_message *msg, int *cli_id, int *msg_id)
{
	uint32_t len = strlen(msg->data);
	unit_fail_if(len < TEST_MSG_ID_LEN);
	int rc = sscanf(msg->data, "cli_%d_msg_%d ", cli_id, msg_id);
	unit_fail_if(rc != 2);
	memset(msg->data, '0', TEST_MSG_ID_LEN);
}

static uint16_t
server_get_port(const struct chat_server *s)
{
	int sock = chat_server_get_socket(s);
	unit_fail_if(sock < 0);
	struct sockaddr_storage addr;
	socklen_t len = sizeof(addr);
	int rc = getsockname(sock, (void *)&addr, &len);
	unit_fail_if(rc != 0);
	if (addr.ss_family == AF_INET)
		return ntohs(((struct sockaddr_in *)&addr)->sin_port);
	unit_fail_if(addr.ss_family != AF_INET6);
	return ntohs(((struct sockaddr_in6 *)&addr)->sin6_port);
}

static inline const char *
make_addr_str(uint16_t port)
{
	static __thread char host[128];
	sprintf(host, "localhost:%u", port);
	return host;
}

static void
server_consume_events(struct chat_server *s)
{
	int rc;
	while ((rc = chat_server_update(s, 0)) == 0)
		{};
	unit_fail_if(rc != CHAT_ERR_TIMEOUT);
}

static struct chat_message *
server_pop_next_blocking_from(struct chat_server *s, struct chat_client *c)
{
	struct chat_message *msg;
	while ((msg = chat_server_pop_next(s)) == NULL) {
		chat_client_update(c, 0);
		chat_server_update(s, 0);
	}
	return msg;
}

static void
client_consume_events(struct chat_client *c)
{
	int rc;
	while ((rc = chat_client_update(c, 0)) == 0)
		{};
	unit_fail_if(rc != CHAT_ERR_TIMEOUT);
}

static struct chat_message *
client_pop_next_blocking(struct chat_client *c, struct chat_server *s)
{
	struct chat_message *msg;
	while ((msg = chat_client_pop_next(c)) == NULL) {
		chat_client_update(c, 0);
		chat_server_update(s, 0);
	}
	return msg;
}

static bool
author_is_eq(const struct chat_message *msg, const char *name)
{
#ifdef NEED_AUTHOR
	return strcmp(msg->author, name) == 0;
#else
	(void)msg;
	(void)name;
	return true;
#endif
}

static void
test_basic(void)
{
	unit_test_start();
	//
	// Delete the server without listen.
	//
	struct chat_server *s = chat_server_new();
	unit_check(chat_server_get_socket(s) < 0, "server no socket");
	unit_check(chat_server_get_events(s) == 0, "server no events");
	unit_check(chat_server_update(s, 0) == CHAT_ERR_NOT_STARTED,
		   "server no update");
	chat_server_delete(s);
	//
	// Delete the server right after listen.
	//
	s = chat_server_new();
	int rc = chat_server_listen(s, 0);
	unit_check(rc == 0, "listen");
	unit_check(chat_server_get_socket(s) >= 0, "server has socket");
	unit_check(server_get_port(s) > 0, "has port");
	unit_check(chat_server_get_events(s) != 0, "has events");
	chat_server_delete(s);
	//
	// Delete the client right away.
	//
	struct chat_client *c1 = chat_client_new("c1");
	unit_check(chat_client_update(c1, 0) == CHAT_ERR_NOT_STARTED,
		   "client no update");
	unit_check(chat_client_get_descriptor(c1) < 0, "client no socket");
	unit_check(chat_client_get_events(c1) == 0, "client no events");
	chat_client_delete(c1);
	//
	// Delete the client after connect.
	//
	s = chat_server_new();
	rc = chat_server_listen(s, 0);
	uint16_t port = server_get_port(s);
	c1 = chat_client_new("c1");
	unit_check(chat_server_get_events(s) == CHAT_EVENT_INPUT,
		   "server needs input");
	unit_check(chat_server_update(s, 0) == CHAT_ERR_TIMEOUT,
		   "server no clients yet");
	unit_check(chat_client_connect(c1, make_addr_str(port)) == 0,
		   "connect");
	unit_check(chat_server_update(s, 0) == 0, "server got client");
	unit_check(chat_server_get_events(s) == CHAT_EVENT_INPUT,
		   "server always needs more input");
	chat_client_delete(c1);
	unit_check(chat_server_update(s, 0) == 0, "server lost client");
	unit_check(chat_server_get_events(s) == CHAT_EVENT_INPUT,
		   "server needs more input");
	//
	// Connect a client and send a message.
	//
	c1 = chat_client_new("c1");
	unit_fail_if(chat_client_connect(c1, make_addr_str(port)) != 0);
	unit_check(chat_client_feed(c1, "hello\n", 6) == 0, "feed to client");
	client_consume_events(c1);
	server_consume_events(s);
	struct chat_message *msg = chat_server_pop_next(s);
	unit_check(msg != NULL, "server got msg");
	unit_check(strcmp(msg->data, "hello") == 0, "msg data");
	unit_check(author_is_eq(msg, "c1"), "msg author");
	chat_message_delete(msg);
	//
	// Send a non-zero terminated message.
	//
	// Yes, 5, not 10. Send only "msg1\n".
	unit_check(chat_client_feed(c1, "msg1\nmsg2\n", 5) == 0,
		   "feed a part of str");
	client_consume_events(c1);
	server_consume_events(s);
	msg = chat_server_pop_next(s);
	unit_check(strcmp(msg->data, "msg1") == 0, "msg data");
	unit_check(author_is_eq(msg, "c1"), "msg author");
	chat_message_delete(msg);
	unit_check(chat_server_pop_next(s) == NULL, "no more messages");
	chat_client_delete(c1);
	chat_server_delete(s);

	unit_test_finish();
}

static void
test_big_messages(void)
{
	unit_test_start();

	struct chat_server *s = chat_server_new();
	unit_fail_if(chat_server_listen(s, 0) != 0);
	uint16_t port = server_get_port(s);
	struct chat_client *c1 = chat_client_new("c1");
	unit_fail_if(chat_client_connect(c1, make_addr_str(port)) != 0);

	uint32_t len = 1024 * 1024;
	struct test_msg *test_msg = test_msg_new(len);
	int rc;
	struct chat_message *msg;
	int count = 100;
	int real_count = 0;
	for (int i = 0; i < count; ++i) {
		unit_fail_if(chat_client_feed(c1,
			test_msg->data, test_msg->size) != 0);
		rc = chat_client_update(c1, 0);
		unit_fail_if(rc != 0 && rc != CHAT_ERR_TIMEOUT);
		rc = chat_server_update(s, 0);
		unit_fail_if(rc != 0 && rc != CHAT_ERR_TIMEOUT);
		while ((msg = chat_server_pop_next(s)) != NULL) {
			++real_count;
			test_msg_check_data(test_msg, msg->data);
			unit_fail_if(!author_is_eq(msg, "c1"));
			chat_message_delete(msg);
		}
	}
	while (true) {
		int rc1 = chat_client_update(c1, 0);
		unit_fail_if(rc1 != 0 && rc1 != CHAT_ERR_TIMEOUT);
		int rc2 = chat_server_update(s, 0);
		unit_fail_if(rc2 != 0 && rc2 != CHAT_ERR_TIMEOUT);
		while ((msg = chat_server_pop_next(s)) != NULL) {
			++real_count;
			test_msg_check_data(test_msg, msg->data);
			unit_fail_if(!author_is_eq(msg, "c1"));
			chat_message_delete(msg);
		}
		if (rc1 == CHAT_ERR_TIMEOUT && rc2 == CHAT_ERR_TIMEOUT)
			break;
	}
	unit_check(real_count == count, "server got all msgs");
	unit_check(chat_server_pop_next(s) == NULL, "server has no msgs");
	unit_check(chat_client_pop_next(c1) == NULL, "client has no msgs");
	chat_client_delete(c1);
	chat_server_delete(s);
	test_msg_delete(test_msg);

	unit_test_finish();
}

static void
test_multi_feed(void)
{
	unit_test_start();

	struct chat_server *s = chat_server_new();
	unit_fail_if(chat_server_listen(s, 0) != 0);
	uint16_t port = server_get_port(s);
	struct chat_client *c1 = chat_client_new("c1");
	unit_fail_if(chat_client_connect(c1, make_addr_str(port)) != 0);
	// Multiple messages in one string still should be split by '\n'. If
	// there is no '\n' in the end, then data must be accumulated.
	const char *data = "msg1\nmsg2\nmsg";
	unit_check(chat_client_feed(c1, data, strlen(data)) == 0,
		   "feed an incomplete batch");
	client_consume_events(c1);
	server_consume_events(s);
	struct chat_message *msg = chat_server_pop_next(s);
	unit_check(strcmp(msg->data, "msg1") == 0, "msg1");
	unit_check(author_is_eq(msg, "c1"), "msg1 author");
	chat_message_delete(msg);
	msg = chat_server_pop_next(s);
	unit_check(strcmp(msg->data, "msg2") == 0, "msg2");
	unit_check(author_is_eq(msg, "c1"), "msg2 author");
	chat_message_delete(msg);
	unit_check(chat_server_pop_next(s) == NULL, "waiting for msg3");
	unit_check(chat_client_feed(c1, "345", 3) == 0, "feed next part");
	client_consume_events(c1);
	server_consume_events(s);
	unit_check(chat_server_pop_next(s) == NULL, "still waiting for msg3");
	unit_check(chat_client_feed(c1, "678\n", 4) == 0, "feed next part");
	client_consume_events(c1);
	server_consume_events(s);
	msg = chat_server_pop_next(s);
	unit_check(strcmp(msg->data, "msg345678") == 0, "msg3");
	unit_check(author_is_eq(msg, "c1"), "msg3 author");
	chat_message_delete(msg);
	chat_client_delete(c1);
	chat_server_delete(s);

	unit_test_finish();
}

static void
test_multi_client(void)
{
	unit_test_start();

	struct chat_server *s = chat_server_new();
	unit_fail_if(chat_server_listen(s, 0) != 0);
	uint16_t port = server_get_port(s);
	int client_count = 20;
	int msg_count = 100;
	uint32_t len = 1024;
	struct test_msg *test_msg = test_msg_new(len);
	struct chat_message *msg;
	struct chat_client *cli;

	unit_msg("Connect clients");
	struct chat_client **clis = malloc(client_count * sizeof(clis[0]));
	for (int i = 0; i < client_count; ++i) {
		char name[128];
		sprintf(name, "cli_%d", i);
		cli = chat_client_new(name);
		unit_fail_if(chat_client_connect(
			cli, make_addr_str(port)) != 0);
		server_consume_events(s);
		clis[i] = cli;
	}
	unit_msg("Say hello");
	cli = clis[client_count - 1];
	unit_fail_if(chat_client_feed(cli, "hello\n", 6) != 0);
	msg = server_pop_next_blocking_from(s, cli);
	unit_fail_if(strcmp(msg->data, "hello") != 0);
	chat_message_delete(msg);
	for (int i = 0; i < client_count - 1; ++i) {
		cli = clis[i];
		msg = client_pop_next_blocking(cli, s);
		unit_fail_if(strcmp(msg->data, "hello") != 0);
		chat_message_delete(msg);
	}
	unit_msg("Send messages");
	for (int mi = 0; mi < msg_count; ++mi) {
		for (int ci = 0; ci < client_count; ++ci) {
			test_msg_set_id(test_msg, ci, mi);
			unit_fail_if(chat_client_feed(
				clis[ci], test_msg->data, test_msg->size) != 0);
			chat_client_update(clis[ci], 0);
		}
		chat_server_update(s, 0);
	}
	unit_msg("Consume all events");
	while (true)
	{
		bool have_events = false;
		for (int i = 0; i < client_count; ++i) {
			if (chat_client_update(clis[i], 0) == 0)
				have_events = true;
		}
		if (chat_server_update(s, 0) == 0)
			have_events = true;
		if (!have_events)
			break;
	}
	unit_msg("Check all is delivered");
	test_msg_clear_id(test_msg);
	int *msg_counts = calloc(client_count, sizeof(msg_counts[0]));
	for (int i = 0, end = msg_count * client_count; i < end; ++i) {
		msg = chat_server_pop_next(s);
		unit_fail_if(msg == NULL);
		int cli_id = -1;
		int msg_id = -1;
		chat_message_extract_id(msg, &cli_id, &msg_id);
		unit_fail_if(cli_id > client_count || cli_id < 0);
		unit_fail_if(msg_id > msg_count || msg_id < 0);
		unit_fail_if(msg_counts[cli_id] != msg_id);
		++msg_counts[cli_id];

		char name[128];
		sprintf(name, "cli_%d", cli_id);
		test_msg_check_data(test_msg, msg->data);
		unit_fail_if(!author_is_eq(msg, name));

		chat_message_delete(msg);
	}
	for (int ci = 0; ci < client_count; ++ci) {
		memset(msg_counts, 0, client_count * sizeof(msg_counts[0]));
		cli = clis[ci];
		// -1 because own messages are not delivered to self.
		int total_msg_count = msg_count * (client_count - 1);
		for (int mi = 0; mi < total_msg_count; ++mi) {
			msg = client_pop_next_blocking(cli, s);
			int cli_id = -1;
			int msg_id = -1;
			chat_message_extract_id(msg, &cli_id, &msg_id);
			unit_fail_if(cli_id > client_count || cli_id < 0);
			unit_fail_if(msg_id > msg_count || msg_id < 0);
			unit_fail_if(msg_counts[cli_id] != msg_id);
			++msg_counts[cli_id];

			char name[128];
			sprintf(name, "cli_%d", cli_id);
			test_msg_check_data(test_msg, msg->data);
			unit_fail_if(!author_is_eq(msg, name));

			chat_message_delete(msg);
		}
		unit_fail_if(msg_counts[ci] != 0);
	}
	for (int i = 0; i < client_count; ++i)
		chat_client_delete(clis[i]);
	free(clis);
	free(msg_counts);
	chat_server_delete(s);
	test_msg_delete(test_msg);

	unit_test_finish();
}

struct test_stress_ctx {
	int msg_count;
	uint32_t msg_len;
	int thread_idx;
	uint16_t port;
	bool is_running;
};

static void *
test_stress_worker_f(void *arg)
{
	struct test_stress_ctx *ctx = arg;
	int thread_idx = __atomic_fetch_add(
		&ctx->thread_idx, 1, __ATOMIC_RELAXED);
	struct test_msg *test_msg = test_msg_new(ctx->msg_len);
	char name[128];
	sprintf(name, "cli_%d", thread_idx);
	struct chat_client *cli = chat_client_new(name);
	unit_fail_if(chat_client_connect(cli, make_addr_str(ctx->port)) != 0);
	for (int i = 0; i < ctx->msg_count; ++i) {
		test_msg_set_id(test_msg, thread_idx, i);
		unit_fail_if(chat_client_feed(cli,
			test_msg->data, test_msg->size) != 0);
		chat_client_update(cli, 0);
	}
	while (__atomic_load_n(&ctx->is_running, __ATOMIC_RELAXED)) {
		int rc = chat_client_update(cli, 0.1);
		unit_fail_if(rc != 0 && rc != CHAT_ERR_TIMEOUT);
	}
	chat_client_delete(cli);
	test_msg_delete(test_msg);
	return NULL;
}

static void
test_stress(void)
{
	unit_test_start();

	struct chat_server *s = chat_server_new();
	unit_fail_if(chat_server_listen(s, 0) != 0);

	const int client_count = 10;
	struct test_stress_ctx ctx;
	ctx.msg_count = 100;
	ctx.msg_len = 1024;
	ctx.thread_idx = 0;
	ctx.port = server_get_port(s);
	ctx.is_running = true;

	unit_msg("Start client threads");
	pthread_t threads[client_count];
	for (int i = 0; i < client_count; ++i) {
		int rc = pthread_create(&threads[i], NULL,
			test_stress_worker_f, &ctx);
		unit_fail_if(rc != 0);
	}
	int *msg_counts = calloc(client_count, sizeof(msg_counts[0]));
	struct test_msg *test_msg = test_msg_new(ctx.msg_len);
	unit_msg("Receive all messages");
	for (int i = 0, end = ctx.msg_count * client_count; i < end; ++i) {
		struct chat_message *msg;
		while ((msg = chat_server_pop_next(s)) == NULL) {
			int rc = chat_server_update(s, 0.1);
			unit_fail_if(rc != 0 && rc != CHAT_ERR_TIMEOUT);
		}
		unit_fail_if(msg == NULL);
		int cli_id = -1;
		int msg_id = -1;
		chat_message_extract_id(msg, &cli_id, &msg_id);
		unit_fail_if(cli_id > client_count || cli_id < 0);
		unit_fail_if(msg_id > ctx.msg_count || msg_id < 0);
		unit_fail_if(msg_counts[cli_id] != msg_id);
		++msg_counts[cli_id];

		char name[128];
		sprintf(name, "cli_%d", cli_id);
		test_msg_check_data(test_msg, msg->data);
		unit_fail_if(!author_is_eq(msg, name));

		chat_message_delete(msg);
	}
	free(msg_counts);
	test_msg_delete(test_msg);
	unit_msg("Clean up the clients");
	__atomic_store_n(&ctx.is_running, false, __ATOMIC_RELAXED);
	for (int i = 0; i < client_count; ++i) {
		void *res;
		int rc = pthread_join(threads[i], &res);
		unit_fail_if(rc != 0);
	}
	chat_server_delete(s);

	unit_test_finish();
}

static void
test_big_author(void)
{
#ifdef NEED_AUTHOR
	unit_test_start();

	uint64_t author1_len = 10 * 1024 * 1024;
	char *author1 = malloc(author1_len + 1);
	memset(author1, 'z', author1_len);
	author1[author1_len] = 0;

	uint64_t author2_len = 11 * 1024 * 1024;
	char *author2 = malloc(author2_len + 1);
	memset(author2, 'y', author2_len);
	author2[author2_len] = 0;

	struct chat_server *s = chat_server_new();
	unit_fail_if(chat_server_listen(s, 0) != 0);
	uint16_t port = server_get_port(s);
	struct chat_client *c1 = chat_client_new(author1);
	unit_fail_if(chat_client_connect(c1, make_addr_str(port)) != 0);
	struct chat_client *c2 = chat_client_new(author2);
	unit_fail_if(chat_client_connect(c2, make_addr_str(port)) != 0);

	uint64_t body_len = 20 * 1024 * 1024;
	char *body = malloc(body_len + 1);
	memset(body, 'm', body_len);
	body[body_len] = '\n';

	unit_check(chat_client_feed(c1, body, body_len + 1) == 0, "feed to client");
	struct chat_message *msg = server_pop_next_blocking_from(s, c1);
	unit_check(msg != NULL, "server got msg");
	body[body_len] = 0;
	unit_check(strcmp(msg->data, body) == 0, "msg data");
	unit_check(author_is_eq(msg, author1), "msg author");
	chat_message_delete(msg);

	msg = client_pop_next_blocking(c2, s);
	unit_check(msg != NULL, "other client got msg");
	unit_check(strcmp(msg->data, body) == 0, "msg data");
	unit_check(author_is_eq(msg, author1), "msg author");
	chat_message_delete(msg);

	free(body);
	free(author1);
	free(author2);
	chat_client_delete(c1);
	chat_client_delete(c2);
	chat_server_delete(s);

	unit_test_finish();
#endif
}

static void
test_server_feed(void)
{
#if NEED_SERVER_FEED
	unit_test_start();

	struct chat_server *s = chat_server_new();
	unit_fail_if(chat_server_listen(s, 0) != 0);
	uint16_t port = server_get_port(s);

	struct chat_client *c1 = chat_client_new("c1");
	unit_fail_if(chat_client_connect(c1, make_addr_str(port)) != 0);

	struct chat_client *c2 = chat_client_new("c2");
	unit_fail_if(chat_client_connect(c2, make_addr_str(port)) != 0);

	const char *data = "msg1\nmsg2\nmsg3";
	unit_check(chat_server_feed(s, data, strlen(data)) == 0, "feed server");
	server_consume_events(s);

	struct chat_message *msg = client_pop_next_blocking(c1, s);
	unit_check(msg != NULL, "c1 got msg");
	unit_check(strcmp(msg->data, "msg1") == 0, "msg1");
	unit_check(author_is_eq(msg, "server"), "msg1 author");
	chat_message_delete(msg);

	msg = client_pop_next_blocking(c1, s);
	unit_check(msg != NULL, "c1 got msg");
	unit_check(strcmp(msg->data, "msg2") == 0, "msg2");
	unit_check(author_is_eq(msg, "server"), "msg2 author");
	chat_message_delete(msg);

	msg = client_pop_next_blocking(c2, s);
	unit_check(msg != NULL, "c2 got msg");
	unit_check(strcmp(msg->data, "msg1") == 0, "msg1");
	unit_check(author_is_eq(msg, "server"), "msg1 author");
	chat_message_delete(msg);

	msg = client_pop_next_blocking(c2, s);
	unit_check(msg != NULL, "c2 got msg");
	unit_check(strcmp(msg->data, "msg2") == 0, "msg2");
	unit_check(author_is_eq(msg, "server"), "msg2 author");
	chat_message_delete(msg);

	unit_check(chat_client_update(c1, 0) == CHAT_ERR_TIMEOUT,
		   "no more events in c1");
	unit_check(chat_client_update(c2, 0) == CHAT_ERR_TIMEOUT,
		   "no more events in c2");

	// Feed just one byte from the string, yes.
	unit_check(chat_server_feed(s, "\ntail\n", 1) == 0, "feed server");
	server_consume_events(s);

	msg = client_pop_next_blocking(c1, s);
	unit_check(msg != NULL, "c1 got msg");
	unit_check(strcmp(msg->data, "msg3") == 0, "msg3");
	unit_check(author_is_eq(msg, "server"), "msg3 author");
	chat_message_delete(msg);

	msg = client_pop_next_blocking(c2, s);
	unit_check(msg != NULL, "c2 got msg");
	unit_check(strcmp(msg->data, "msg3") == 0, "msg3");
	unit_check(author_is_eq(msg, "server"), "msg3 author");
	chat_message_delete(msg);

	unit_check(chat_client_update(c1, 0) == CHAT_ERR_TIMEOUT,
		   "no more events in c1");
	unit_check(chat_client_update(c2, 0) == CHAT_ERR_TIMEOUT,
		   "no more events in c2");

	chat_client_delete(c1);
	chat_client_delete(c2);
	chat_server_delete(s);

	unit_test_finish();
#endif
}

int
main(void)
{
	unit_test_start();

	test_server_feed();
	test_basic();
	test_big_messages();
	test_multi_feed();
	test_multi_client();
	test_stress();
	test_big_author();

	unit_test_finish();
	return 0;
}
