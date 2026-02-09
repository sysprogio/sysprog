#pragma once

#include <stddef.h>

/**
 * Here you should specify which bonuses do you want via the
 * macros. It is important to define these macros here, in the
 * header, because it is used by tests.
 */
#define NEED_BROADCAST 1
#define NEED_BATCH 1

enum coro_bus_error_code {
	CORO_BUS_ERR_NONE = 0,
	CORO_BUS_ERR_NO_CHANNEL,
	CORO_BUS_ERR_WOULD_BLOCK,
	CORO_BUS_ERR_NOT_IMPLEMENTED,
};

struct coro_bus;

/** Get the latest error happened in coro_bus. */
enum coro_bus_error_code
coro_bus_errno(void);

/** Set the global coro_bus error. */
void
coro_bus_errno_set(enum coro_bus_error_code err);

/** Create a new messaging bus with no channels in it. */
struct coro_bus *
coro_bus_new(void);

/**
 * Destroy the bus and all its channels. The channels can not have
 * any suspended coroutines, but might have unconsumed data which
 * should be deleted too.
 */
void
coro_bus_delete(struct coro_bus *bus);

/**
 * Create a channel inside the bus.
 * @param bus The bus to create the channel in.
 * @param size_limit Maximum messages a channel can hold in memory
 *     at once.
 *
 * @retval >=0 Descriptor of the channel. It must be passed to the
 *     send/recv functions.
 */
int
coro_bus_channel_open(struct coro_bus *bus, size_t size_limit);

/**
 * Destroy the channel identified by the given descriptor. The
 * channel must exist. All pending messages of the channel are
 * deleted and lost. All the coroutines suspended on this channel
 * are woken up and get the error that the channel is missing.
 * @param bus Bus to destroy the channel in.
 * @param channel Descriptor of the channel to destroy.
 */
void
coro_bus_channel_close(struct coro_bus *bus, int channel);

/**
 * Send the given message to the specified channel. If the channel
 * is full, the function should suspend the current coroutine and
 * retry until success or until the channel is gone.
 * @param bus Bus where the channel is located.
 * @param channel Descriptor of the channel to send data to.
 * @param data Data to send.
 *
 * @retval 0 Success.
 * @retval -1 Error. Check coro_bus_errno() for reason.
 *     - CORO_BUS_ERR_NO_CHANNEL - the channel doesn't exist.
 */
int
coro_bus_send(struct coro_bus *bus, int channel, unsigned data);

/**
 * Same as coro_bus_send(), but if the channel is full, the
 * function immediately returns. It never suspends the current
 * coroutine.
 * @param bus Bus where the channel is located.
 * @param channel Descriptor of the channel to send data to.
 * @param data Data to send.
 *
 * @retval 0 Success.
 * @retval -1 Error. Check coro_bus_errno() for reason.
 *     - CORO_BUS_ERR_NO_CHANNEL - the channel doesn't exist.
 *     - CORO_BUS_ERR_WOULD_BLOCK - the channel is full.
 */
int
coro_bus_try_send(struct coro_bus *bus, int channel, unsigned data);

/**
 * Recv a message from the specified channel. If the channel is
 * empty, the function should suspend the current coroutine and
 * retry until success or until the channel is gone.
 * @param bus Bus where the channel is located.
 * @param channel Descriptor of the channel to send data to.
 * @param data Output parameter to save the data to.
 *
 * @retval 0 Success. Data output is filled with the received
 *     message.
 * @retval -1 Error. Check coro_bus_errno() for reason.
 *     - CORO_BUS_ERR_NO_CHANNEL - the channel doesn't exist.
 */
int
coro_bus_recv(struct coro_bus *bus, int channel, unsigned *data);

/**
 * Same as coro_bus_recv(), but if the channel is empty, the
 * function immediately returns. It never suspends the current
 * coroutine.
 * @param bus Bus where the channel is located.
 * @param channel Descriptor of the channel to send data to.
 * @param data Output parameter to save the data to.
 *
 * @retval 0 Success. Data output is filled with the received
 *     message.
 * @retval -1 Error. Check coro_bus_errno() for reason.
 *     - CORO_BUS_ERR_NO_CHANNEL - the channel doesn't exist.
 *     - CORO_BUS_ERR_WOULD_BLOCK - the channel is empty.
 */
int
coro_bus_try_recv(struct coro_bus *bus, int channel, unsigned *data);


#if NEED_BROADCAST /* Bonus 1 */

/**
 * Send the given message to all the registered channels at once.
 * If any of the channels are full, then the message isn't sent
 * anywhere, and the coroutine is suspended until can submit the
 * data to all the channels.
 * @param bus Bus where the channels are located.
 * @param data Data to send.
 *
 * @retval 0 Success. Sent to all the channels.
 * @retval -1 Error. Check coro_bus_errno() for reason.
 *     - CORO_BUS_ERR_NO_CHANNEL - no channels in the bus.
 */
int
coro_bus_broadcast(struct coro_bus *bus, unsigned data);

/**
 * Same as coro_bus_broadcast(), but if any of the channels are
 * full, it instantly returns, not suspends.
 * @param bus  Bus where the channels are located.
 * @param data Data to send.
 *
 * @retval 0 Success. Sent to all the channels.
 * @retval -1 Error. Check coro_bus_errno() for reason.
 *     - CORO_BUS_ERR_NO_CHANNEL - no channels in the bus.
 *     - CORO_BUS_ERR_WOULD_BLOCK - at least one channel is full.
 */
int
coro_bus_try_broadcast(struct coro_bus *bus, unsigned data);

#endif /* Bonus 1 */

#if NEED_BATCH /* Bonus 2 */

/**
 * Same as coro_bus_send(), but can submit multiple messages at
 * once. If the channel is full, then the coroutine is suspended
 * until the channel has space. When there is space, the function
 * submits as many messages as the channel fits, and returns how
 * many was sent.
 * @param bus Bus where the channel is located.
 * @param channel Descriptor of the channel to send data to.
 * @param data Array of messages to send.
 * @param count Size of @a data.
 *
 * @retval >0 Success, how many messages were sent. They are sent
 *     in the order of being in @a data. For example, if 3
 *     messages are sent, they are guaranteed data[0-2].
 * @retval -1 Error. Check coro_bus_errno() for reason.
 *     - CORO_BUS_ERR_NO_CHANNEL - the channel doesn't exist.
 */
int
coro_bus_send_v(struct coro_bus *bus, int channel,
	const unsigned *data, unsigned count);

/**
 * Same as coro_bus_send_v(), but fails instantly in case the
 * channel is full and doesn't fit a single message.
 * @param bus Bus where the channel is located.
 * @param channel Descriptor of the channel to send data to.
 * @param data Array of messages to send.
 * @param count Size of @a data.
 *
 * @retval >0 Success, how many messages were sent. They are sent
 *     in the order of being in @a data. For example, if 3
 *     messages are sent, they are guaranteed data[0-2].
 * @retval -1 Error. Check coro_bus_errno() for reason.
 *     - CORO_BUS_ERR_NO_CHANNEL - the channel doesn't exist.
 *     - CORO_BUS_ERR_WOULD_BLOCK - the channel is full.
 */
int
coro_bus_try_send_v(struct coro_bus *bus, int channel,
	const unsigned *data, unsigned count);

/**
 * Same as coro_bus_recv(), but can receive multiple messages at
 * once. If the channel is empty, then the coroutine is suspended
 * until there are messages in the channel. When messages found,
 * the function receives as many of them as can, and returns how
 * many.
 * @param bus Bus where the channel is located.
 * @param channel Descriptor of the channel to recv data from.
 * @param data Array to save the received messages into.
 * @param count Capacity of @a data.
 *
 * @retval >0 Success, how many messages were received. They are
 *     saved into @a data in the order of receiving. For example,
 *     if 3 messages are received, they are guaranteed stored in
 *     data[0-2].
 * @retval -1 Error. Check coro_bus_errno() for reason.
 *     - CORO_BUS_ERR_NO_CHANNEL - the channel doesn't exist.
 */
int
coro_bus_recv_v(struct coro_bus *bus, int channel,
	unsigned *data, unsigned capacity);

/**
 * Same as coro_bus_recv_v(), but fails instantly if the channel
 * is empty.
 * @param bus Bus where the channel is located.
 * @param channel Descriptor of the channel to recv data from.
 * @param data Array to save the received messages into.
 * @param count Capacity of @a data.
 *
 * @retval >0 Success, how many messages were received. They are
 *     saved into @a data in the order of receiving. For example,
 *     if 3 messages are received, they are guaranteed stored in
 *     data[0-2].
 * @retval -1 Error. Check coro_bus_errno() for reason.
 *     - CORO_BUS_ERR_NO_CHANNEL - the channel doesn't exist.
 *     - CORO_BUS_ERR_WOULD_BLOCK - the channel is empty.
 */
int
coro_bus_try_recv_v(struct coro_bus *bus, int channel,
	unsigned *data, unsigned capacity);

#endif /* Bonus 2 */
