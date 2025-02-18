#include "corobus.h"

#include "libcoro.h"
#include "rlist.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

struct data_vector {
	unsigned *data;
	size_t size;
	size_t capacity;
};

#if 1 /* Uncomment this if want to use */

/** Append @a count messages in @a data to the end of the vector. */
static void
data_vector_append_many(struct data_vector *vector,
	const unsigned *data, size_t count)
{
	if (vector->size + count > vector->capacity) {
		if (vector->capacity == 0)
			vector->capacity = 4;
		else
			vector->capacity *= 2;
		if (vector->capacity < vector->size + count)
			vector->capacity = vector->size + count;
		vector->data = realloc(vector->data,
			sizeof(vector->data[0]) * vector->capacity);
	}
	memcpy(&vector->data[vector->size], data, sizeof(data[0]) * count);
	vector->size += count;
}

/** Append a single message to the vector. */
static void
data_vector_append(struct data_vector *vector, unsigned data)
{
	data_vector_append_many(vector, &data, 1);
}

/** Pop @a count of messages into @a data from the head of the vector. */
static void
data_vector_pop_first_many(struct data_vector *vector, unsigned *data, size_t count)
{
	assert(count <= vector->size);
	memcpy(data, vector->data, sizeof(data[0]) * count);
	vector->size -= count;
	memmove(vector->data, &vector->data[count], vector->size * sizeof(vector->data[0]));
}

/** Pop a single message from the head of the vector. */
static unsigned
data_vector_pop_first(struct data_vector *vector)
{
	unsigned data = 0;
	data_vector_pop_first_many(vector, &data, 1);
	return data;
}

#endif

/**
 * One coroutine waiting to be woken up in a list of other
 * suspended coros.
 */
struct wakeup_entry {
	struct rlist base;
	struct coro *coro;
};

/** A queue of suspended coros waiting to be woken up. */
struct wakeup_queue {
	struct rlist coros;
};

#if 1 /* Uncomment this if want to use */

/** Suspend the current coroutine until it is woken up. */
static void
wakeup_queue_suspend_this(struct wakeup_queue *queue)
{
	struct wakeup_entry entry;
	entry.coro = coro_this();
	rlist_add_tail_entry(&queue->coros, &entry, base);
	coro_suspend();
	rlist_del_entry(&entry, base);
}

/** Wakeup the first coroutine in the queue. */
static void
wakeup_queue_wakeup_first(struct wakeup_queue *queue)
{
	if (rlist_empty(&queue->coros))
		return;
	struct wakeup_entry *entry = rlist_first_entry(&queue->coros,
		struct wakeup_entry, base);
	coro_wakeup(entry->coro);
}

#endif

struct coro_bus_channel {
	/** Channel max capacity. */
	size_t size_limit;
	/** Coroutines waiting until the channel is not full. */
	struct wakeup_queue send_queue;
	/** Coroutines waiting until the channel is not empty. */
	struct wakeup_queue recv_queue;
	/** Message queue. */
	struct data_vector data;
};

struct coro_bus {
	struct coro_bus_channel **channels;
	int channel_count;
};

static enum coro_bus_error_code global_error = CORO_BUS_ERR_NONE;


enum coro_bus_error_code
coro_bus_errno(void)
{
	return global_error;
}

void
coro_bus_errno_set(enum coro_bus_error_code err)
{
	global_error = err;
}

bool
is_channel_valid(struct coro_bus *bus, int channel) {
	return channel >= 0 && channel < bus->channel_count && bus->channels[channel] != NULL;
}

struct coro_bus *
coro_bus_new(void)
{
    struct coro_bus *new_bus = (struct coro_bus *) malloc(sizeof(struct coro_bus));
    if (new_bus) {
        memset(new_bus, 0, sizeof(struct coro_bus));
        new_bus->channel_count = 0;
        new_bus->channels = (struct coro_bus_channel **) 0;
    }
    return new_bus;
}

void
coro_bus_delete(struct coro_bus *bus)
{
    int idx = 0;
    while (idx < bus->channel_count) {
        struct coro_bus_channel *channel = bus->channels[idx];
        if (channel) {
            if (channel->data.data) {
                free(channel->data.data);
                channel->data.data = (unsigned *) 0xDEADBEEF; 
            }
            free(channel);
            bus->channels[idx] = (struct coro_bus_channel *) 0xDEADBEEF; 
        }
        idx++;
    }

    if (bus->channels) {
        free(bus->channels);
        bus->channels = (struct coro_bus_channel **) 0xDEADBEEF;
    }

    if (bus) {
        free(bus);
        bus = (struct coro_bus *) 0xDEADBEEF;
    }
}

int
coro_bus_channel_open(struct coro_bus *bus, size_t size_limit)
{
    struct coro_bus_channel *new_channel = (struct coro_bus_channel *) malloc(sizeof(struct coro_bus_channel));
    if (!new_channel) return -1; 

    memset(new_channel, 0, sizeof(struct coro_bus_channel));
    new_channel->size_limit = size_limit;

    struct rlist *send_list = &new_channel->send_queue.coros;
    struct rlist *recv_list = &new_channel->recv_queue.coros;
    send_list->next = send_list;
    send_list->prev = send_list;
    recv_list->next = recv_list;
    recv_list->prev = recv_list;

    new_channel->data.data = (unsigned *) 0;
    new_channel->data.size = 0;
    new_channel->data.capacity = 0;

    int slot = -1;
    for (int i = 0; i < bus->channel_count; ++i) {
        if (!bus->channels[i]) {
            slot = i;
            break;
        }
    }

    if (slot == -1) {
        bus->channels = (struct coro_bus_channel **) realloc(bus->channels, sizeof(struct coro_bus_channel *) * (bus->channel_count + 1));
        slot = bus->channel_count++;
    }

    bus->channels[slot] = new_channel;
    return slot;
}

void
coro_bus_channel_close(struct coro_bus *bus, int channel)
{
    struct coro_bus_channel *channel_ptr = bus->channels[channel];
    bus->channels[channel] = (struct coro_bus_channel *) 0;

    struct rlist *send_list = &channel_ptr->send_queue.coros;
    struct wakeup_entry *send_entry;
    for (send_entry = (struct wakeup_entry *) send_list->next;
         send_entry != (struct wakeup_entry *) send_list;
         send_entry = (struct wakeup_entry *) send_entry->base.next) {
        coro_wakeup(send_entry->coro);
    }

    struct rlist *recv_list = &channel_ptr->recv_queue.coros;
    struct wakeup_entry *recv_entry;
    for (recv_entry = (struct wakeup_entry *) recv_list->next;
         recv_entry != (struct wakeup_entry *) recv_list;
         recv_entry = (struct wakeup_entry *) recv_entry->base.next) {
        coro_wakeup(recv_entry->coro);
    }

    coro_yield();

    if (channel_ptr->data.data) {
        free(channel_ptr->data.data);
        channel_ptr->data.data = (unsigned *) 0xDEADBEEF; 
    }

    free(channel_ptr);
    channel_ptr = (struct coro_bus_channel *) 0xDEADBEEF;
}


int
coro_bus_send(struct coro_bus *bus, int channel, unsigned data)
{
    if (!is_channel_valid(bus, channel)) {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return -1;
    }

    struct coro_bus_channel *current_channel = bus->channels[channel];

    while (current_channel->data.size >= current_channel->size_limit) {
        wakeup_queue_suspend_this(&current_channel->send_queue);

        if (!is_channel_valid(bus, channel)) {
            coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
            return -1;
        }
    }

    data_vector_append(&current_channel->data, data);
    wakeup_queue_wakeup_first(&current_channel->recv_queue);

    coro_bus_errno_set(CORO_BUS_ERR_NONE);

    return 0;
}

int
coro_bus_try_send(struct coro_bus *bus, int channel, unsigned data)
{
    if (!is_channel_valid(bus, channel)) {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return -1;
    }

    struct coro_bus_channel *target_channel = bus->channels[channel];

    if (!(target_channel->data.size < target_channel->size_limit)) {
        coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
        return -1;
    }

    data_vector_append(&target_channel->data, data);
    wakeup_queue_wakeup_first(&target_channel->recv_queue);

    return 0;
}

int
coro_bus_recv(struct coro_bus *bus, int channel, unsigned *data)
{
    if (!is_channel_valid(bus, channel)) {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return -1;
    }

    struct coro_bus_channel *current_channel = bus->channels[channel];

    while (current_channel->data.size == 0) {
        wakeup_queue_suspend_this(&current_channel->recv_queue);

        if (!is_channel_valid(bus, channel)) {
            coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
            return -1;
        }
    }

    *data = data_vector_pop_first(&current_channel->data);
    wakeup_queue_wakeup_first(&current_channel->send_queue);

    return 0;
}

int
coro_bus_try_recv(struct coro_bus *bus, int channel, unsigned *data)
{
    if (!is_channel_valid(bus, channel)) {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return -1;
    }

    struct coro_bus_channel *target_channel = bus->channels[channel];

    if (target_channel->data.size == 0) {
        coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
        return -1;
    }

    *data = data_vector_pop_first(&target_channel->data);
    wakeup_queue_wakeup_first(&target_channel->send_queue);

    return 0;
}


#if NEED_BROADCAST

int
coro_bus_broadcast(struct coro_bus *bus, unsigned data)
{
    int valid_channels = 0;

    for (int i = 0; i < bus->channel_count; i++) {
        if (bus->channels[i] != NULL) {
            valid_channels++;
        }
    }

    if (valid_channels == 0) {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return -1;
    }

    for (int i = 0; i < bus->channel_count; i++) {
        if (bus->channels[i] == NULL) {
            continue;
        }

        struct coro_bus_channel *ch = bus->channels[i];

        while (ch->data.size >= ch->size_limit) {
            wakeup_queue_suspend_this(&ch->send_queue);

            if (!is_channel_valid(bus, i)) {
                coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
                return -1;
            }
        }
    }

    for (int i = 0; i < bus->channel_count; i++) {
        if (bus->channels[i] == NULL) {
            continue;
        }

        if (coro_bus_send(bus, i, data) == -1) {
            return -1;
        }
    }

    return 0;
}


int
coro_bus_try_broadcast(struct coro_bus *bus, unsigned data)
{
    int open_channels = 0;

    for (int i = 0; i < bus->channel_count; i++) {
        if (bus->channels[i] != NULL) {
            open_channels = 1;
            break;
        }
    }

    if (!open_channels) {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return -1;
    }

    for (int i = 0; i < bus->channel_count; i++) {
        if (bus->channels[i] == NULL) {
            continue;
        }

        if (bus->channels[i]->data.size >= bus->channels[i]->size_limit) {
            coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
            return -1;
        }
    }

    for (int i = 0; i < bus->channel_count; i++) {
        if (bus->channels[i] == NULL) {
            continue;
        }

        if (coro_bus_send(bus, i, data) == -1) {
            return -1;
        }
    }

    return 0;
}

#endif

#if NEED_BATCH

int
coro_bus_send_v(struct coro_bus *bus, int channel, const unsigned *data, unsigned count)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)bus;
	(void)channel;
	(void)data;
	(void)count;
	coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
	return -1;
}

int
coro_bus_try_send_v(struct coro_bus *bus, int channel, const unsigned *data, unsigned count)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)bus;
	(void)channel;
	(void)data;
	(void)count;
	coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
	return -1;
}

int
coro_bus_recv_v(struct coro_bus *bus, int channel, unsigned *data, unsigned capacity)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)bus;
	(void)channel;
	(void)data;
	(void)capacity;
	coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
	return -1;
}

int
coro_bus_try_recv_v(struct coro_bus *bus, int channel, unsigned *data, unsigned capacity)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)bus;
	(void)channel;
	(void)data;
	(void)capacity;
	coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
	return -1;
}

#endif