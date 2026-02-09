#include "corobus.h"

#include "libcoro.h"
#include "rlist.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <deque>

#define MAX_CHANNEL_COUNT_SIZE 128

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

static void
wakeup_queue_init(struct wakeup_queue *queue)
{
	rlist_create(&queue->coros);
}

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


struct coro_bus_channel {
	/** Channel max capacity. */
	size_t size_limit;
	/** Coroutines waiting until the channel is not full. */
	struct wakeup_queue send_queue;
	/** Coroutines waiting until the channel is not empty. */
	struct wakeup_queue recv_queue;
	/** Message queue. */
	std::deque<unsigned> data;
};

static struct coro_bus_channel*
coro_bus_channel_create(size_t size_limit)
{
    struct coro_bus_channel *channel = new coro_bus_channel();

    channel->size_limit = size_limit;
    wakeup_queue_init(&channel->send_queue);
    wakeup_queue_init(&channel->recv_queue);

    return channel;
}

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

struct coro_bus *
coro_bus_new(void)
{
	struct coro_bus *bus = new coro_bus();
    bus->channel_count = MAX_CHANNEL_COUNT_SIZE;
	bus->channels = new coro_bus_channel*[bus->channel_count];

	memset(bus->channels, 0, bus->channel_count * sizeof(coro_bus_channel*));
    return bus;
}

void
coro_bus_delete(struct coro_bus *bus)
{
	for (int i = 0; i < bus->channel_count; i++) {
		delete bus->channels[i];
	}
	delete[] bus->channels;
	delete bus;
}

int
coro_bus_channel_open(struct coro_bus *bus, size_t size_limit)
{
	/*
	 * One of the tests will force you to reuse the channel
	 * descriptors. It means, that if your maximal channel
	 * descriptor is N, and you have any free descriptor in
	 * the range 0-N, then you should open the new channel on
	 * that old descriptor.
	 *
	 * A more precise instruction - check if any of the
	 * bus->channels[i] with i = 0 -> bus->channel_count is
	 * free (== NULL). If yes - reuse the slot. Don't grow the
	 * bus->channels array, when have space in it.
	 */
	for (int i = 0; i < bus->channel_count; i++) {
		if (bus->channels[i] == NULL) {
			bus->channels[i] = coro_bus_channel_create(size_limit);
			return i;
		}
	}
	// assert(false);
	return -1;
}

void
coro_bus_channel_close(struct coro_bus *bus, int channel)
{
	/*
	 * Be very attentive here. What happens, if the channel is
	 * closed while there are coroutines waiting on it? For
	 * example, the channel was empty, and some coros were
	 * waiting on its recv_queue.
	 *
	 * If you wakeup those coroutines and just delete the
	 * channel right away, then those waiting coroutines might
	 * on wakeup try to reference invalid memory.
	 *
	 * Can happen, for example, if you use an intrusive list
	 * (rlist), delete the list itself (by deleting the
	 * channel), and then the coroutines on wakeup would try
	 * to remove themselves from the already destroyed list.
	 *
	 * Think how you could address that. Remove all the
	 * waiters from the list before freeing it? Yield this
	 * coroutine after waking up the waiters but before
	 * freeing the channel, so the waiters could safely leave?
	 */

	struct coro_bus_channel *ch = bus->channels[channel];
	assert(ch != NULL);

	bus->channels[channel] = NULL;

	struct wakeup_entry *entry;
	rlist_foreach_entry(entry, &ch->recv_queue.coros, base) {
		coro_wakeup(entry->coro);
	}
	entry = NULL;
	rlist_foreach_entry(entry, &ch->send_queue.coros, base) {
		coro_wakeup(entry->coro);
	}
	coro_yield();

	delete ch;
}

static int
coro_bus_channel_send_general(struct coro_bus *bus, int channel, unsigned data, bool blocking)
{
    for (;;) {
        struct coro_bus_channel *ch = bus->channels[channel];
        if (ch == NULL) {
            coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
            return -1;
        }

        if (ch->data.size() < ch->size_limit) {
            ch->data.push_back(data);
            wakeup_queue_wakeup_first(&ch->recv_queue);
            return 0;
        }

        if (!blocking) {
            coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
            return -1;
        }

        wakeup_queue_suspend_this(&ch->send_queue);
    }
}

int
coro_bus_send(struct coro_bus *bus, int channel, unsigned data)
{
	/*
	 * Try sending in a loop, until success. If error, then
	 * check which one is that. If 'wouldblock', then suspend
	 * this coroutine and try again when woken up.
	 *
	 * If see the channel has space, then wakeup the first
	 * coro in the send-queue. That is needed so when there is
	 * enough space for many messages, and many coroutines are
	 * waiting, they would then wake each other up one by one
	 * as lone as there is still space.
	 */
	return coro_bus_channel_send_general(bus, channel, data, true);
}

int
coro_bus_try_send(struct coro_bus *bus, int channel, unsigned data)
{
	/*
	 * Append data if has space. Otherwise 'wouldblock' error.
	 * Wakeup the first coro in the recv-queue! To let it know
	 * there is data.
	 */
	return coro_bus_channel_send_general(bus, channel, data, false);
}

static int
coro_bus_channel_recv_internal(struct coro_bus *bus, int channel, unsigned *out, bool blocking)
{
    assert(out != NULL);

    for (;;) {
        struct coro_bus_channel *ch = bus->channels[channel];
        if (ch == NULL) {
            coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
            return -1;
        }

        if (!ch->data.empty()) {
            *out = ch->data.front();
            ch->data.pop_front();
            wakeup_queue_wakeup_first(&ch->send_queue);
            return 0;
        }

        if (!blocking) {
            coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
            return -1;
        }

        wakeup_queue_suspend_this(&ch->recv_queue);
    }
}

int
coro_bus_recv(struct coro_bus *bus, int channel, unsigned *data)
{
	return coro_bus_channel_recv_internal(bus, channel, data, true);
}

int
coro_bus_try_recv(struct coro_bus *bus, int channel, unsigned *data)
{
	return coro_bus_channel_recv_internal(bus, channel, data, false);
}


#if NEED_BROADCAST

static void
coro_bus_broadcast_unsafe(struct coro_bus *bus, unsigned data)
{
	for (int i = 0; i < bus->channel_count; i++) {
		struct coro_bus_channel *ch = bus->channels[i];
		if (ch == NULL) {
			continue;
		}

		assert(ch->data.size() < ch->size_limit);

		ch->data.push_back(data);
		wakeup_queue_wakeup_first(&ch->recv_queue);
	}
}

/**
 * Check if all channels in the bus are available for broadcast.
 *
 * @param bus Pointer to the coro_bus structure to check.
 * @param out_first_full Pointer to store the first full channel pointer.
 *                       Can be NULL if caller doesn't need the channel pointer.
 *                       If a full channel is found, it will be dereferenced here.
 *
 * @return 0 if all open channels have space available (broadcast can proceed)
 *        -1 if broadcast cannot proceed. Sets coro_bus_errno to:
 *           - CORO_BUS_ERR_NO_CHANNEL: if no channels are open
 *           - CORO_BUS_ERR_WOULD_BLOCK: if at least one channel is full
 *                                       (out_first_full will contain that channel)
 */
static int
coro_bus_check_broadcast_availability(struct coro_bus *bus, struct coro_bus_channel **out_first_full)
{
    bool has_open_channels = false;
    
    for (int i = 0; i < bus->channel_count; i++) {
        struct coro_bus_channel *ch = bus->channels[i];
        if (ch == NULL) {
            continue;
        }
        has_open_channels = true;
        
        if (ch->data.size() >= ch->size_limit) {
            if (out_first_full != NULL) {
                *out_first_full = ch;
            }
            coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
            return -1;
        }
    }
    
    if (!has_open_channels) {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return -1;
    }
    
    return 0;
}

int
coro_bus_broadcast(struct coro_bus *bus, unsigned data)
{	
	for (;;) {

		struct coro_bus_channel *first_full;
		int ret = coro_bus_check_broadcast_availability(bus, &first_full);
		if (ret == 0) {
			// All channels are available, can broadcast.
			break;
		}

		if (coro_bus_errno() == CORO_BUS_ERR_NO_CHANNEL) {
			return -1;
		}
		assert(coro_bus_errno() == CORO_BUS_ERR_WOULD_BLOCK);
		assert(first_full != NULL);
		wakeup_queue_suspend_this(&first_full->send_queue);
	}

	coro_bus_broadcast_unsafe(bus, data);
	return 0;
}

int
coro_bus_try_broadcast(struct coro_bus *bus, unsigned data)
{
	int ret = coro_bus_check_broadcast_availability(bus, NULL);
	if (ret < 0) {
		assert(
			coro_bus_errno() == CORO_BUS_ERR_WOULD_BLOCK || 
			coro_bus_errno() == CORO_BUS_ERR_NO_CHANNEL
		);
		return -1;
	}
	
	coro_bus_broadcast_unsafe(bus, data);
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
