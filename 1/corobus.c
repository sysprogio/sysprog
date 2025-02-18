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
                        const unsigned *data, size_t count) {
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
data_vector_append(struct data_vector *vector, unsigned data) {
    data_vector_append_many(vector, &data, 1);
}

/** Pop @a count of messages into @a data from the head of the vector. */
static void
data_vector_pop_first_many(struct data_vector *vector, unsigned *data, size_t count) {
    assert(count <= vector->size);
    memcpy(data, vector->data, sizeof(data[0]) * count);
    vector->size -= count;
    memmove(vector->data, &vector->data[count], vector->size * sizeof(vector->data[0]));
}

/** Pop a single message from the head of the vector. */
static unsigned
data_vector_pop_first(struct data_vector *vector) {
    unsigned data = 0;
    data_vector_pop_first_many(vector, &data, 1);
    return data;
}

static void
data_vector_clear(struct data_vector *vector) {
    free(vector->data);
    vector->data = NULL;
    vector->size = 0;
    vector->capacity = 0;
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
// static void
// wakeup_queue_suspend_this(struct wakeup_queue *queue)
// {
// 	struct wakeup_entry entry;
// 	entry.coro = coro_this();
// 	rlist_add_tail_entry(&queue->coros, &entry, base);
// 	coro_suspend();
// 	rlist_del_entry(&entry, base);
// }

/** Wakeup the first coroutine in the queue. */
static void
wakeup_queue_wakeup_first(struct wakeup_queue *queue) {
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
coro_bus_errno(void) {
    return global_error;
}

void
coro_bus_errno_set(enum coro_bus_error_code err) {
    global_error = err;
}

struct coro_bus *
coro_bus_new(void) {
    struct coro_bus *bus = malloc(sizeof(struct coro_bus));
    bus->channels = NULL;
    bus->channel_count = 0;
    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return bus;
}

void
coro_bus_delete(struct coro_bus *bus) {
    for (int i = 0; i < bus->channel_count; ++i) {
        struct coro_bus_channel *ch = bus->channels[i];
        if (ch != NULL) {
            data_vector_clear(&ch->data);
            free(ch->data.data);
            free(ch);
        }
    }
    free(bus->channels);
    free(bus);
}

int
coro_bus_channel_open(struct coro_bus *bus, size_t size_limit) {
    struct coro_bus_channel *ch = malloc(sizeof(struct coro_bus_channel));
    ch->size_limit = size_limit;
    rlist_create(&ch->send_queue.coros);
    rlist_create(&ch->recv_queue.coros);
    ch->data.data = NULL;
    ch->data.size = 0;
    ch->data.capacity = 0;

    for (int i = 0; i < bus->channel_count; ++i) {
        if (bus->channels[i] == NULL) {
            bus->channels[i] = ch;
            coro_bus_errno_set(CORO_BUS_ERR_NONE);
            return i;
        }
    }

    int new_count = bus->channel_count + 1;
    bus->channels = realloc(bus->channels, new_count * sizeof(struct coro_bus_channel *));
    bus->channels[bus->channel_count] = ch;
    int desc = bus->channel_count;
    bus->channel_count = new_count;
    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return desc;
}

void
coro_bus_channel_close(struct coro_bus *bus, int channel) {
    if (channel < 0 || channel >= bus->channel_count)
        return;

    struct coro_bus_channel *channel_bus = bus->channels[channel];
    if (channel_bus == NULL)
        return;

    data_vector_clear(&channel_bus->data);
    free(channel_bus->data.data);
    channel_bus->data.data = NULL;

    struct rlist *current = channel_bus->send_queue.coros.next;
    while (current != &channel_bus->send_queue.coros) {
        struct rlist *next = current->next;
        struct wakeup_entry *entry = rlist_entry(current, struct wakeup_entry, base);
        coro_wakeup(entry->coro);
        rlist_del(current);
        current = next;
    }
    current = channel_bus->recv_queue.coros.next;
    while (current != &channel_bus->recv_queue.coros) {
        struct rlist *next = current->next;
        struct wakeup_entry *entry = rlist_entry(current, struct wakeup_entry, base);
        coro_wakeup(entry->coro);
        rlist_del(current);
        current = next;
    }

    free(channel_bus);
    bus->channels[channel] = NULL;
}

static struct coro_bus_channel *
get_channel(struct coro_bus *bus, int channel) {
    if (channel < 0 || channel >= bus->channel_count)
        return NULL;
    return bus->channels[channel];
}

int
coro_bus_try_send(struct coro_bus *bus, int channel, unsigned data) {
    struct coro_bus_channel *ch = get_channel(bus, channel);
    if (ch == NULL) {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return -1;
    }

    if (ch->data.size >= ch->size_limit) {
        coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
        return -1;
    }

    data_vector_append(&ch->data, data);

    if (!rlist_empty(&ch->recv_queue.coros)) {
        wakeup_queue_wakeup_first(&ch->recv_queue);
    }

    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return 0;
}

int
coro_bus_send(struct coro_bus *bus, int channel, unsigned data) {
    while (1) {
        int res = coro_bus_try_send(bus, channel, data);
        if (res == 0)
            return 0;

        enum coro_bus_error_code err = coro_bus_errno();
        if (err == CORO_BUS_ERR_NO_CHANNEL)
            return -1;

        struct coro_bus_channel *ch = get_channel(bus, channel);
        if (ch == NULL) {
            coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
            return -1;
        }

        struct wakeup_entry entry;
        entry.coro = coro_this();
        rlist_create(&entry.base);
        rlist_add_tail_entry(&ch->send_queue.coros, &entry, base);
        coro_suspend();
        rlist_del_entry(&entry, base);
    }
}

int
coro_bus_try_recv(struct coro_bus *bus, int channel, unsigned *data) {
    struct coro_bus_channel *ch = get_channel(bus, channel);
    if (ch == NULL) {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return -1;
    }

    size_t prev_size = ch->data.size;
    if (prev_size == 0) {
        coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
        return -1;
    }

    *data = data_vector_pop_first(&ch->data);

    if (prev_size == ch->size_limit && !rlist_empty(&ch->send_queue.coros)) {
        wakeup_queue_wakeup_first(&ch->send_queue);
    }

    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return 0;
}

int
coro_bus_recv(struct coro_bus *bus, int channel, unsigned *data) {
    while (1) {
        int res = coro_bus_try_recv(bus, channel, data);
        if (res == 0)
            return 0;

        enum coro_bus_error_code err = coro_bus_errno();
        if (err == CORO_BUS_ERR_NO_CHANNEL)
            return -1;

        struct coro_bus_channel *ch = get_channel(bus, channel);
        if (ch == NULL) {
            coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
            return -1;
        }

        struct wakeup_entry entry;
        entry.coro = coro_this();
        rlist_create(&entry.base);
        rlist_add_tail_entry(&ch->recv_queue.coros, &entry, base);
        coro_suspend();
        rlist_del_entry(&entry, base);
    }
}


int
coro_bus_try_broadcast(struct coro_bus *bus, unsigned data) {
    if (bus->channel_count == 0) {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return -1;
    }

    bool any_channel = false;
    for (int i = 0; i < bus->channel_count; ++i) {
        struct coro_bus_channel *ch = bus->channels[i];
        if (ch == NULL)
            continue;
        any_channel = true;
        if (ch->data.size >= ch->size_limit) {
            coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
            return -1;
        }
    }

    if (!any_channel) {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return -1;
    }

    for (int i = 0; i < bus->channel_count; ++i) {
        struct coro_bus_channel *ch = bus->channels[i];
        if (ch == NULL)
            continue;
        data_vector_append(&ch->data, data);
        if (!rlist_empty(&ch->recv_queue.coros)) {
            wakeup_queue_wakeup_first(&ch->recv_queue);
        }
    }

    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return 0;
}

int
coro_bus_broadcast(struct coro_bus *bus, unsigned data) {
    while (1) {
        int res = coro_bus_try_broadcast(bus, data);
        if (res == 0)
            return 0;

        enum coro_bus_error_code err = coro_bus_errno();
        if (err == CORO_BUS_ERR_NO_CHANNEL)
            return -1;
        if (err != CORO_BUS_ERR_WOULD_BLOCK)
            return -1;

        struct rlist entries;
        rlist_create(&entries);

        for (int i = 0; i < bus->channel_count; ++i) {
            struct coro_bus_channel *ch = bus->channels[i];
            if (ch == NULL)
                continue;
            if (ch->data.size >= ch->size_limit) {
                struct wakeup_entry *entry = malloc(sizeof(struct wakeup_entry));
                entry->coro = coro_this();
                rlist_add_tail_entry(&ch->send_queue.coros, entry, base);
                rlist_add_tail(&entries, &entry->base);
            }
        }

        if (rlist_empty(&entries)) {
            continue;
        }

        coro_suspend();

        struct rlist *current = entries.next;
        while (current != &entries) {
            struct rlist *next = current->next;
            struct wakeup_entry *entry = rlist_entry(current, struct wakeup_entry, base);

            for (int i = 0; i < bus->channel_count; ++i) {
                struct coro_bus_channel *ch = bus->channels[i];
                if (ch == NULL)
                    continue;

                struct rlist *node = ch->send_queue.coros.next;
                while (node != &ch->send_queue.coros) {
                    struct rlist *n = node->next;
                    if (node == &entry->base) {
                        rlist_del(node);
                        break;
                    }
                    node = n;
                }
            }
            free(entry);
            current = next;
        }
    }
}

#if NEED_BATCH

int
coro_bus_try_send_v(struct coro_bus *bus, int channel, const unsigned *data, unsigned count)
{
    struct coro_bus_channel *ch = get_channel(bus, channel);
    if (ch == NULL) {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return -1;
    }

    size_t available = ch->size_limit - ch->data.size;
    if (available == 0) {
        coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
        return -1;
    }

    unsigned to_send = count > available ? available : count;
    data_vector_append_many(&ch->data, data, to_send);

    for (unsigned i = 0; i < to_send; ++i) {
        if (rlist_empty(&ch->recv_queue.coros))
            break;
        wakeup_queue_wakeup_first(&ch->recv_queue);
    }

    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return to_send;
}

int
coro_bus_send_v(struct coro_bus *bus, int channel, const unsigned *data, unsigned count)
{
    unsigned sent = 0;
    while (sent < count) {
        int res = coro_bus_try_send_v(bus, channel, data + sent, count - sent);
        if (res > 0) {
            sent += res;
        } else {
            if (coro_bus_errno() != CORO_BUS_ERR_WOULD_BLOCK)
                return -1;

            struct coro_bus_channel *ch = get_channel(bus, channel);
            if (ch == NULL)
                return -1;

            struct wakeup_entry entry;
            entry.coro = coro_this();
            rlist_create(&entry.base);
            rlist_add_tail_entry(&ch->send_queue.coros, &entry, base);
            coro_suspend();
            rlist_del_entry(&entry, base);
        }
    }
    return sent;
}

#endif
