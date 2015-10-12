/*
   drbd_transport_rdma.c

   This file is part of DRBD.

   Copyright (C) 2014, LINBIT HA-Solutions GmbH.

   drbd is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   drbd is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with drbd; see the file COPYING.  If not, write to
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#undef pr_fmt
#define pr_fmt(fmt)	"drbd_rdma: " fmt

#ifndef SENDER_COMPACTS_BVECS
/* My benchmarking shows a limit of 30 MB/s
 * with the current implementation of this idea.
 * cpu bound, perf top shows mainly get_page/put_page.
 * Without this, using the plain send_page,
 * I achieve > 400 MB/s on the same system.
 * => disable for now, improve later.
 */
#define SENDER_COMPACTS_BVECS 0
#endif

#include <linux/module.h>
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

#include <linux/drbd_genl_api.h>
#include <drbd_protocol.h>
#include <drbd_transport.h>
#include <drbd_wrappers.h>


/* Nearly all data transfer uses the send/receive semantics. No need to
   actually use RDMA WRITE / READ.

   Only for DRBD's remote read (P_DATA_REQUEST and P_DATA_REPLY) a
   RDMA WRITE would make a lot of sense:
     Right now the recv_dless_read() function in DRBD is one of the few
     remaining callers of recv(,,CALLER_BUFFER). This in turn needs a
     memcpy().

   The block_id field (64 bit) could be re-labelled to be the RKEY for
   an RDMA WRITE. The P_DATA_REPLY packet will then only deliver the
   news that the RDMA WRITE was executed...


   Flow Control
   ============

   If the receiving machine can not keep up with the data rate it needs to
   slow down the sending machine. In order to do so we keep track of the
   number of rx_descs the peer has posted (peer_rx_descs).

   If one player posts new rx_descs it tells the peer about it with a
   dtr_flow_control packet. Those packet get never delivered to the
   DRBD above us.
*/

MODULE_AUTHOR("Roland Kammerer <roland.kammerer@linbit.com>");
MODULE_AUTHOR("Philipp Reisner <philipp.reisner@linbit.com>");
MODULE_AUTHOR("Lars Ellenberg <lars.ellenberg@linbit.com>");
MODULE_DESCRIPTION("RDMA transport layer for DRBD");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0.0");

int allocation_size;
/* module_param(allocation_size, int, 0664);
   MODULE_PARM_DESC(allocation_size, "Allocation size for receive buffers (page size of peer)");

   That needs to be implemented in dtr_create_rx_desc() and in dtr_recv() and dtr_recv_pages() */

/* If no recvbuf_size or sendbuf_size is configured use 1MiByte + 3 pages the DATA_STREAM */
/* Actually it is not a buffer, but the number of tx_descs or rx_descs we allow,
   very comparable to the socket sendbuf and recvbuf sizes */
/* Right now refilling the peer_rx_descs only works while the receiver on both sides tries
   to receive something, better make sure a complete BIO always fits in.
   Probably a better approach would be to do the receiving actually in the callback
   dtr_rx_cq_event_handler(), then we would always get flowcontrol messages in in a timely
   manner */ /* Update! Early processing of flowcontrol messages is implemented! */
#define RDMA_DEF_BUFFER_SIZE (2 * DRBD_MAX_BIO_SIZE + (3 * DRBD_SOCKET_BUFFER_SIZE))

/* If we can send less than 8 packets, we consider the transport as congested. */
#define DESCS_LOW_LEVEL 8

/* Assuming that a singe 4k write should be at the highest scatterd over 8
   pages. I.e. has no parts smaller than 512 bytes.
   Arbitrary assumption. It seems that Mellanox hardware can do up to 29
   ppc64 page size might be 64k */
#if (PAGE_SIZE / 512) > 28
# define DTR_MAX_TX_SGES 28
#else
# define DTR_MAX_TX_SGES (PAGE_SIZE / 512)
#endif

#define DTR_MAGIC ((u32)0x5257494E)

struct dtr_flow_control {
	uint32_t magic;
	uint32_t new_rx_descs[2];
} __packed;

/* These numbers are sent within the immediate data value to identify
   if the packet is a data, and control or a (transport private) flow_control
   message */
enum dtr_stream_nr {
	ST_DATA = DATA_STREAM,
	ST_CONTROL = CONTROL_STREAM,
	ST_FLOW_CTRL
};

/* IB_WR_SEND_WITH_IMM and IB_WR_RDMA_WRITE_WITH_IMM

   both transfer user data and a 32bit value with is delivered at the receiving
   to the event handler of the completion queue. I.e. that can be used to queue
   the incoming messages to different streams.

   dtr_imm:
   In order to support folding the data and the control stream into one RDMA
   connection we use the stream field of dtr_imm: DATA_STREAM, CONTROL_STREAM
   and FLOW_CONTROL.
   To be able to order the messages on the receiving side before delivering them
   to the upper layers we use a sequence number.

   */
#define SEQUENCE_BITS 30
union dtr_immediate {
	struct {
#if defined(__LITTLE_ENDIAN_BITFIELD)
		unsigned int sequence:SEQUENCE_BITS;
		unsigned int stream:2;
#elif defined(__BIG_ENDIAN_BITFIELD)
		unsigned int stream:2;
		unsigned int sequence:SEQUENCE_BITS;
#else
# error "this endianness is not supported"
#endif
	};
	unsigned int i;
};


enum drbd_rdma_state {
	IDLE = 1,
	CONNECT_REQUEST,
	ADDR_RESOLVED,
	ROUTE_RESOLVED,
	CONNECTED,
	DISCONNECTED,
	ERROR
};

struct drbd_rdma_rx_desc {
	struct page *page;
	struct list_head list;
	int size;
	unsigned int sequence;
	struct dtr_path *path;
	struct ib_sge sge;
};

struct drbd_rdma_tx_desc {
	union {
		struct page *page;
		void *data;
		struct bio *bio;
	};
	enum {
		SEND_PAGE,
		SEND_MSG,
		SEND_BIO,
	} type;
	int nr_sges;
	enum dtr_stream_nr stream_nr;
	struct ib_sge sge[0]; /* must be last! */
};

struct dtr_cm {
	struct rdma_cm_id *id;
	enum drbd_rdma_state state;
	wait_queue_head_t state_wq;
};

enum {
	P_CONNECT_WORK =   1 << 0,
	P_CANCEL_CONNECT = 1 << 1,
};

struct dtr_flow {
	struct dtr_path *path;

	atomic_t tx_descs_posted;
	int tx_descs_max; /* derived from net_conf->sndbuf_size. Do not change after alloc. */
	atomic_t peer_rx_descs; /* peer's receive window in number of rx descs */

	int rx_descs_posted;
	int rx_descs_max;  /* derived from net_conf->rcvbuf_size. Do not change after alloc. */

	int rx_descs_allocated;  // keep in stream??
	int rx_descs_want_posted;
	atomic_t rx_descs_known_to_peer;
};

struct dtr_path {
	struct drbd_path path;

	struct work_struct connect_work;

	struct dtr_cm *cm;

	struct ib_cq *recv_cq;
	struct ib_cq *send_cq;
	struct ib_pd *pd;
	struct ib_qp *qp;

	struct ib_mr *dma_mr;

	struct drbd_rdma_transport *rdma_transport;
	unsigned long flags;
	struct dtr_flow flow[2];
	wait_queue_head_t wq;
};

struct dtr_stream {
	wait_queue_head_t send_wq;
	wait_queue_head_t recv_wq;

	/* for recv() to keep track of the current rx_desc:
	 * - whenever the bytes_left of the current rx_desc == 0, we know that all data
	 *   is consumed, and get a new rx_desc from the completion queue, and set
	 *   current rx_desc accodingly.
	 */
	struct {
		struct drbd_rdma_rx_desc *desc;
		void *pos;
		int bytes_left;
	} current_rx;

	struct list_head rx_descs;
	spinlock_t rx_descs_lock;

	long send_timeout;
	long recv_timeout;

	char name[8]; /* "control" or "data" */
	unsigned int tx_sequence;
	unsigned int rx_sequence;
	struct drbd_rdma_transport *rdma_transport;
};

struct drbd_rdma_transport {
	struct drbd_transport transport;
	struct dtr_stream stream[2];
	int rx_allocation_size;
	bool active; /* connect() returned no error. I.e. C_CONNECTING or C_CONNECTED */

	atomic_t first_path_connect_err;
	struct completion connected;
};

struct dtr_listener {
	struct drbd_listener listener;

	struct dtr_cm cm;
	struct rdma_cm_id *child_cms; /* Single linked list on the context member */
};

struct dtr_waiter {
	struct drbd_waiter waiter;

	struct dtr_cm *cm; /* to pass a path between waiters... */
};

static int stream_nr = 0; /* debugging */

static int dtr_init(struct drbd_transport *transport);
static void dtr_free(struct drbd_transport *transport, enum drbd_tr_free_op);
static int dtr_connect(struct drbd_transport *transport);
static int dtr_recv(struct drbd_transport *transport, enum drbd_stream stream, void **buf, size_t size, int flags);
static void dtr_stats(struct drbd_transport *transport, struct drbd_transport_stats *stats);
static void dtr_set_rcvtimeo(struct drbd_transport *transport, enum drbd_stream stream, long timeout);
static long dtr_get_rcvtimeo(struct drbd_transport *transport, enum drbd_stream stream);
static int dtr_send_page(struct drbd_transport *transport, enum drbd_stream stream, struct page *page,
		int offset, size_t size, unsigned msg_flags);
static int dtr_send_zc_bio(struct drbd_transport *, struct bio *bio);
static int dtr_recv_pages(struct drbd_transport *transport, struct drbd_page_chain_head *chain, size_t size);
static bool dtr_stream_ok(struct drbd_transport *transport, enum drbd_stream stream);
static bool dtr_hint(struct drbd_transport *transport, enum drbd_stream stream, enum drbd_tr_hints hint);
static void dtr_debugfs_show(struct drbd_transport *, struct seq_file *m);
static int dtr_add_path(struct drbd_transport *, struct drbd_path *path);
static int dtr_remove_path(struct drbd_transport *, struct drbd_path *path);

static bool dtr_path_ok(struct dtr_path *path);
static bool dtr_transport_ok(struct drbd_transport *transport);
static int __dtr_post_tx_desc(struct dtr_path *, struct drbd_rdma_tx_desc *);
static int dtr_post_tx_desc(struct drbd_rdma_transport *, struct drbd_rdma_tx_desc *);
static int dtr_repost_tx_desc(struct drbd_rdma_transport *, struct drbd_rdma_tx_desc *);
static void dtr_repost_rx_desc(struct dtr_path *path, struct drbd_rdma_rx_desc *rx_desc);
static bool dtr_receive_rx_desc(struct dtr_stream *, struct drbd_rdma_rx_desc **);
static void dtr_recycle_rx_desc(struct drbd_transport *transport,
				enum drbd_stream stream,
				struct drbd_rdma_rx_desc **pp_rx_desc);
static void dtr_refill_rx_desc(struct drbd_rdma_transport *rdma_transport,
			       enum drbd_stream stream);
static void dtr_free_rx_desc(struct dtr_path *path, struct drbd_rdma_rx_desc *rx_desc);
static void dtr_disconnect_path(struct dtr_path *path);
static void dtr_uninit_path(struct dtr_path *path);

static struct drbd_transport_class rdma_transport_class = {
	.name = "rdma",
	.instance_size = sizeof(struct drbd_rdma_transport),
	.path_instance_size = sizeof(struct dtr_path),
	.module = THIS_MODULE,
	.init = dtr_init,
	.list = LIST_HEAD_INIT(rdma_transport_class.list),
};

static struct drbd_transport_ops dtr_ops = {
	.free = dtr_free,
	.connect = dtr_connect,
	.recv = dtr_recv,
	.stats = dtr_stats,
	.set_rcvtimeo = dtr_set_rcvtimeo,
	.get_rcvtimeo = dtr_get_rcvtimeo,
	.send_page = dtr_send_page,
	.send_zc_bio = dtr_send_zc_bio,
	.recv_pages = dtr_recv_pages,
	.stream_ok = dtr_stream_ok,
	.hint = dtr_hint,
	.debugfs_show = dtr_debugfs_show,
	.add_path = dtr_add_path,
	.remove_path = dtr_remove_path,
};

static struct workqueue_struct *dtr_work_queue;


static struct drbd_path* dtr_drbd_path(struct drbd_transport *transport)
{
	return list_first_entry_or_null(&transport->paths, struct drbd_path, list);
}

static struct dtr_path *dtr_path(struct drbd_rdma_transport *rdma_transport)
{
	struct drbd_path *path = dtr_drbd_path(&rdma_transport->transport);

	/* path might be NULL. Using container_of on that is safe since
	   the offset is 0. I.e. the NULL pointer will continue to be NULL */
	return container_of(path, struct dtr_path, path);
}

static int dtr_init(struct drbd_transport *transport)
{
	struct drbd_rdma_transport *rdma_transport =
		container_of(transport, struct drbd_rdma_transport, transport);

	transport->ops = &dtr_ops;
	transport->class = &rdma_transport_class;

	rdma_transport->rx_allocation_size = allocation_size;
	rdma_transport->active = false;

	return 0;
}

static void dtr_free(struct drbd_transport *transport, enum drbd_tr_free_op free_op)
{
	struct drbd_rdma_transport *rdma_transport =
		container_of(transport, struct drbd_rdma_transport, transport);
	struct drbd_path *drbd_path;

	list_for_each_entry(drbd_path, &transport->paths, list) {
		struct dtr_path *path = container_of(drbd_path, struct dtr_path, path);
		dtr_disconnect_path(path);
	}

	rdma_transport->active = false;

	if (free_op == DESTROY_TRANSPORT) {
		struct drbd_path *tmp;
		list_for_each_entry_safe(drbd_path, tmp, &transport->paths, list) {
			struct dtr_path *path = container_of(drbd_path, struct dtr_path, path);
			list_del_init(&drbd_path->list);
			dtr_uninit_path(path);
			kfree(path);
		}

		/* The transport object itself is embedded into a conneciton.
		   Do not free it here! The function should better be called
		   uninit. */
	}
}


static int dtr_send(struct dtr_path *path, void *buf, size_t size)
{
	struct ib_device *device;
	struct drbd_rdma_tx_desc *tx_desc;
	void *send_buffer;

	// pr_info("%s: dtr_send() size = %d data[0]:%lx\n", rdma_stream->name, (int)size, *(unsigned long*)buf);

	tx_desc = kzalloc(sizeof(*tx_desc) + sizeof(struct ib_sge), GFP_NOIO);
	if (!tx_desc)
		return -ENOMEM;

	send_buffer = kmalloc(size, GFP_NOIO);
	if (!send_buffer) {
		kfree(tx_desc);
		return -ENOMEM;
	}
	memcpy(send_buffer, buf, size);

	device = path->cm->id->device;
	tx_desc->type = SEND_MSG;
	tx_desc->data = send_buffer;
	tx_desc->nr_sges = 1;
	tx_desc->sge[0].addr = ib_dma_map_single(device, send_buffer, size, DMA_TO_DEVICE);
	tx_desc->sge[0].lkey = path->dma_mr->lkey;
	tx_desc->sge[0].length = size;
	tx_desc->stream_nr = ST_FLOW_CTRL;

	__dtr_post_tx_desc(path, tx_desc);

	return size;
}


static int dtr_recv_pages(struct drbd_transport *transport, struct drbd_page_chain_head *chain, size_t size)
{
	struct drbd_rdma_transport *rdma_transport =
		container_of(transport, struct drbd_rdma_transport, transport);
	struct dtr_stream *rdma_stream = &rdma_transport->stream[DATA_STREAM];
	struct page *page, *head = NULL, *tail = NULL;
	int i = 0;

	if (!dtr_transport_ok(transport))
		return -ECONNRESET;

	// pr_info("%s: in recv_pages, size: %zu\n", rdma_stream->name, size);
	TR_ASSERT(transport, rdma_stream->current_rx.bytes_left == 0);
	dtr_recycle_rx_desc(transport, DATA_STREAM, &rdma_stream->current_rx.desc);
	dtr_refill_rx_desc(rdma_transport, DATA_STREAM);

	while (size) {
		struct drbd_rdma_rx_desc *rx_desc = NULL;
		long t;

		t = wait_event_interruptible_timeout(rdma_stream->recv_wq,
					dtr_receive_rx_desc(rdma_stream, &rx_desc),
					rdma_stream->recv_timeout);

		if (t <= 0) {
			/*
			 * Cannot give back pages that may still be in use!
			 * (More reason why we only have one rx_desc per page,
			 * and don't get_page() in dtr_create_rx_desc).
			 */
			drbd_free_pages(transport, head, 0);
			return t == 0 ? -EAGAIN : -EINTR;
		}

		page = rx_desc->page;
		/* put_page() if we would get_page() in
		 * dtr_create_rx_desc().  but we don't. We return the page
		 * chain to the user, which is supposed to give it back to
		 * drbd_free_pages() eventually. */
		rx_desc->page = NULL;
		size -= rx_desc->size;

		/* If the sender did dtr_send_page every bvec of a bio with
		 * unaligned bvecs (as xfs often creates), rx_desc->size and
		 * offset may well be not the PAGE_SIZE and 0 we hope for.
		 */
		if (tail) {
			/* See also dtr_create_rx_desc().
			 * For PAGE_SIZE > 4k, we may create several RR per page.
			 * We cannot link a page to itself, though.
			 *
			 * Adding to size would be easy enough.
			 * But what do we do about possible holes?
			 * FIXME
			 */
			BUG_ON(page == tail);

			set_page_chain_next(tail, page);
			tail = page;
		} else
			head = tail = page;

		set_page_chain_offset(page, 0);
		set_page_chain_size(page, rx_desc->size);

		rx_desc->path->flow[DATA_STREAM].rx_descs_allocated--;
		dtr_free_rx_desc(NULL, rx_desc);

		i++;
		dtr_refill_rx_desc(rdma_transport, DATA_STREAM);
	}

	// pr_info("%s: rcvd %d pages\n", rdma_stream->name, i);
	chain->head = head;
	chain->nr_pages = i;
	return 0;
}

static int _dtr_recv(struct drbd_transport *transport, enum drbd_stream stream,
		     void **buf, size_t size, int flags)
{
	struct drbd_rdma_transport *rdma_transport =
		container_of(transport, struct drbd_rdma_transport, transport);
	struct dtr_stream *rdma_stream = &rdma_transport->stream[stream];
	struct drbd_rdma_rx_desc *rx_desc = NULL;
	void *buffer;

	if (flags & GROW_BUFFER) {
		/* Since transport_rdma always returns the full, requested amount
		   of data, DRBD should never call with GROW_BUFFER! */
		tr_err(transport, "Called with GROW_BUFFER\n");
		return -EINVAL;
	} else if (rdma_stream->current_rx.bytes_left == 0) {
		long t;

		dtr_recycle_rx_desc(transport, stream, &rdma_stream->current_rx.desc);
		if (flags & MSG_DONTWAIT) {
			t = dtr_receive_rx_desc(rdma_stream, &rx_desc);
		} else {
			t = wait_event_interruptible_timeout(rdma_stream->recv_wq,
						dtr_receive_rx_desc(rdma_stream, &rx_desc),
						rdma_stream->recv_timeout);
		}

		if (t <= 0)
			return t == 0 ? -EAGAIN : -EINTR;

		// pr_info("%s: got a new page with size: %d\n", rdma_stream->name, rx_desc->size);
		buffer = page_address(rx_desc->page);
		rdma_stream->current_rx.desc = rx_desc;
		rdma_stream->current_rx.pos = buffer + size;
		rdma_stream->current_rx.bytes_left = rx_desc->size - size;
		if (rdma_stream->current_rx.bytes_left < 0)
			tr_warn(transport,
				"new, requesting more (%zu) than available (%d)\n", size, rx_desc->size);

		if (flags & CALLER_BUFFER)
			memcpy(*buf, buffer, size);
		else
			*buf = buffer;

		// pr_info("%s: recv completely new fine, returning size on\n", rdma_stream->name);
		// pr_info("%s: rx_count: %d\n", rdma_stream->name, rdma_stream->rx_descs_posted);

		return size;
	} else { /* return next part */
		// pr_info("recv next part on %s\n", rdma_stream->name);
		buffer = rdma_stream->current_rx.pos;
		rdma_stream->current_rx.pos += size;

		if (rdma_stream->current_rx.bytes_left < size) {
			tr_err(transport,
			       "requested more than left! bytes_left = %d, size = %zu\n",
					rdma_stream->current_rx.bytes_left, size);
			rdma_stream->current_rx.bytes_left = 0; /* 0 left == get new entry */
		} else {
			rdma_stream->current_rx.bytes_left -= size;
			// pr_info("%s: old_rx left: %d\n", rdma_stream->name, rdma_stream->current_rx.bytes_left);
		}

		if (flags & CALLER_BUFFER)
			memcpy(*buf, buffer, size);
		else
			*buf = buffer;

		// pr_info("%s: recv next part fine, returning size\n", rdma_stream->name);
		return size;
	}

	return 0;
}

static int dtr_recv(struct drbd_transport *transport, enum drbd_stream stream, void **buf, size_t size, int flags)
{
	struct drbd_rdma_transport *rdma_transport =
		container_of(transport, struct drbd_rdma_transport, transport);
	int err;

	if (!dtr_transport_ok(transport))
		return -ECONNRESET;

	err = _dtr_recv(transport, stream, buf, size, flags);

	dtr_refill_rx_desc(rdma_transport, stream);
	return err;
}

static void dtr_stats(struct drbd_transport* transport, struct drbd_transport_stats *stats)
{
	struct drbd_path *drbd_path;
	int sb_size = 0, sb_used = 0;

	list_for_each_entry(drbd_path, &transport->paths, list) {
		struct dtr_path *path = container_of(drbd_path, struct dtr_path, path);
		struct dtr_flow *flow = &path->flow[DATA_STREAM];

		sb_size += flow->tx_descs_max;
		sb_used += atomic_read(&flow->tx_descs_posted);
	}

	/* these are used by the sender, guess we should them get right */
	stats->send_buffer_size = sb_size * DRBD_SOCKET_BUFFER_SIZE;
	stats->send_buffer_used = sb_used * DRBD_SOCKET_BUFFER_SIZE;

	/* these two for debugfs */
	stats->unread_received = 0; /* Iterate over tx_descs... to find out */
	stats->unacked_send = stats->send_buffer_used;

}

static int dtr_cma_event_handler(struct rdma_cm_id *cm_id, struct rdma_cm_event *event)
{
	int err;
	/* context comes from rdma_create_id() */
	struct dtr_cm *cm_context = cm_id->context;
	struct dtr_listener *listener;
	struct drbd_waiter *waiter;

	if (!cm_context) {
		pr_err("id %p event %d, but no context!\n", cm_id, event->event);
		return 0;
	}

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		// pr_info("%s: RDMA_CM_EVENT_ADDR_RESOLVED\n", cm_context->name);
		cm_context->state = ADDR_RESOLVED;
		err = rdma_resolve_route(cm_id, 2000);
		if (err)
			pr_err("rdma_resolve_route error %d\n", err);
		wake_up_interruptible(&cm_context->state_wq);
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		// pr_info("%s: RDMA_CM_EVENT_ROUTE_RESOLVED\n", cm_context->name);
		cm_context->state = ROUTE_RESOLVED;
		wake_up_interruptible(&cm_context->state_wq);
		break;

	case RDMA_CM_EVENT_CONNECT_REQUEST:
		// pr_info("%s: RDMA_CM_EVENT_CONNECT_REQUEST\n", cm_context->name);
		/* for listener */
		cm_context->state = CONNECT_REQUEST;

		listener = container_of(cm_context, struct dtr_listener, cm);

		spin_lock(&listener->listener.waiters_lock);
		listener->listener.pending_accepts++;
		waiter = list_entry(listener->listener.waiters.next, struct drbd_waiter, list);

		/* I found this a bit confusing. When a new connection comes in, the callback
		   gets called with a new rdma_cm_id. The new rdma_cm_id inherits its context
		   pointer from the listening rdma_cm_id. We will create a new context later */

		/* Insert the fresh cm_id it at the head of the list child_cms */
		cm_id->context = listener->child_cms;
		listener->child_cms = cm_id;

		/* set cm_id to the listener */
		cm_id = cm_context->id;

		wake_up(&waiter->wait); /* wake an arbitrary waiter */
		spin_unlock(&listener->listener.waiters_lock);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		// pr_info("%s: RDMA_CM_EVENT_ESTABLISHED\n", cm_context->name);
		cm_context->state = CONNECTED;
		wake_up_interruptible(&cm_context->state_wq);
		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
		// pr_info("%s: RDMA_CM_EVENT_ADDR_ERROR\n", cm_context->name);
	case RDMA_CM_EVENT_ROUTE_ERROR:
		// pr_info("%s: RDMA_CM_EVENT_ROUTE_ERROR\n", cm_context->name);
	case RDMA_CM_EVENT_CONNECT_ERROR:
		// pr_info("%s: RDMA_CM_EVENT_CONNECT_ERROR\n", cm_context->name);
	case RDMA_CM_EVENT_UNREACHABLE:
		// pr_info("%s: RDMA_CM_EVENT_UNREACHABLE\n", cm_context->name);
	case RDMA_CM_EVENT_REJECTED:
		// pr_info("%s: RDMA_CM_EVENT_REJECTED\n", cm_context->name);
		// pr_info("event = %d, status = %d\n", event->event, event->status);
		cm_context->state = ERROR;
		wake_up_interruptible(&cm_context->state_wq);
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		// pr_info("%s: RDMA_CM_EVENT_DISCONNECTED\n", cm_context->name);
		cm_context->state = DISCONNECTED;
		wake_up_interruptible(&cm_context->state_wq);
		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		// pr_info("%s: RDMA_CM_EVENT_DEVICE_REMOVAL\n", cm_context->name);
		break;

	default:
		pr_warn("id %p context %p unexpected event %d!\n",
				cm_id, cm_context, event->event);
		wake_up_interruptible(&cm_context->state_wq);
		break;
	}
	return 0;
}

static int dtr_create_cm_id(struct dtr_cm *cm_context)
{
	struct rdma_cm_id *id;

	cm_context->state = IDLE;
	init_waitqueue_head(&cm_context->state_wq);

	id = rdma_create_id(dtr_cma_event_handler, cm_context, RDMA_PS_TCP, IB_QPT_RC);
	if (IS_ERR(id)) {
		cm_context->id = NULL;
		cm_context->state = ERROR;
		return PTR_ERR(id);
	}

	cm_context->id = id;
	return 0;
}

static bool dtr_receive_rx_desc(struct dtr_stream *rdma_stream,
				struct drbd_rdma_rx_desc **ptr_rx_desc)
{
	struct drbd_rdma_rx_desc *rx_desc;

	spin_lock_irq(&rdma_stream->rx_descs_lock);
	rx_desc = list_first_entry_or_null(&rdma_stream->rx_descs, struct drbd_rdma_rx_desc, list);
	if (rx_desc) {
		if (rx_desc->sequence == rdma_stream->rx_sequence) {
			list_del(&rx_desc->list);
			rdma_stream->rx_sequence =
				(rdma_stream->rx_sequence + 1) & ((1UL << SEQUENCE_BITS) - 1);
		} else {
			rx_desc = NULL;
		}
	}
	spin_unlock_irq(&rdma_stream->rx_descs_lock);

	if (rx_desc) {
		struct dtr_path *path = rx_desc->path;
		struct drbd_rdma_transport *rdma_transport = path->rdma_transport;

		INIT_LIST_HEAD(&rx_desc->list);
		ib_dma_sync_single_for_cpu(rx_desc->path->cm->id->device, rx_desc->sge.addr,
					   rdma_transport->rx_allocation_size, DMA_FROM_DEVICE);
		*ptr_rx_desc = rx_desc;
		return true;
	}

	return false;
}

static int dtr_send_flow_control_msg(struct dtr_path *path)
{
	struct dtr_flow *flow;
	struct dtr_flow_control msg;
	enum drbd_stream i;

	msg.magic = cpu_to_be32(DTR_MAGIC);
	for (i = DATA_STREAM; i <= CONTROL_STREAM; i++) {
		int n;

		flow = &path->flow[i];
		n = flow->rx_descs_posted - atomic_read(&flow->rx_descs_known_to_peer);

		atomic_add(n, &flow->rx_descs_known_to_peer);
		msg.new_rx_descs[i] = cpu_to_be32(n);
	}

	return dtr_send(path, &msg, sizeof(msg));
}

static void dtr_flow_control(struct dtr_flow *flow)
{
	int n, known_to_peer = atomic_read(&flow->rx_descs_known_to_peer);
	int tx_descs_max = flow->tx_descs_max;

	n = flow->rx_descs_posted - known_to_peer;
	if (n > tx_descs_max / 8 || known_to_peer < tx_descs_max / 8)
		dtr_send_flow_control_msg(flow->path);
}

static void dtr_got_flow_control_msg(struct dtr_path *path,
				     struct dtr_flow_control *msg)
{
	struct drbd_rdma_transport *rdma_transport = path->rdma_transport;
	struct dtr_flow *flow;
	int i, n;

	for (i = CONTROL_STREAM; i >= DATA_STREAM; i--) {
		uint32_t new_rx_descs = be32_to_cpu(msg->new_rx_descs[i]);
		flow = &path->flow[i];

		n = atomic_add_return(new_rx_descs, &flow->peer_rx_descs);
		wake_up_interruptible(&rdma_transport->stream[i].send_wq);
	}

	/* rdma_stream is the data_stream here... */
	if (n >= DESCS_LOW_LEVEL) {
		int tx_descs_posted = atomic_read(&flow->tx_descs_posted);
		if (flow->tx_descs_max - tx_descs_posted >= DESCS_LOW_LEVEL)
			clear_bit(NET_CONGESTED, &rdma_transport->transport.flags);
	}
}

static void __dtr_order_rx_descs(struct dtr_stream *rdma_stream,
				 struct drbd_rdma_rx_desc *rx_desc)
{
	struct drbd_rdma_rx_desc *pos;
	unsigned int seq = rx_desc->sequence;

	list_for_each_entry_reverse(pos, &rdma_stream->rx_descs, list) {
		if (seq > pos->sequence) {
			list_add(&rx_desc->list, &pos->list);
			return;
		}
	}
	list_add(&rx_desc->list, &rdma_stream->rx_descs);
}

static void dtr_order_rx_descs(struct dtr_stream *rdma_stream,
			       struct drbd_rdma_rx_desc *rx_desc)
{
	unsigned long flags;

	spin_lock_irqsave(&rdma_stream->rx_descs_lock, flags);
	__dtr_order_rx_descs(rdma_stream, rx_desc);
	spin_unlock_irqrestore(&rdma_stream->rx_descs_lock, flags);
}

static void dtr_rx_cq_event_handler(struct ib_cq *cq, void *ctx)
{
	struct dtr_path *path = ctx;
	struct drbd_rdma_transport *rdma_transport = path->rdma_transport;
	struct drbd_rdma_rx_desc *rx_desc;
	union dtr_immediate immediate;
	struct ib_wc wc;
	int ret;

	ret = ib_req_notify_cq(path->recv_cq, IB_CQ_NEXT_COMP);
	if (ret)
		tr_err(&rdma_transport->transport, "ib_req_notify_cq failed\n");

	while ((ret = ib_poll_cq(cq, 1, &wc)) == 1) {
		rx_desc = (struct drbd_rdma_rx_desc *) (unsigned long) wc.wr_id;

		if(wc.status != IB_WC_SUCCESS || wc.opcode != IB_WC_RECV) {
			tr_warn(&rdma_transport->transport,
				"wc.status = %d (%s), wc.opcode = %d (%s)\n",
				wc.status, wc.status == IB_WC_SUCCESS ? "ok" : "bad",
				wc.opcode, wc.opcode == IB_WC_RECV ? "ok": "bad");
			return;
		}

		rx_desc->size = wc.byte_len;
		immediate.i = be32_to_cpu(wc.ex.imm_data);
		if (immediate.stream == ST_FLOW_CTRL) {
			ib_dma_sync_single_for_cpu(path->cm->id->device, rx_desc->sge.addr,
						   rdma_transport->rx_allocation_size, DMA_FROM_DEVICE);
			dtr_got_flow_control_msg(path, page_address(rx_desc->page));
			/* rx_descs_posted, rx_descs_konwn_to_peer stays constant */
			dtr_repost_rx_desc(path, rx_desc);
		} else {
			struct dtr_flow *flow = &path->flow[immediate.stream];
			struct dtr_stream *rdma_stream = &rdma_transport->stream[immediate.stream];

			flow->rx_descs_posted--;
			atomic_dec(&flow->rx_descs_known_to_peer);

			rx_desc->sequence = immediate.sequence;
			dtr_order_rx_descs(rdma_stream, rx_desc);

			wake_up_interruptible(&rdma_stream->recv_wq);
		}
	}

	// pr_info("%s: got rx cq event. state %d\n", rdma_stream->name, rdma_stream->cm.state);
}

static void dtr_free_tx_desc(struct dtr_path *path, struct drbd_rdma_tx_desc *tx_desc)
{
	struct ib_device *device = path->cm->id->device;
	DRBD_BIO_VEC_TYPE bvec;
	DRBD_ITER_TYPE iter;
	int i, nr_sges;

	switch (tx_desc->type) {
	case SEND_PAGE:
		ib_dma_unmap_page(device, tx_desc->sge[0].addr, tx_desc->sge[0].length, DMA_TO_DEVICE);
		put_page(tx_desc->page);
		break;
	case SEND_MSG:
		ib_dma_unmap_single(device, tx_desc->sge[0].addr, tx_desc->sge[0].length, DMA_TO_DEVICE);
		kfree(tx_desc->data);
		break;
	case SEND_BIO:
		nr_sges = tx_desc->nr_sges;
		for (i = 0; i < nr_sges; i++)
			ib_dma_unmap_page(device, tx_desc->sge[i].addr, tx_desc->sge[i].length,
					  DMA_TO_DEVICE);
		bio_for_each_segment(bvec, tx_desc->bio, iter)
			put_page(bvec BVD bv_page);
		break;
	}
	kfree(tx_desc);
}

static void dtr_tx_cq_event_handler(struct ib_cq *cq, void *ctx)
{
	struct dtr_path *path = ctx;
	struct dtr_stream *rdma_stream = NULL;
	struct drbd_rdma_transport *rdma_transport = path->rdma_transport;
	struct drbd_rdma_tx_desc *tx_desc;
	struct ib_wc wc;
	enum dtr_stream_nr stream_nr;
	int ret;

	// pr_info("%s: got tx cq event. state %d\n", rdma_stream->name, rdma_stream->cm.state);

	ret = ib_req_notify_cq(cq, IB_CQ_NEXT_COMP);
	if (ret)
		tr_err(&rdma_transport->transport, "ib_req_notify_cq failed\n");

	/* Alternatively put them onto a list here, and do the processing (freeing)
	   at a later point in time. Probably resource freeing is cheap enough to do
	   it directly here. */
	while ((ret = ib_poll_cq(cq, 1, &wc)) == 1) {
		tx_desc = (struct drbd_rdma_tx_desc *) (unsigned long) wc.wr_id;
		stream_nr = tx_desc->stream_nr;

		if (wc.status != IB_WC_SUCCESS) {
			tr_err(&rdma_transport->transport,
			       "tx_event: wc.status != IB_WC_SUCCESS %d\n", wc.status);
			goto disconnect;
		}

		if (wc.opcode != IB_WC_SEND) {
			tr_err(&rdma_transport->transport, "wc.opcode != IB_WC_SEND %d\n", wc.opcode);
			goto disconnect;
		}

		if (stream_nr != ST_FLOW_CTRL) {
			struct dtr_flow *flow = &path->flow[stream_nr];
			rdma_stream = &rdma_transport->stream[stream_nr];
			atomic_dec(&flow->tx_descs_posted);
		}
		dtr_free_tx_desc(path, tx_desc);
	}

	if (ret != 0)
		tr_warn(&rdma_transport->transport, "ib_poll_cq() returned %d\n", ret);

	if (0) {
		int err;
disconnect:
		path->cm->state = ERROR;

		err = dtr_repost_tx_desc(rdma_transport, tx_desc);
		if (err) {
			tr_warn(&rdma_transport->transport, "repost of tx_desc failed!\n");
			/* TODO: queue instead! */
			dtr_free_tx_desc(path, tx_desc);
		}

		if (!test_and_set_bit(P_CONNECT_WORK, &path->flags))
			queue_work(dtr_work_queue, &path->connect_work);
	}

	if (rdma_stream)
		wake_up_interruptible(&rdma_stream->send_wq);
}

static int dtr_create_qp(struct dtr_path *path, int rx_descs_max, int tx_descs_max)
{
	int err;
	struct ib_qp_init_attr init_attr = {
		.cap.max_send_wr = tx_descs_max,
		.cap.max_recv_wr = rx_descs_max,
		.cap.max_recv_sge = 1, /* We only receive into single pages */
		.cap.max_send_sge = DTR_MAX_TX_SGES,
		.qp_type = IB_QPT_RC,
		.send_cq = path->send_cq,
		.recv_cq = path->recv_cq,
		.sq_sig_type = IB_SIGNAL_REQ_WR
	};

	err = rdma_create_qp(path->cm->id, path->pd, &init_attr);
	if (err) {
		tr_err(&path->rdma_transport->transport,
				"rdma_create_qp failed: %d\n", err);
		return err;
	}

	path->qp = path->cm->id->qp;
	return 0;
}

static int dtr_post_rx_desc(struct dtr_path *path,
		struct drbd_rdma_rx_desc *rx_desc)
{
	struct drbd_rdma_transport *rdma_transport = path->rdma_transport;
	struct ib_recv_wr recv_wr, *recv_wr_failed;
	int err;

	recv_wr.next = NULL;
	recv_wr.wr_id = (unsigned long)rx_desc;
	recv_wr.sg_list = &rx_desc->sge;
	recv_wr.num_sge = 1;

	ib_dma_sync_single_for_device(path->cm->id->device,
			rx_desc->sge.addr, rdma_transport->rx_allocation_size, DMA_FROM_DEVICE);

	err = ib_post_recv(path->qp, &recv_wr, &recv_wr_failed);
	if (err) {
		tr_err(&rdma_transport->transport, "ib_post_recv error %d\n", err);
		return err;
	}
	// pr_info("%s: Created recv_wr (%p, %p): lkey=%x, addr=%llx, length=%d\n", rdma_stream->name, rx_desc->page, rx_desc, rx_desc->sge.lkey, rx_desc->sge.addr, rx_desc->sge.length);

	return 0;
}

static void dtr_free_rx_desc(struct dtr_path *unused, struct drbd_rdma_rx_desc *rx_desc)
{
	struct dtr_path *path = rx_desc->path;
	struct ib_device *device = path->cm->id->device;
	struct drbd_rdma_transport *rdma_transport = path->rdma_transport;
	int alloc_size = rdma_transport->rx_allocation_size;

	if (!rx_desc)
		return; /* Allow call with NULL */

	ib_dma_unmap_single(device, rx_desc->sge.addr, alloc_size, DMA_FROM_DEVICE);

	if (rx_desc->page) {
		/* put_page(), if we had more than one rx_desc per page,
		 * but see comments in dtr_create_rx_desc */
		drbd_free_pages(&rdma_transport->transport, rx_desc->page, 0);
	}
	kfree(rx_desc);
}

static int dtr_create_rx_desc(struct dtr_flow *flow)
{
	struct dtr_path *path = flow->path;
	struct drbd_transport *transport = &path->rdma_transport->transport;
	struct drbd_rdma_rx_desc *rx_desc;
	struct ib_device *device = path->cm->id->device;
	struct page *page;
	int err, alloc_size = path->rdma_transport->rx_allocation_size;
	int nr_pages = alloc_size / PAGE_SIZE;

	rx_desc = kzalloc(sizeof(*rx_desc), GFP_NOIO);
	if (!rx_desc)
		return -ENOMEM;

	/* As of now, this MUST NEVER return a highmem page!
	 * Which means no other user may ever have requested and then given
	 * back a highmem page!
	 */
	page = drbd_alloc_pages(transport, nr_pages, GFP_NOIO);
	if (!page) {
		kfree(rx_desc);
		return -ENOMEM;
	}
	BUG_ON(PageHighMem(page));

	rx_desc->path = path;
	rx_desc->page = page;
	rx_desc->size = 0;
	rx_desc->sge.lkey = path->dma_mr->lkey;
	rx_desc->sge.addr = ib_dma_map_single(device, page_address(page), alloc_size,
					      DMA_FROM_DEVICE);
	rx_desc->sge.length = alloc_size;

	err = dtr_post_rx_desc(path, rx_desc);
	if (err) {
		tr_err(transport, "dtr_post_rx_desc() returned %d\n", err);
		dtr_free_rx_desc(path, rx_desc);
	} else {
		flow->rx_descs_allocated++;
		flow->rx_descs_posted++;
	}

	return err;
}

static void __dtr_refill_rx_desc(struct dtr_path *path, enum drbd_stream stream)
{
	struct dtr_flow *flow = &path->flow[stream];
	int descs_want_posted, descs_max;

	descs_max = flow->rx_descs_max;
	descs_want_posted = flow->rx_descs_want_posted;

	while (flow->rx_descs_posted < descs_want_posted &&
	       flow->rx_descs_allocated < descs_max) {
		int err = dtr_create_rx_desc(flow);
		if (err)
			break;
	}
}

static void dtr_refill_rx_desc(struct drbd_rdma_transport *rdma_transport,
			       enum drbd_stream stream)
{
	struct drbd_transport *transport = &rdma_transport->transport;
	struct drbd_path *drbd_path;

	list_for_each_entry(drbd_path, &transport->paths, list) {
		struct dtr_path *path = container_of(drbd_path, struct dtr_path, path);

		if (!dtr_path_ok(path))
			continue;

		__dtr_refill_rx_desc(path, stream);
		dtr_flow_control(&path->flow[stream]);
	}
}

static void dtr_repost_rx_desc(struct dtr_path *path,
			       struct drbd_rdma_rx_desc *rx_desc)
{
	int err;

	rx_desc->size = 0;
	rx_desc->sge.lkey = path->dma_mr->lkey;
	/* rx_desc->sge.addr = rx_desc->dma_addr;
	   rx_desc->sge.length = rx_desc->alloc_size; */

	err = dtr_post_rx_desc(path, rx_desc);
	if (err) {
		struct drbd_transport *transport = &path->rdma_transport->transport;
		tr_err(transport, "repost of an rx_desc failed!\n");
		/* one of the rx_descs_allocated is now off by one! */
		dtr_free_rx_desc(path, rx_desc);
	}
}

static void dtr_recycle_rx_desc(struct drbd_transport *transport,
				enum drbd_stream stream,
				struct drbd_rdma_rx_desc **pp_rx_desc)
{
	struct drbd_path *drbd_path;
	struct drbd_rdma_rx_desc *rx_desc = *pp_rx_desc;

	if (!rx_desc)
		return;

	list_for_each_entry(drbd_path, &transport->paths, list) {
		struct dtr_path *path = container_of(drbd_path, struct dtr_path, path);
		struct dtr_flow *flow = &path->flow[stream];
		int max_posted = flow->rx_descs_max;


		if (flow->rx_descs_posted < max_posted) {
			dtr_repost_rx_desc(path, rx_desc);
			dtr_flow_control(flow);
			goto done;
		}
	}

	rx_desc->path->flow[stream].rx_descs_allocated--;
	dtr_free_rx_desc(NULL, rx_desc);

done:
	*pp_rx_desc = NULL;
}

static int __dtr_post_tx_desc(struct dtr_path *path,
			      struct drbd_rdma_tx_desc *tx_desc)
{
	struct drbd_rdma_transport *rdma_transport = path->rdma_transport;
	struct ib_device *device = path->cm->id->device;
	struct ib_send_wr send_wr, *send_wr_failed;
	enum dtr_stream_nr stream_nr = tx_desc->stream_nr;
	union dtr_immediate immediate;
	int i, err;

	immediate = (union dtr_immediate) {
		.stream = stream_nr,
		.sequence =
			stream_nr == ST_FLOW_CTRL ? 0 :
				rdma_transport->stream[stream_nr].tx_sequence++,
	};
	send_wr.next = NULL;
	send_wr.wr_id = (unsigned long)tx_desc;
	send_wr.sg_list = tx_desc->sge;
	send_wr.num_sge = tx_desc->nr_sges;
	send_wr.ex.imm_data = cpu_to_be32(immediate.i);
	send_wr.opcode = IB_WR_SEND_WITH_IMM;
	send_wr.send_flags = IB_SEND_SIGNALED;

	for (i = 0; i < tx_desc->nr_sges; i++)
		ib_dma_sync_single_for_device(device, tx_desc->sge[i].addr,
					      tx_desc->sge[i].length, DMA_TO_DEVICE);

	err = ib_post_send(path->qp, &send_wr, &send_wr_failed);
	if (err)
		tr_err(&rdma_transport->transport, "ib_post_send() failed %d\n", err);

	return err;
}

static struct dtr_path *dtr_select_path_for_tx(struct drbd_rdma_transport *rdma_transport,
					       enum drbd_stream stream)
{
	struct drbd_transport *transport = &rdma_transport->transport;
	struct drbd_path *drbd_path;

	/* TOOD: Could try to balance along the paths in a more clever way */

	list_for_each_entry(drbd_path, &transport->paths, list) {
		struct dtr_path *path = container_of(drbd_path, struct dtr_path, path);
		struct dtr_flow *flow = &path->flow[stream];

		if (!dtr_path_ok(path))
			continue;

		if (atomic_read(&flow->tx_descs_posted) < flow->tx_descs_max &&
		    atomic_read(&flow->peer_rx_descs))
			return path;
	}

	return NULL;
}

static int dtr_repost_tx_desc(struct drbd_rdma_transport *rdma_transport,
			      struct drbd_rdma_tx_desc *tx_desc)
{
	struct dtr_path *path = dtr_select_path_for_tx(rdma_transport, tx_desc->stream_nr);
	int err;

	if (path)
		err = __dtr_post_tx_desc(path, tx_desc);
	else
		err = -ECONNRESET;

	return err;
}

static int dtr_post_tx_desc(struct drbd_rdma_transport *rdma_transport,
			    struct drbd_rdma_tx_desc *tx_desc)
{
	enum drbd_stream stream = tx_desc->stream_nr;
	struct dtr_stream *rdma_stream = &rdma_transport->stream[stream];
	struct ib_device *device;
	struct dtr_path *path;
	struct dtr_flow *flow;
	int offset, err;
	long t;

retry:
	t = wait_event_interruptible_timeout(rdma_stream->send_wq,
			path = dtr_select_path_for_tx(rdma_transport, stream),
			rdma_stream->send_timeout);

	if (t == 0) {
		struct drbd_rdma_transport *rdma_transport = rdma_stream->rdma_transport;

		if (drbd_stream_send_timed_out(&rdma_transport->transport, stream))
			return -EAGAIN;
		goto retry;
	} else if (t < 0)
		return -EINTR;

	device = path->cm->id->device;
	switch (tx_desc->type) {
	case SEND_PAGE:
		offset = tx_desc->sge[0].lkey;
		tx_desc->sge[0].addr = ib_dma_map_page(device, tx_desc->page, offset,
						      tx_desc->sge[0].length, DMA_TO_DEVICE);
		tx_desc->sge[0].lkey = path->dma_mr->lkey;
		break;
	case SEND_MSG:
	case SEND_BIO:
		BUG();
	}

	err = __dtr_post_tx_desc(path, tx_desc);
	flow = &path->flow[stream];

	atomic_inc(&flow->tx_descs_posted);
	atomic_dec(&flow->peer_rx_descs);

	// pr_info("%s: Created send_wr (%p, %p): nr_sges=%u, first seg: lkey=%x, addr=%llx, length=%d\n", rdma_stream->name, tx_desc->page, tx_desc, tx_desc->nr_sges, tx_desc->sge[0].lkey, tx_desc->sge[0].addr, tx_desc->sge[0].length);

	return err;
}

static int dtr_init_flow(struct dtr_flow *flow, struct dtr_path *path)
{
	struct drbd_transport *transport = &path->rdma_transport->transport;
	unsigned int rcvbuf_size = RDMA_DEF_BUFFER_SIZE;
	unsigned int sndbuf_size = RDMA_DEF_BUFFER_SIZE;
	struct net_conf *nc;
	int err = 0;

	rcu_read_lock();
	nc = rcu_dereference(transport->net_conf);
	if (!nc) {
		rcu_read_unlock();
		tr_err(transport, "need net_conf\n");
		err = -EINVAL;
		goto out;
	}

	if (nc->rcvbuf_size)
		rcvbuf_size = nc->rcvbuf_size;
	if (nc->sndbuf_size)
		sndbuf_size = nc->sndbuf_size;

	/* Do not allow smaller settings than RDMA_DEF_BUFFER_SIZE for now. */
	rcvbuf_size = max(rcvbuf_size, RDMA_DEF_BUFFER_SIZE);
	sndbuf_size = max(sndbuf_size, RDMA_DEF_BUFFER_SIZE);

	if (rcvbuf_size / DRBD_SOCKET_BUFFER_SIZE > nc->max_buffers) {
		tr_err(transport, "Set max-buffers at least to %d, (right now it is %d).\n",
		       rcvbuf_size / DRBD_SOCKET_BUFFER_SIZE, nc->max_buffers);
		tr_err(transport, "This is due to rcvbuf-size = %d.\n", rcvbuf_size);
		rcu_read_unlock();
		err = -EINVAL;
		goto out;
	}

	rcu_read_unlock();

	flow->path = path;
	flow->tx_descs_max = sndbuf_size / DRBD_SOCKET_BUFFER_SIZE;
	flow->rx_descs_max = rcvbuf_size / DRBD_SOCKET_BUFFER_SIZE;

	atomic_set(&flow->tx_descs_posted, 0);
	atomic_set(&flow->peer_rx_descs, 0);
	atomic_set(&flow->rx_descs_known_to_peer, 0);

	flow->rx_descs_posted = 0;
	flow->rx_descs_allocated = 0;

	flow->rx_descs_want_posted = flow->rx_descs_max / 2;

 out:
	return err;
}

/* allocate rdma specific resources for the stream */
static void dtr_init_stream(struct dtr_stream *rdma_stream,
			    struct drbd_transport *transport)
{
	rdma_stream->current_rx.desc = NULL;
	rdma_stream->current_rx.pos = NULL;
	rdma_stream->current_rx.bytes_left = 0;

	rdma_stream->recv_timeout = MAX_SCHEDULE_TIMEOUT;
	rdma_stream->send_timeout = MAX_SCHEDULE_TIMEOUT;

	sprintf(rdma_stream->name, "s%06d", stream_nr++);

	init_waitqueue_head(&rdma_stream->recv_wq);
	init_waitqueue_head(&rdma_stream->send_wq);
	rdma_stream->rdma_transport =
		container_of(transport, struct drbd_rdma_transport, transport);
	rdma_stream->tx_sequence = 1;
	rdma_stream->rx_sequence = 1;

	INIT_LIST_HEAD(&rdma_stream->rx_descs);
	spin_lock_init(&rdma_stream->rx_descs_lock);
}

static int dtr_path_alloc_rdma_res(struct dtr_path *path,
				   struct drbd_transport *transport)
{
	struct drbd_rdma_transport *rdma_transport =
		container_of(transport, struct drbd_rdma_transport, transport);
	int err, i, rx_descs_max = 0, tx_descs_max = 0;

	/* Each path might be the sole path, therefore it must be able to
	   support both streams */
	for (i = DATA_STREAM; i <= CONTROL_STREAM ; i++) {
		rx_descs_max += path->flow[i].rx_descs_max;
		tx_descs_max += path->flow[i].tx_descs_max;
	}

	/* alloc protection domain (PD) */
	path->pd = ib_alloc_pd(path->cm->id->device);
	if (IS_ERR(path->pd)) {
		tr_err(transport, "ib_alloc_pd failed\n");
		err = PTR_ERR(path->pd);
		goto pd_failed;
	}

	/* create recv completion queue (CQ) */
	path->recv_cq = ib_create_cq(path->cm->id->device,
			dtr_rx_cq_event_handler, NULL, path,
			rx_descs_max, 0);
	if (IS_ERR(path->recv_cq)) {
		tr_err(transport, "ib_create_cq recv failed\n");
		err = PTR_ERR(path->recv_cq);
		goto recv_cq_failed;
	}

	/* create send completion queue (CQ) */
	path->send_cq = ib_create_cq(path->cm->id->device,
			dtr_tx_cq_event_handler, NULL, path,
			tx_descs_max, 0);
	if (IS_ERR(path->send_cq)) {
		tr_err(transport, "ib_create_cq send failed\n");
		err = PTR_ERR(path->send_cq);
		goto send_cq_failed;
	}

	/* arm CQs */
	err = ib_req_notify_cq(path->recv_cq, IB_CQ_NEXT_COMP);
	if (err) {
		tr_err(transport, "ib_req_notify_cq recv failed\n");
		goto notify_failed;
	}

	err = ib_req_notify_cq(path->send_cq, IB_CQ_NEXT_COMP);
	if (err) {
		tr_err(transport, "ib_req_notify_cq send failed\n");
		goto notify_failed;
	}

	/* create a queue pair (QP) */
	err = dtr_create_qp(path, rx_descs_max, tx_descs_max);
	if (err) {
		tr_err(transport, "create_qp error %d\n", err);
		goto createqp_failed;
	}

	/* create RDMA memory region (MR) */
	path->dma_mr = ib_get_dma_mr(path->pd,
			IB_ACCESS_LOCAL_WRITE |
			IB_ACCESS_REMOTE_READ |
			IB_ACCESS_REMOTE_WRITE);
	if (IS_ERR(path->dma_mr)) {
		tr_err(transport, "ib_get_dma_mr failed\n");
		err = PTR_ERR(path->dma_mr);
		goto dma_failed;
	}

	path->rdma_transport = rdma_transport;

	return 0;

dma_failed:
	ib_destroy_qp(path->qp);
	path->qp = NULL;
createqp_failed:
notify_failed:
	ib_destroy_cq(path->send_cq);
	path->send_cq = NULL;
send_cq_failed:
	ib_destroy_cq(path->recv_cq);
	path->recv_cq = NULL;
recv_cq_failed:
	ib_dealloc_pd(path->pd);
	path->pd = NULL;
pd_failed:
	return err;
}


static void dtr_drain_cq(struct dtr_path *path, struct ib_cq *cq,
			 void (*free_desc)(struct dtr_path *, void *))
{
	struct ib_wc wc;
	void *desc;

	while (ib_poll_cq(cq, 1, &wc) == 1) {
		desc = (void *) (unsigned long) wc.wr_id;
		free_desc(path, desc);
	}
}

static void __dtr_disconnect_path(struct dtr_path *path, bool cancel_work)
{
	int err;

	if (!path)
		return;

	if (cancel_work && test_bit(P_CONNECT_WORK, &path->flags)) {
		set_bit(P_CANCEL_CONNECT, &path->flags);
		wait_event(path->wq, test_bit(P_CONNECT_WORK, &path->flags) == 0);
	}

	if (!path->cm || !path->cm->id)
		return;

	err = rdma_disconnect(path->cm->id);
	if (err) {
		pr_warn("failed to disconnect, id %p context %p err %d\n",
			path->cm->id, path->cm->id->context, err);
		/* We are ignoring errors here on purpose */
	}

	/* There might be a signal pending here. Not incorruptible! */
	wait_event_timeout(path->cm->state_wq,
			   path->cm->state >= DISCONNECTED,
			   HZ);

	if (path->send_cq)
		dtr_drain_cq(path, path->send_cq,
			(void (*)(struct dtr_path *, void *)) dtr_free_tx_desc);

	if (path->recv_cq)
		dtr_drain_cq(path, path->recv_cq,
			(void (*)(struct dtr_path *, void *)) dtr_free_rx_desc);

	/*
	   rx_descs_allocated = 0;
	*/

	if (path->cm->state < DISCONNECTED)
		/* rdma_stream->rdma_transport might still be NULL here. */
		pr_warn("WARN: not properly disconnected\n");
}

static void dtr_disconnect_path(struct dtr_path *path)
{
	__dtr_disconnect_path(path, true);
}

static void dtr_free_cm(struct dtr_cm *cm)
{
	if (!cm)
		return;

	if (cm->id) {
		/* Just in case some callback is still triggered
		 * after we kfree'd path. */
		cm->id->context = NULL;
		rdma_destroy_id(cm->id);
		cm->id = NULL;
	}

	kfree(cm);
}

static void __dtr_uninit_path(struct dtr_path *path, bool cancel_work)
{
	if (!path)
		return;

	__dtr_disconnect_path(path, cancel_work);

	if (path->dma_mr) {
		ib_dereg_mr(path->dma_mr);
		path->dma_mr = NULL;
	}
	if (path->qp) {
		ib_destroy_qp(path->qp);
		path->qp = NULL;
	}
	if (path->send_cq) {
		ib_destroy_cq(path->send_cq);
		path->send_cq = NULL;
	}
	if (path->recv_cq) {
		ib_destroy_cq(path->recv_cq);
		path->recv_cq = NULL;
	}
	if (path->pd) {
		ib_dealloc_pd(path->pd);
		path->pd = NULL;
	}
	if (path->cm) {
		dtr_free_cm(path->cm);
		path->cm = NULL;
	}
}

static void dtr_uninit_path(struct dtr_path *path)
{
	__dtr_uninit_path(path, true);
}

static int dtr_try_connect(struct dtr_path *path, struct dtr_cm **ret_cm)
{
	struct drbd_transport *transport = &path->rdma_transport->transport;
	struct rdma_conn_param conn_param;
	struct dtr_cm *cm;
	int err = -ENOMEM;

	cm = kzalloc(sizeof(*cm), GFP_KERNEL);
	if (!cm)
		goto out;

	err = dtr_create_cm_id(cm);
	if (err) {
		tr_err(transport, "rdma_create_id() failed %d\n", err);
		goto out;
	}

	err = rdma_resolve_addr(cm->id, NULL,
				(struct sockaddr *)&path->path.peer_addr,
				2000);
	if (err) {
		tr_err(transport, "rdma_resolve_addr error %d\n", err);
		goto out;
	}

	wait_event_interruptible(cm->state_wq,
				 cm->state >= ROUTE_RESOLVED);

	if (cm->state != ROUTE_RESOLVED)
		goto out; /* Happens if peer not reachable */

	/* Connect peer */
	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.retry_count = 10;

	err = rdma_connect(cm->id, &conn_param);
	if (err) {
		tr_err(transport, "rdma_connect error %d\n", err);
		goto out;
	}

	/* Make sure that we see an eventuall RDMA_CM_EVENT_REJECTED here (cm.state
	   is ERROR in that case). We can not wait for RDMA_CM_EVENT_ESTABLISHED since
	   that requires the peer to call accept.
	   -> would lead to distributed deadlock. */
	wait_event_interruptible_timeout(cm->state_wq,
					 cm->state != ROUTE_RESOLVED,
					 HZ/20);

	if (cm->state == ERROR)
		goto out;

	// pr_info("%s: rdma_connect successful\n", path->name);
	*ret_cm = cm;
	return 0;

out:
	/* TODO: Eventually wait longer, till no callback can come in. */
	dtr_free_cm(cm);
	return err;
}

static void dtr_destroy_listener(struct drbd_listener *generic_listener)
{
	struct dtr_listener *listener =
		container_of(generic_listener, struct dtr_listener, listener);

	rdma_destroy_id(listener->cm.id);
	kfree(listener);
}

static int dtr_create_listener(struct drbd_transport *transport, const struct sockaddr *addr, struct drbd_listener **ret_listener)
{
	struct dtr_listener *listener = NULL;
	struct sockaddr_storage my_addr;
	int err = -ENOMEM;

	my_addr = *(struct sockaddr_storage *)addr;
	listener = kzalloc(sizeof(*listener), GFP_KERNEL);
	if (!listener)
		goto out;

	err = dtr_create_cm_id(&listener->cm);
	if (err) {
		tr_err(transport, "rdma_create_id() failed\n");
		goto out;
	}

	err = rdma_bind_addr(listener->cm.id, (struct sockaddr *)&my_addr);
	if (err) {
		tr_err(transport, "rdma_bind_addr error %d\n", err);
		goto out;
	}

	err = rdma_listen(listener->cm.id, 3);
	if (err) {
		tr_err(transport, "rdma_listen error %d\n", err);
		goto out;
	}

	listener->listener.listen_addr = *(struct sockaddr_storage *)addr;
	listener->listener.destroy = dtr_destroy_listener;

	*ret_listener = &listener->listener;
	return 0;
out:
	if (listener && listener->cm.id)
		rdma_destroy_id(listener->cm.id);
	kfree(listener);
	return err;
}

static bool dtr_wait_connect_cond(struct dtr_waiter *waiter)
{
	struct drbd_listener *listener = waiter->waiter.listener;
	bool rv;

	spin_lock_bh(&listener->waiters_lock);
	rv = waiter->waiter.listener->pending_accepts > 0 || waiter->cm != NULL;
	spin_unlock_bh(&listener->waiters_lock);

	return rv;
}

static int dtr_wait_for_connect(struct dtr_waiter *waiter, struct dtr_cm **ret_cm)
{
	struct drbd_transport *transport = waiter->waiter.transport;
	struct dtr_cm *cm = NULL;
	struct sockaddr_storage *peer_addr;
	struct net_conf *nc;
	struct dtr_listener *listener =
		container_of(waiter->waiter.listener, struct dtr_listener, listener);
	struct rdma_conn_param conn_param;
	struct rdma_cm_id *cm_id = NULL;
	struct drbd_waiter *waiter2_gen;
	long timeo;
	int connect_int, err = 0;

	rcu_read_lock();
	nc = rcu_dereference(transport->net_conf);
	if (!nc) {
		rcu_read_unlock();
		return -EINVAL;
	}
	connect_int = nc->connect_int;
	rcu_read_unlock();

	timeo = connect_int * HZ;
	timeo += (prandom_u32() & 1) ? timeo / 7 : -timeo / 7; /* 28.5% random jotter */

retry:
	dtr_free_cm(cm);
	cm = NULL;

	timeo = wait_event_interruptible_timeout(waiter->waiter.wait, dtr_wait_connect_cond(waiter), timeo);
	if (timeo <= 0)
		return -EAGAIN;

	spin_lock_bh(&listener->listener.waiters_lock);
	if (waiter->cm) {
		cm = waiter->cm;
		waiter->cm = NULL;
	} else if (listener->listener.pending_accepts > 0) {
		listener->listener.pending_accepts--;

		cm_id = listener->child_cms; /* Get head from single linked list */
		listener->child_cms = cm_id->context;
		cm_id->context = NULL;
	}
	spin_unlock_bh(&listener->listener.waiters_lock);

	if (cm_id) {
		cm = kzalloc(sizeof(*cm), GFP_KERNEL);
		if (!cm)
			return -ENOMEM;

		cm->state = IDLE;
		init_waitqueue_head(&cm->state_wq);
		cm_id->context = cm;
		cm->id = cm_id;

		memset(&conn_param, 0, sizeof conn_param);
		conn_param.responder_resources = 1;
		conn_param.initiator_depth = 1;

		err = rdma_accept(cm->id, &conn_param);
		if (err) {
			tr_err(transport, "rdma_accept error %d\n", err);
			goto err;
		}

		peer_addr = &cm_id->route.addr.dst_addr;

		spin_lock_bh(&listener->listener.waiters_lock);
		waiter2_gen = drbd_find_waiter_by_addr(waiter->waiter.listener, peer_addr);

		if (waiter2_gen && waiter2_gen != &waiter->waiter) {
			struct dtr_waiter *waiter2 =
				container_of(waiter2_gen, struct dtr_waiter, waiter);

			if (waiter2->cm) {
				tr_err(waiter2->waiter.transport,
					 "Receiver busy; rejecting incoming connection\n");
				goto retry_locked;
			}
			/* pass it to the right waiter... */
			waiter2->cm = cm;
			cm = NULL;
			wake_up(&waiter2->waiter.wait);
			goto retry_locked;
		}
		spin_unlock_bh(&listener->listener.waiters_lock);

		if (!waiter2_gen) {
			struct sockaddr_in *from_sin, *to_sin;

			from_sin = (struct sockaddr_in *)&peer_addr;
			to_sin = (struct sockaddr_in *)&dtr_drbd_path(transport)->my_addr;
			tr_err(transport, "Closing unexpected connection from "
				 "%pI4 to port %u\n",
				 &from_sin->sin_addr,
				 be16_to_cpu(to_sin->sin_port));

			goto retry;
		}

		/* if waiter2_gen is not null and rdma_stream is also not null,
		   we know the connection is for us... return it with RC = success */
	}

	*ret_cm = cm;
	return 0;

err:
	dtr_free_cm(cm);
	return err;

retry_locked:
	spin_unlock_bh(&listener->listener.waiters_lock);
	goto retry;
}

static void dtr_connect_path_work_fn(struct work_struct *work)
{
	struct dtr_path *path = container_of(work, struct dtr_path, connect_work);
	struct drbd_rdma_transport *rdma_transport = path->rdma_transport;
	struct drbd_transport *transport = &rdma_transport->transport;
	struct dtr_waiter waiter;
	bool resolve_conflicts = false;
	enum drbd_stream i;
	int p, err;

	__dtr_uninit_path(path, false);

	waiter.waiter.transport = transport;
	waiter.cm = NULL;

	err = drbd_get_listener(&waiter.waiter,
				(struct sockaddr *)&path->path.my_addr,
				dtr_create_listener);
	if (err)
		goto out;

	while (true) {
		struct dtr_cm *cm = NULL;

		err = dtr_try_connect(path, &cm);
		if (err < 0 && err != -EAGAIN)
			goto out;

		if (cm) {
			resolve_conflicts = false;
			path->cm = cm;
			break;
		}

		err = dtr_wait_for_connect(&waiter, &cm);
		if (err < 0 && err != -EAGAIN)
			goto out;

		if (cm) {
			resolve_conflicts = true;
			path->cm = cm;
			break;
		}

		if (drbd_should_abort_listening(transport))
			goto out_eagain;

		if (test_bit(P_CANCEL_CONNECT, &path->flags))
			goto out_eagain;

	}

	drbd_put_listener(&waiter.waiter);

	for (i = DATA_STREAM; i <= CONTROL_STREAM ; i++)
		dtr_init_flow(&path->flow[i], path);
	dtr_path_alloc_rdma_res(path, transport);

	if (0) {
out_eagain:
		err = -EAGAIN;
	}
	if (err) {
out:
		drbd_put_listener(&waiter.waiter);
		dtr_uninit_path(path);
	}

	p = atomic_cmpxchg(&rdma_transport->first_path_connect_err, 1, err);
	if (!err) {
		__dtr_refill_rx_desc(path, DATA_STREAM);
		__dtr_refill_rx_desc(path, CONTROL_STREAM);
		err = dtr_send_flow_control_msg(path);
		if (err < 0) {
			tr_err(transport, "sending first flow_control_msg() failed\n");
			goto out_eagain;
		}

		if (p == 1) {
			/* I was the first! */

			if (resolve_conflicts)
				set_bit(RESOLVE_CONFLICTS, &transport->flags);
			else
				clear_bit(RESOLVE_CONFLICTS, &transport->flags);

			complete(&rdma_transport->connected);
		}
	}

	clear_bit(P_CONNECT_WORK, &path->flags);
	wake_up_all(&path->wq);
}

static int dtr_connect(struct drbd_transport *transport)
{
	struct drbd_rdma_transport *rdma_transport =
		container_of(transport, struct drbd_rdma_transport, transport);

	struct dtr_stream *data_stream = NULL, *control_stream = NULL;
	struct drbd_path *drbd_path;
	struct dtr_path *path;
	struct net_conf *nc;
	int timeout, n, err = -ENOMEM;

	n = 0;
	list_for_each_entry(drbd_path, &transport->paths, list)
		n++;

	if (n == 0)
		return -EDESTADDRREQ;

	data_stream = &rdma_transport->stream[DATA_STREAM];
	dtr_init_stream(data_stream, transport);

	control_stream = &rdma_transport->stream[CONTROL_STREAM];
	dtr_init_stream(control_stream, transport);

	strcpy(data_stream->name, "data");
	strcpy(control_stream->name, "control");

	rcu_read_lock();
	nc = rcu_dereference(transport->net_conf);

	timeout = nc->timeout * HZ / 10;
	rcu_read_unlock();

	data_stream->send_timeout = timeout;
	control_stream->send_timeout = timeout;

	atomic_set(&rdma_transport->first_path_connect_err, 1);
	init_completion(&rdma_transport->connected);

	rdma_transport->active = true;

	if (n == 1) {
		path = dtr_path(rdma_transport);
		set_bit(P_CONNECT_WORK, &path->flags);
		dtr_connect_path_work_fn(&path->connect_work);
	} else /* n > 1 */ {
		list_for_each_entry(drbd_path, &transport->paths, list) {
			path = container_of(drbd_path, struct dtr_path, path);
			set_bit(P_CONNECT_WORK, &path->flags);
			queue_work(dtr_work_queue, &path->connect_work);
		}
	}

	wait_for_completion_interruptible(&rdma_transport->connected);

	err = atomic_read(&rdma_transport->first_path_connect_err);

	if (err) {
		rdma_transport->active = false;

		list_for_each_entry(drbd_path, &transport->paths, list) {
			path = container_of(drbd_path, struct dtr_path, path);
			dtr_disconnect_path(path);
		}
	}

	return err;
}

static void dtr_set_rcvtimeo(struct drbd_transport *transport, enum drbd_stream stream, long timeout)
{
	struct drbd_rdma_transport *rdma_transport =
		container_of(transport, struct drbd_rdma_transport, transport);

	rdma_transport->stream[stream].recv_timeout = timeout;
}

static long dtr_get_rcvtimeo(struct drbd_transport *transport, enum drbd_stream stream)
{
	struct drbd_rdma_transport *rdma_transport =
		container_of(transport, struct drbd_rdma_transport, transport);

	return rdma_transport->stream[stream].recv_timeout;
}

static bool dtr_path_ok(struct dtr_path *path)
{
	struct dtr_cm *cm = path->cm;

	return cm && cm->id && cm->state == CONNECTED;
}

static bool dtr_transport_ok(struct drbd_transport *transport)
{
	struct drbd_path *drbd_path;

	list_for_each_entry(drbd_path, &transport->paths, list) {
		struct dtr_path *path = container_of(drbd_path, struct dtr_path, path);

		if (dtr_path_ok(path))
			return true;
	}
	return false;
}

static bool dtr_stream_ok(struct drbd_transport *transport, enum drbd_stream stream)
{
	return dtr_transport_ok(transport);
}

static void dtr_update_congested(struct drbd_transport *transport)
{
	struct drbd_path *drbd_path;
	bool congested = true;

	list_for_each_entry(drbd_path, &transport->paths, list) {
		struct dtr_path *path = container_of(drbd_path, struct dtr_path, path);
		struct dtr_flow *flow = &path->flow[DATA_STREAM];
		bool path_congested = false;
		int tx_descs_posted;

		if (!dtr_path_ok(path))
			continue;

		tx_descs_posted = atomic_read(&flow->tx_descs_posted);
		path_congested |= flow->tx_descs_max - tx_descs_posted < DESCS_LOW_LEVEL;
		path_congested |= atomic_read(&flow->peer_rx_descs) < DESCS_LOW_LEVEL;

		if (!path_congested) {
			congested = false;
			break;
		}
	}

	if (congested)
		set_bit(NET_CONGESTED, &transport->flags);
}

static int dtr_send_page(struct drbd_transport *transport, enum drbd_stream stream,
			 struct page *page, int offset, size_t size, unsigned msg_flags)
{
	struct drbd_rdma_transport *rdma_transport =
		container_of(transport, struct drbd_rdma_transport, transport);
	struct dtr_path *path = dtr_path(rdma_transport);
	struct drbd_rdma_tx_desc *tx_desc;
	int err;

	// pr_info("%s: in send_page, size: %zu\n", rdma_stream->name, size);

	if (!dtr_transport_ok(transport))
		return -ECONNRESET;

	tx_desc = kmalloc(sizeof(*tx_desc) + sizeof(struct ib_sge), GFP_NOIO);
	if (!tx_desc)
		return -ENOMEM;

	get_page(page); /* The put_page() is in dtr_tx_cq_event_handler() */
	tx_desc->type = SEND_PAGE;
	tx_desc->page = page;
	tx_desc->nr_sges = 1;
	tx_desc->stream_nr = stream;
	tx_desc->sge[0].length = size;
	tx_desc->sge[0].lkey = offset; /* abusing lkey fild. See dtr_post_tx_desc() */

	err = dtr_post_tx_desc(rdma_transport, tx_desc);
	if (err) {
		dtr_free_tx_desc(path, tx_desc);
		tx_desc = NULL;
	}

	if (stream == DATA_STREAM)
		dtr_update_congested(transport);

	return err;
}

#if SENDER_COMPACTS_BVECS
static int dtr_send_bio_part(struct drbd_rdma_transport *rdma_transport,
			     struct bio *bio, int start, int size_tx_desc, int sges)
{
	struct dtr_stream *rdma_stream = &rdma_transport->stream[DATA_STREAM];
	struct drbd_rdma_tx_desc *tx_desc;
	struct ib_device *device;
	DRBD_BIO_VEC_TYPE bvec;
	DRBD_ITER_TYPE iter;
	int i = 0, pos = 0, done = 0, err;

	if (!size_tx_desc)
		return 0;

	//tr_info(&rdma_transport->transport,
	//	"  dtr_send_bio_part(start = %d, size = %d, sges = %d)\n",
	//	start, size_tx_desc, sges);

	tx_desc = kmalloc(sizeof(*tx_desc) + sizeof(struct ib_sge) * sges, GFP_NOIO);
	if (!tx_desc)
		return -ENOMEM;

	tx_desc->type = SEND_BIO;
	tx_desc->bio = bio;
	tx_desc->nr_sges = sges;
	device = rdma_stream->cm.id->device;

	bio_for_each_segment(bvec, tx_desc->bio, iter) {
		struct page *page = bvec BVD bv_page;
		int offset = bvec BVD bv_offset;
		int size = bvec BVD bv_len;
		int shift = 0;
		get_page(page);

		if (pos < start || done == size_tx_desc) {
			if (done != size_tx_desc && pos + size > start) {
				shift = (start - pos);
			} else {
				pos += size;
				continue;
			}
		}

		pos += size;
		offset += shift;
		size = min(size - shift, size_tx_desc - done);

		//tr_info(&rdma_transport->transport,
		//	"   sge (i = %d, offset = %d, size = %d)\n",
		//	i, offset, size);

		tx_desc->sge[i].addr = ib_dma_map_page(device, page, offset, size, DMA_TO_DEVICE);
		tx_desc->sge[i].lkey = rdma_stream->dma_mr->lkey;
		tx_desc->sge[i].length = size;
		done += size;
		i++;
	}

	TR_ASSERT(&rdma_transport->transport, done == size_tx_desc);
	tx_desc->stream_nr = ST_DATA;

	err = dtr_post_tx_desc(rdma_stream, tx_desc);
	if (err) {
		bio_for_each_segment(bvec, tx_desc->bio, iter)
			put_page(bvec BVD bv_page);
	}

	return err;
}
#endif

static int dtr_send_zc_bio(struct drbd_transport *transport, struct bio *bio)
{
#if SENDER_COMPACTS_BVECS
	int start = 0, sges = 0, size_tx_desc = 0, remaining = 0, err;
#endif
	int err = -EINVAL;
	DRBD_BIO_VEC_TYPE bvec;
	DRBD_ITER_TYPE iter;

	//tr_info(transport, "in send_zc_bio, size: %d\n", bio->bi_size);

	if (!dtr_transport_ok(transport))
		return -ECONNRESET;

#if SENDER_COMPACTS_BVECS
	bio_for_each_segment(bvec, bio, iter) {
		size_tx_desc += bvec BVD bv_len;
		//tr_info(transport, " bvec len = %d\n", bvec BVD bv_len);
		if (size_tx_desc > DRBD_SOCKET_BUFFER_SIZE) {
			remaining = size_tx_desc - DRBD_SOCKET_BUFFER_SIZE;
			size_tx_desc = DRBD_SOCKET_BUFFER_SIZE;
		}
		sges++;
		if (size_tx_desc == DRBD_SOCKET_BUFFER_SIZE || sges == DTR_MAX_TX_SGES) {
			err = dtr_send_bio_part(rdma_transport, bio, start, size_tx_desc, sges);
			if (err)
				goto out;
			start += size_tx_desc;
			sges = 0;
			size_tx_desc = remaining;
			if (remaining) {
				sges++;
				remaining = 0;
			}
		}
	}
	err = dtr_send_bio_part(rdma_transport, bio, start, size_tx_desc, sges);
	start += size_tx_desc;

	TR_ASSERT(transport, start == DRBD_BIO_BI_SIZE(bio));
out:
#else
	bio_for_each_segment(bvec, bio, iter) {
		err = dtr_send_page(transport, DATA_STREAM,
			bvec BVD bv_page, bvec BVD bv_offset, bvec BVD bv_len,
			0 /* flags currently unused by dtr_send_page */);
		if (err)
			break;
	}
#endif
	if (1 /* stream == DATA_STREAM */)
		dtr_update_congested(transport);

	return err;
}

static bool dtr_hint(struct drbd_transport *transport, enum drbd_stream stream,
		enum drbd_tr_hints hint)
{
	switch (hint) {
	default: /* not implemented, but should not trigger error handling */
		return true;
	}
	return true;
}

static void dtr_debugfs_show_flow(struct dtr_flow *flow, struct seq_file *m)
{
	seq_printf(m,    "%-7s  field:  posted\t alloc\tdesired\t  max\n", flow->stream->name);
	seq_printf(m, "      tx_descs: %5d\t\t\t%5d\n", atomic_read(&flow->tx_descs_posted), flow->tx_descs_max);
	seq_printf(m, " peer_rx_descs: %5d (receive window at peer)\n", atomic_read(&flow->peer_rx_descs));
	seq_printf(m, "      rx_descs: %5d\t%5d\t%5d\t%5d\n", flow->rx_descs_posted, flow->rx_descs_allocated,
		   flow->rx_descs_want_posted, flow->rx_descs_max);
	seq_printf(m, " rx_peer_knows: %5d (what the peer knows about my recive window)\n\n",
		   atomic_read(&flow->rx_descs_known_to_peer));
}

static void dtr_debugfs_show_path(struct dtr_path *path, struct seq_file *m)
{
	enum drbd_stream i;

	seq_printf(m, "%pI4 - %pI4:\n", &((struct sockaddr_in *)&path->path.my_addr)->sin_addr,
		   &((struct sockaddr_in *)&path->path.peer_addr)->sin_addr);

	if (dtr_path_ok(path)) {
		for (i = DATA_STREAM; i <= CONTROL_STREAM ; i++)
			dtr_debugfs_show_flow(&path->flow[i], m);
	} else {
		seq_printf(m, " not connected\n");
	}
}

static void dtr_debugfs_show(struct drbd_transport *transport, struct seq_file *m)
{
	struct drbd_path *drbd_path;

	/* BUMP me if you change the file format/content/presentation */
	seq_printf(m, "v: %u\n\n", 0);

	list_for_each_entry(drbd_path, &transport->paths, list) {
		struct dtr_path *path = container_of(drbd_path, struct dtr_path, path);

		dtr_debugfs_show_path(path, m);
	}
}

static int dtr_add_path(struct drbd_transport *transport, struct drbd_path *drbd_path)
{
	struct drbd_rdma_transport *rdma_transport =
		container_of(transport, struct drbd_rdma_transport, transport);
	struct dtr_path *path = container_of(drbd_path, struct dtr_path, path);

	/* initialize private parts of path */
	path->rdma_transport = rdma_transport;
	INIT_WORK(&path->connect_work, dtr_connect_path_work_fn);
	init_waitqueue_head(&path->wq);

	list_add(&drbd_path->list, &transport->paths);

	if (rdma_transport->active)
		queue_work(dtr_work_queue, &path->connect_work);

	return 0;
}

static int dtr_remove_path(struct drbd_transport *transport, struct drbd_path *del_path)
{
	struct drbd_rdma_transport *rdma_transport =
		container_of(transport, struct drbd_rdma_transport, transport);
	struct drbd_path *drbd_path, *connected_path = NULL;
	int n = 0, connected = 0, match = 0;

	list_for_each_entry(drbd_path, &transport->paths, list) {
		struct dtr_path *path = container_of(drbd_path, struct dtr_path, path);

		if (dtr_path_ok(path)) {
			connected++;
			connected_path = drbd_path;
		}
		if (del_path == drbd_path)
			match++;
		n++;
	}

	if (rdma_transport->active &&
	    ((connected == 1 && connected_path == del_path) || n == 1))
		return -EBUSY;

	if (match) {
		struct dtr_path *path = container_of(drbd_path, struct dtr_path, path);

		list_del_init(&del_path->list);
		dtr_uninit_path(path);
		return 0;
	}

	return -ENOENT;
}

static int __init dtr_initialize(void)
{
	allocation_size = PAGE_SIZE;

	dtr_work_queue = alloc_workqueue("drbd_rdma", 0, 0);

	return drbd_register_transport_class(&rdma_transport_class,
					     DRBD_TRANSPORT_API_VERSION,
					     sizeof(struct drbd_transport));
}

static void __exit dtr_cleanup(void)
{
	destroy_workqueue(dtr_work_queue);
	drbd_unregister_transport_class(&rdma_transport_class);
}

module_init(dtr_initialize)
module_exit(dtr_cleanup)
