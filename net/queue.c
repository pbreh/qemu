/*
 * Copyright (c) 2003-2008 Fabrice Bellard
 * Copyright (c) 2009 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "net/queue.h"
#include "qemu/queue.h"
#include "qemu/timer.h"
#include "net/net.h"
#include "trace.h"

/* The delivery handler may only return zero if it will call
 * qemu_net_queue_flush() when it determines that it is once again able
 * to deliver packets. It must also call qemu_net_queue_purge() in its
 * cleanup path.
 *
 * If a sent callback is provided to send(), the caller must handle a
 * zero return from the delivery handler by not sending any more packets
 * until we have invoked the callback. Only in that case will we queue
 * the packet.
 *
 * If a sent callback isn't provided, we just drop the packet to avoid
 * unbounded queueing.
 */

struct NetPacket {
    QTAILQ_ENTRY(NetPacket) entry;
    NetClientState *sender;
    unsigned flags;
    int size;
    NetPacketSent *sent_cb;
    uint8_t data[0];
};

struct NetQueue {
    void *opaque;
    uint32_t nq_maxlen;
    uint32_t nq_count;

    QTAILQ_HEAD(packets, NetPacket) packets;

    unsigned delivering : 1;
};

static int64_t bandwidth_limit = 0 ;	/* maximum number of bits per second */

void qemu_net_set_bandwidth_limit (int64_t limit)
{
	bandwidth_limit = limit ;
	trace_qemu_net_set_bandwidth_limit (limit) ;
}


NetQueue *qemu_new_net_queue(void *opaque)
{
    NetQueue *queue;

    queue = g_malloc0(sizeof(NetQueue));

    queue->opaque = opaque;
    queue->nq_maxlen = 10000;
    queue->nq_count = 0;

    QTAILQ_INIT(&queue->packets);

    queue->delivering = 0;

    return queue;
}

void qemu_del_net_queue(NetQueue *queue)
{
    NetPacket *packet, *next;

    QTAILQ_FOREACH_SAFE(packet, &queue->packets, entry, next) {
        QTAILQ_REMOVE(&queue->packets, packet, entry);
        g_free(packet);
    }

    g_free(queue);
}

static void qemu_net_queue_append(NetQueue *queue,
                                  NetClientState *sender,
                                  unsigned flags,
                                  const uint8_t *buf,
                                  size_t size,
                                  NetPacketSent *sent_cb)
{
    NetPacket *packet;

    if (queue->nq_count >= queue->nq_maxlen && !sent_cb) {
        return; /* drop if queue full and no callback */
    }
    packet = g_malloc(sizeof(NetPacket) + size);
    packet->sender = sender;
    packet->flags = flags;
    packet->size = size;
    packet->sent_cb = sent_cb;
    memcpy(packet->data, buf, size);

    queue->nq_count++;
    QTAILQ_INSERT_TAIL(&queue->packets, packet, entry);
}

static void qemu_net_queue_append_iov(NetQueue *queue,
                                      NetClientState *sender,
                                      unsigned flags,
                                      const struct iovec *iov,
                                      int iovcnt,
                                      NetPacketSent *sent_cb)
{
    NetPacket *packet;
    size_t max_len = 0;
    int i;

    if (queue->nq_count >= queue->nq_maxlen && !sent_cb) {
        return; /* drop if queue full and no callback */
    }
    for (i = 0; i < iovcnt; i++) {
        max_len += iov[i].iov_len;
    }

    packet = g_malloc(sizeof(NetPacket) + max_len);
    packet->sender = sender;
    packet->sent_cb = sent_cb;
    packet->flags = flags;
    packet->size = 0;

    for (i = 0; i < iovcnt; i++) {
        size_t len = iov[i].iov_len;

        memcpy(packet->data + packet->size, iov[i].iov_base, len);
        packet->size += len;
    }

    queue->nq_count++;
    QTAILQ_INSERT_TAIL(&queue->packets, packet, entry);
}

static ssize_t qemu_net_queue_deliver(NetQueue *queue,
                                      NetClientState *sender,
                                      unsigned flags,
                                      const uint8_t *data,
                                      size_t size)
{
    ssize_t ret = -1;

    queue->delivering = 1;
    ret = qemu_deliver_packet(sender, flags, data, size, queue->opaque);
    queue->delivering = 0;

    return ret;
}

static ssize_t qemu_net_queue_deliver_iov(NetQueue *queue,
                                          NetClientState *sender,
                                          unsigned flags,
                                          const struct iovec *iov,
                                          int iovcnt)
{
    ssize_t ret = -1;

    queue->delivering = 1;
    ret = qemu_deliver_packet_iov(sender, flags, iov, iovcnt, queue->opaque);
    queue->delivering = 0;

    return ret;
}

static int64_t limit_network_performance (int64_t start_clock,
										  int64_t bytes)
{
	int64_t clock = get_clock() ;
	int64_t sleep_usecs = 0 ;
	if (bandwidth_limit > 0)
		sleep_usecs = (bytes * 8 * 1000000LL) / bandwidth_limit - (clock - start_clock) / 1000LL ;
	if (sleep_usecs > 0) {
		usleep (sleep_usecs) ;
		clock = get_clock() ;
	}

	return clock ;
}

static void log_and_limit_network_performance (size_t size)
{
	/*
	 * Performance logging isn't specified yet.
	 * Therefore we're using existing tracing.
	 */
	static int64_t logged_clock = 0 ;
	static int64_t packets = 0 ;
	static int64_t bytes = 0 ;
	int64_t clock = 0 ;

	packets++ ;
	bytes = bytes + size ;
	clock = limit_network_performance (logged_clock, bytes) ;
	if (clock - logged_clock >= 1000000000LL) {
		if (logged_clock > 0) /* don't log first event */
			trace_log_network_performance (packets, bytes*8, (clock - logged_clock) / 1000000000LL) ;
		packets = 0 ;
		bytes = 0 ;
		logged_clock = clock ;
	}
}

ssize_t qemu_net_queue_send(NetQueue *queue,
                            NetClientState *sender,
                            unsigned flags,
                            const uint8_t *data,
                            size_t size,
                            NetPacketSent *sent_cb)
{
    ssize_t ret;

    log_and_limit_network_performance (size) ;
    if (queue->delivering || !qemu_can_send_packet(sender)) {
        qemu_net_queue_append(queue, sender, flags, data, size, sent_cb);
        return 0;
    }

    ret = qemu_net_queue_deliver(queue, sender, flags, data, size);
    if (ret == 0) {
        qemu_net_queue_append(queue, sender, flags, data, size, sent_cb);
        return 0;
    }

    qemu_net_queue_flush(queue);

    return ret;
}

ssize_t qemu_net_queue_send_iov(NetQueue *queue,
                                NetClientState *sender,
                                unsigned flags,
                                const struct iovec *iov,
                                int iovcnt,
                                NetPacketSent *sent_cb)
{
    ssize_t ret;

    if (queue->delivering || !qemu_can_send_packet(sender)) {
        qemu_net_queue_append_iov(queue, sender, flags, iov, iovcnt, sent_cb);
        return 0;
    }

    ret = qemu_net_queue_deliver_iov(queue, sender, flags, iov, iovcnt);
    if (ret == 0) {
        qemu_net_queue_append_iov(queue, sender, flags, iov, iovcnt, sent_cb);
        return 0;
    }

    qemu_net_queue_flush(queue);

    return ret;
}

void qemu_net_queue_purge(NetQueue *queue, NetClientState *from)
{
    NetPacket *packet, *next;

    QTAILQ_FOREACH_SAFE(packet, &queue->packets, entry, next) {
        if (packet->sender == from) {
            QTAILQ_REMOVE(&queue->packets, packet, entry);
            queue->nq_count--;
            if (packet->sent_cb) {
                packet->sent_cb(packet->sender, 0);
            }
            g_free(packet);
        }
    }
}

bool qemu_net_queue_flush(NetQueue *queue)
{
    while (!QTAILQ_EMPTY(&queue->packets)) {
        NetPacket *packet;
        int ret;

        packet = QTAILQ_FIRST(&queue->packets);
        QTAILQ_REMOVE(&queue->packets, packet, entry);
        queue->nq_count--;

        ret = qemu_net_queue_deliver(queue,
                                     packet->sender,
                                     packet->flags,
                                     packet->data,
                                     packet->size);
        if (ret == 0) {
            queue->nq_count++;
            QTAILQ_INSERT_HEAD(&queue->packets, packet, entry);
            return false;
        }

        if (packet->sent_cb) {
            packet->sent_cb(packet->sender, ret);
        }

        g_free(packet);
    }
    return true;
}
