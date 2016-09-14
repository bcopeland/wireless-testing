/* RxRPC packet reception
 *
 * Copyright (C) 2007, 2016 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/net.h>
#include <linux/skbuff.h>
#include <linux/errqueue.h>
#include <linux/udp.h>
#include <linux/in.h>
#include <linux/in6.h>
#include <linux/icmp.h>
#include <linux/gfp.h>
#include <net/sock.h>
#include <net/af_rxrpc.h>
#include <net/ip.h>
#include <net/udp.h>
#include <net/net_namespace.h>
#include "ar-internal.h"

static void rxrpc_proto_abort(const char *why,
			      struct rxrpc_call *call, rxrpc_seq_t seq)
{
	if (rxrpc_abort_call(why, call, seq, RX_PROTOCOL_ERROR, EBADMSG)) {
		set_bit(RXRPC_CALL_EV_ABORT, &call->events);
		rxrpc_queue_call(call);
	}
}

/*
 * Apply a hard ACK by advancing the Tx window.
 */
static void rxrpc_rotate_tx_window(struct rxrpc_call *call, rxrpc_seq_t to)
{
	struct sk_buff *skb, *list = NULL;
	int ix;

	spin_lock(&call->lock);

	while (before(call->tx_hard_ack, to)) {
		call->tx_hard_ack++;
		ix = call->tx_hard_ack & RXRPC_RXTX_BUFF_MASK;
		skb = call->rxtx_buffer[ix];
		rxrpc_see_skb(skb);
		call->rxtx_buffer[ix] = NULL;
		call->rxtx_annotations[ix] = 0;
		skb->next = list;
		list = skb;
	}

	spin_unlock(&call->lock);

	while (list) {
		skb = list;
		list = skb->next;
		skb->next = NULL;
		rxrpc_free_skb(skb);
	}
}

/*
 * End the transmission phase of a call.
 *
 * This occurs when we get an ACKALL packet, the first DATA packet of a reply,
 * or a final ACK packet.
 */
static bool rxrpc_end_tx_phase(struct rxrpc_call *call, const char *abort_why)
{
	_enter("");

	switch (call->state) {
	case RXRPC_CALL_CLIENT_RECV_REPLY:
		return true;
	case RXRPC_CALL_CLIENT_AWAIT_REPLY:
	case RXRPC_CALL_SERVER_AWAIT_ACK:
		break;
	default:
		rxrpc_proto_abort(abort_why, call, call->tx_top);
		return false;
	}

	rxrpc_rotate_tx_window(call, call->tx_top);

	write_lock(&call->state_lock);

	switch (call->state) {
	default:
		break;
	case RXRPC_CALL_CLIENT_AWAIT_REPLY:
		call->state = RXRPC_CALL_CLIENT_RECV_REPLY;
		break;
	case RXRPC_CALL_SERVER_AWAIT_ACK:
		__rxrpc_call_completed(call);
		rxrpc_notify_socket(call);
		break;
	}

	write_unlock(&call->state_lock);
	_leave(" = ok");
	return true;
}

/*
 * Scan a jumbo packet to validate its structure and to work out how many
 * subpackets it contains.
 *
 * A jumbo packet is a collection of consecutive packets glued together with
 * little headers between that indicate how to change the initial header for
 * each subpacket.
 *
 * RXRPC_JUMBO_PACKET must be set on all but the last subpacket - and all but
 * the last are RXRPC_JUMBO_DATALEN in size.  The last subpacket may be of any
 * size.
 */
static bool rxrpc_validate_jumbo(struct sk_buff *skb)
{
	struct rxrpc_skb_priv *sp = rxrpc_skb(skb);
	unsigned int offset = sp->offset;
	unsigned int len = skb->data_len;
	int nr_jumbo = 1;
	u8 flags = sp->hdr.flags;

	do {
		nr_jumbo++;
		if (len - offset < RXRPC_JUMBO_SUBPKTLEN)
			goto protocol_error;
		if (flags & RXRPC_LAST_PACKET)
			goto protocol_error;
		offset += RXRPC_JUMBO_DATALEN;
		if (skb_copy_bits(skb, offset, &flags, 1) < 0)
			goto protocol_error;
		offset += sizeof(struct rxrpc_jumbo_header);
	} while (flags & RXRPC_JUMBO_PACKET);

	sp->nr_jumbo = nr_jumbo;
	return true;

protocol_error:
	return false;
}

/*
 * Handle reception of a duplicate packet.
 *
 * We have to take care to avoid an attack here whereby we're given a series of
 * jumbograms, each with a sequence number one before the preceding one and
 * filled up to maximum UDP size.  If they never send us the first packet in
 * the sequence, they can cause us to have to hold on to around 2MiB of kernel
 * space until the call times out.
 *
 * We limit the space usage by only accepting three duplicate jumbo packets per
 * call.  After that, we tell the other side we're no longer accepting jumbos
 * (that information is encoded in the ACK packet).
 */
static void rxrpc_input_dup_data(struct rxrpc_call *call, rxrpc_seq_t seq,
				 u8 annotation, bool *_jumbo_dup)
{
	/* Discard normal packets that are duplicates. */
	if (annotation == 0)
		return;

	/* Skip jumbo subpackets that are duplicates.  When we've had three or
	 * more partially duplicate jumbo packets, we refuse to take any more
	 * jumbos for this call.
	 */
	if (!*_jumbo_dup) {
		call->nr_jumbo_dup++;
		*_jumbo_dup = true;
	}
}

/*
 * Process a DATA packet, adding the packet to the Rx ring.
 */
static void rxrpc_input_data(struct rxrpc_call *call, struct sk_buff *skb,
			     u16 skew)
{
	struct rxrpc_skb_priv *sp = rxrpc_skb(skb);
	unsigned int offset = sp->offset;
	unsigned int ix;
	rxrpc_serial_t serial = sp->hdr.serial, ack_serial = 0;
	rxrpc_seq_t seq = sp->hdr.seq, hard_ack;
	bool immediate_ack = false, jumbo_dup = false, queued;
	u16 len;
	u8 ack = 0, flags, annotation = 0;

	_enter("{%u,%u},{%u,%u}",
	       call->rx_hard_ack, call->rx_top, skb->data_len, seq);

	_proto("Rx DATA %%%u { #%u f=%02x }",
	       sp->hdr.serial, seq, sp->hdr.flags);

	if (call->state >= RXRPC_CALL_COMPLETE)
		return;

	/* Received data implicitly ACKs all of the request packets we sent
	 * when we're acting as a client.
	 */
	if (call->state == RXRPC_CALL_CLIENT_AWAIT_REPLY &&
	    !rxrpc_end_tx_phase(call, "ETD"))
		return;

	call->ackr_prev_seq = seq;

	hard_ack = READ_ONCE(call->rx_hard_ack);
	if (after(seq, hard_ack + call->rx_winsize)) {
		ack = RXRPC_ACK_EXCEEDS_WINDOW;
		ack_serial = serial;
		goto ack;
	}

	flags = sp->hdr.flags;
	if (flags & RXRPC_JUMBO_PACKET) {
		if (call->nr_jumbo_dup > 3) {
			ack = RXRPC_ACK_NOSPACE;
			ack_serial = serial;
			goto ack;
		}
		annotation = 1;
	}

next_subpacket:
	queued = false;
	ix = seq & RXRPC_RXTX_BUFF_MASK;
	len = skb->data_len;
	if (flags & RXRPC_JUMBO_PACKET)
		len = RXRPC_JUMBO_DATALEN;

	if (flags & RXRPC_LAST_PACKET) {
		if (test_and_set_bit(RXRPC_CALL_RX_LAST, &call->flags) &&
		    seq != call->rx_top)
			return rxrpc_proto_abort("LSN", call, seq);
	} else {
		if (test_bit(RXRPC_CALL_RX_LAST, &call->flags) &&
		    after_eq(seq, call->rx_top))
			return rxrpc_proto_abort("LSA", call, seq);
	}

	if (before_eq(seq, hard_ack)) {
		ack = RXRPC_ACK_DUPLICATE;
		ack_serial = serial;
		goto skip;
	}

	if (flags & RXRPC_REQUEST_ACK && !ack) {
		ack = RXRPC_ACK_REQUESTED;
		ack_serial = serial;
	}

	if (call->rxtx_buffer[ix]) {
		rxrpc_input_dup_data(call, seq, annotation, &jumbo_dup);
		if (ack != RXRPC_ACK_DUPLICATE) {
			ack = RXRPC_ACK_DUPLICATE;
			ack_serial = serial;
		}
		immediate_ack = true;
		goto skip;
	}

	/* Queue the packet.  We use a couple of memory barriers here as need
	 * to make sure that rx_top is perceived to be set after the buffer
	 * pointer and that the buffer pointer is set after the annotation and
	 * the skb data.
	 *
	 * Barriers against rxrpc_recvmsg_data() and rxrpc_rotate_rx_window()
	 * and also rxrpc_fill_out_ack().
	 */
	rxrpc_get_skb(skb);
	call->rxtx_annotations[ix] = annotation;
	smp_wmb();
	call->rxtx_buffer[ix] = skb;
	if (after(seq, call->rx_top))
		smp_store_release(&call->rx_top, seq);
	queued = true;

	if (after_eq(seq, call->rx_expect_next)) {
		if (after(seq, call->rx_expect_next)) {
			_net("OOS %u > %u", seq, call->rx_expect_next);
			ack = RXRPC_ACK_OUT_OF_SEQUENCE;
			ack_serial = serial;
		}
		call->rx_expect_next = seq + 1;
	}

skip:
	offset += len;
	if (flags & RXRPC_JUMBO_PACKET) {
		if (skb_copy_bits(skb, offset, &flags, 1) < 0)
			return rxrpc_proto_abort("XJF", call, seq);
		offset += sizeof(struct rxrpc_jumbo_header);
		seq++;
		serial++;
		annotation++;
		if (flags & RXRPC_JUMBO_PACKET)
			annotation |= RXRPC_RX_ANNO_JLAST;

		_proto("Rx DATA Jumbo %%%u", serial);
		goto next_subpacket;
	}

	if (queued && flags & RXRPC_LAST_PACKET && !ack) {
		ack = RXRPC_ACK_DELAY;
		ack_serial = serial;
	}

ack:
	if (ack)
		rxrpc_propose_ACK(call, ack, skew, ack_serial,
				  immediate_ack, true);

	if (sp->hdr.seq == READ_ONCE(call->rx_hard_ack) + 1)
		rxrpc_notify_socket(call);
	_leave(" [queued]");
}

/*
 * Process the extra information that may be appended to an ACK packet
 */
static void rxrpc_input_ackinfo(struct rxrpc_call *call, struct sk_buff *skb,
				struct rxrpc_ackinfo *ackinfo)
{
	struct rxrpc_skb_priv *sp = rxrpc_skb(skb);
	struct rxrpc_peer *peer;
	unsigned int mtu;

	_proto("Rx ACK %%%u Info { rx=%u max=%u rwin=%u jm=%u }",
	       sp->hdr.serial,
	       ntohl(ackinfo->rxMTU), ntohl(ackinfo->maxMTU),
	       ntohl(ackinfo->rwind), ntohl(ackinfo->jumbo_max));

	if (call->tx_winsize > ntohl(ackinfo->rwind))
		call->tx_winsize = ntohl(ackinfo->rwind);

	mtu = min(ntohl(ackinfo->rxMTU), ntohl(ackinfo->maxMTU));

	peer = call->peer;
	if (mtu < peer->maxdata) {
		spin_lock_bh(&peer->lock);
		peer->maxdata = mtu;
		peer->mtu = mtu + peer->hdrsize;
		spin_unlock_bh(&peer->lock);
		_net("Net MTU %u (maxdata %u)", peer->mtu, peer->maxdata);
	}
}

/*
 * Process individual soft ACKs.
 *
 * Each ACK in the array corresponds to one packet and can be either an ACK or
 * a NAK.  If we get find an explicitly NAK'd packet we resend immediately;
 * packets that lie beyond the end of the ACK list are scheduled for resend by
 * the timer on the basis that the peer might just not have processed them at
 * the time the ACK was sent.
 */
static void rxrpc_input_soft_acks(struct rxrpc_call *call, u8 *acks,
				  rxrpc_seq_t seq, int nr_acks)
{
	bool resend = false;
	int ix;

	for (; nr_acks > 0; nr_acks--, seq++) {
		ix = seq & RXRPC_RXTX_BUFF_MASK;
		switch (*acks) {
		case RXRPC_ACK_TYPE_ACK:
			call->rxtx_annotations[ix] = RXRPC_TX_ANNO_ACK;
			break;
		case RXRPC_ACK_TYPE_NACK:
			if (call->rxtx_annotations[ix] == RXRPC_TX_ANNO_NAK)
				continue;
			call->rxtx_annotations[ix] = RXRPC_TX_ANNO_NAK;
			resend = true;
			break;
		default:
			return rxrpc_proto_abort("SFT", call, 0);
		}
	}

	if (resend &&
	    !test_and_set_bit(RXRPC_CALL_EV_RESEND, &call->events))
		rxrpc_queue_call(call);
}

/*
 * Process an ACK packet.
 *
 * ack.firstPacket is the sequence number of the first soft-ACK'd/NAK'd packet
 * in the ACK array.  Anything before that is hard-ACK'd and may be discarded.
 *
 * A hard-ACK means that a packet has been processed and may be discarded; a
 * soft-ACK means that the packet may be discarded and retransmission
 * requested.  A phase is complete when all packets are hard-ACK'd.
 */
static void rxrpc_input_ack(struct rxrpc_call *call, struct sk_buff *skb,
			    u16 skew)
{
	struct rxrpc_skb_priv *sp = rxrpc_skb(skb);
	union {
		struct rxrpc_ackpacket ack;
		struct rxrpc_ackinfo info;
		u8 acks[RXRPC_MAXACKS];
	} buf;
	rxrpc_seq_t first_soft_ack, hard_ack;
	int nr_acks, offset;

	_enter("");

	if (skb_copy_bits(skb, sp->offset, &buf.ack, sizeof(buf.ack)) < 0) {
		_debug("extraction failure");
		return rxrpc_proto_abort("XAK", call, 0);
	}
	sp->offset += sizeof(buf.ack);

	first_soft_ack = ntohl(buf.ack.firstPacket);
	hard_ack = first_soft_ack - 1;
	nr_acks = buf.ack.nAcks;

	_proto("Rx ACK %%%u { m=%hu f=#%u p=#%u s=%%%u r=%s n=%u }",
	       sp->hdr.serial,
	       ntohs(buf.ack.maxSkew),
	       first_soft_ack,
	       ntohl(buf.ack.previousPacket),
	       ntohl(buf.ack.serial),
	       rxrpc_acks(buf.ack.reason),
	       buf.ack.nAcks);

	if (buf.ack.reason == RXRPC_ACK_PING) {
		_proto("Rx ACK %%%u PING Request", sp->hdr.serial);
		rxrpc_propose_ACK(call, RXRPC_ACK_PING_RESPONSE,
				  skew, sp->hdr.serial, true, true);
	} else if (sp->hdr.flags & RXRPC_REQUEST_ACK) {
		rxrpc_propose_ACK(call, RXRPC_ACK_REQUESTED,
				  skew, sp->hdr.serial, true, true);
	}

	offset = sp->offset + nr_acks + 3;
	if (skb->data_len >= offset + sizeof(buf.info)) {
		if (skb_copy_bits(skb, offset, &buf.info, sizeof(buf.info)) < 0)
			return rxrpc_proto_abort("XAI", call, 0);
		rxrpc_input_ackinfo(call, skb, &buf.info);
	}

	if (first_soft_ack == 0)
		return rxrpc_proto_abort("AK0", call, 0);

	/* Ignore ACKs unless we are or have just been transmitting. */
	switch (call->state) {
	case RXRPC_CALL_CLIENT_SEND_REQUEST:
	case RXRPC_CALL_CLIENT_AWAIT_REPLY:
	case RXRPC_CALL_SERVER_SEND_REPLY:
	case RXRPC_CALL_SERVER_AWAIT_ACK:
		break;
	default:
		return;
	}

	/* Discard any out-of-order or duplicate ACKs. */
	if ((int)sp->hdr.serial - (int)call->acks_latest <= 0) {
		_debug("discard ACK %d <= %d",
		       sp->hdr.serial, call->acks_latest);
		return;
	}
	call->acks_latest = sp->hdr.serial;

	if (test_bit(RXRPC_CALL_TX_LAST, &call->flags) &&
	    hard_ack == call->tx_top) {
		rxrpc_end_tx_phase(call, "ETA");
		return;
	}

	if (before(hard_ack, call->tx_hard_ack) ||
	    after(hard_ack, call->tx_top))
		return rxrpc_proto_abort("AKW", call, 0);

	if (after(hard_ack, call->tx_hard_ack))
		rxrpc_rotate_tx_window(call, hard_ack);

	if (after(first_soft_ack, call->tx_top))
		return;

	if (nr_acks > call->tx_top - first_soft_ack + 1)
		nr_acks = first_soft_ack - call->tx_top + 1;
	if (skb_copy_bits(skb, sp->offset, buf.acks, nr_acks) < 0)
		return rxrpc_proto_abort("XSA", call, 0);
	rxrpc_input_soft_acks(call, buf.acks, first_soft_ack, nr_acks);
}

/*
 * Process an ACKALL packet.
 */
static void rxrpc_input_ackall(struct rxrpc_call *call, struct sk_buff *skb)
{
	struct rxrpc_skb_priv *sp = rxrpc_skb(skb);

	_proto("Rx ACKALL %%%u", sp->hdr.serial);

	rxrpc_end_tx_phase(call, "ETL");
}

/*
 * Process an ABORT packet.
 */
static void rxrpc_input_abort(struct rxrpc_call *call, struct sk_buff *skb)
{
	struct rxrpc_skb_priv *sp = rxrpc_skb(skb);
	__be32 wtmp;
	u32 abort_code = RX_CALL_DEAD;

	_enter("");

	if (skb->len >= 4 &&
	    skb_copy_bits(skb, sp->offset, &wtmp, sizeof(wtmp)) >= 0)
		abort_code = ntohl(wtmp);

	_proto("Rx ABORT %%%u { %x }", sp->hdr.serial, abort_code);

	if (rxrpc_set_call_completion(call, RXRPC_CALL_REMOTELY_ABORTED,
				      abort_code, ECONNABORTED))
		rxrpc_notify_socket(call);
}

/*
 * Process an incoming call packet.
 */
static void rxrpc_input_call_packet(struct rxrpc_call *call,
				    struct sk_buff *skb, u16 skew)
{
	struct rxrpc_skb_priv *sp = rxrpc_skb(skb);

	_enter("%p,%p", call, skb);

	switch (sp->hdr.type) {
	case RXRPC_PACKET_TYPE_DATA:
		rxrpc_input_data(call, skb, skew);
		break;

	case RXRPC_PACKET_TYPE_ACK:
		rxrpc_input_ack(call, skb, skew);
		break;

	case RXRPC_PACKET_TYPE_BUSY:
		_proto("Rx BUSY %%%u", sp->hdr.serial);

		/* Just ignore BUSY packets from the server; the retry and
		 * lifespan timers will take care of business.  BUSY packets
		 * from the client don't make sense.
		 */
		break;

	case RXRPC_PACKET_TYPE_ABORT:
		rxrpc_input_abort(call, skb);
		break;

	case RXRPC_PACKET_TYPE_ACKALL:
		rxrpc_input_ackall(call, skb);
		break;

	default:
		_proto("Rx %s %%%u", rxrpc_pkts[sp->hdr.type], sp->hdr.serial);
		break;
	}

	_leave("");
}

/*
 * post connection-level events to the connection
 * - this includes challenges, responses, some aborts and call terminal packet
 *   retransmission.
 */
static void rxrpc_post_packet_to_conn(struct rxrpc_connection *conn,
				      struct sk_buff *skb)
{
	_enter("%p,%p", conn, skb);

	skb_queue_tail(&conn->rx_queue, skb);
	rxrpc_queue_conn(conn);
}

/*
 * post endpoint-level events to the local endpoint
 * - this includes debug and version messages
 */
static void rxrpc_post_packet_to_local(struct rxrpc_local *local,
				       struct sk_buff *skb)
{
	_enter("%p,%p", local, skb);

	skb_queue_tail(&local->event_queue, skb);
	rxrpc_queue_local(local);
}

/*
 * put a packet up for transport-level abort
 */
static void rxrpc_reject_packet(struct rxrpc_local *local, struct sk_buff *skb)
{
	CHECK_SLAB_OKAY(&local->usage);

	skb_queue_tail(&local->reject_queue, skb);
	rxrpc_queue_local(local);
}

/*
 * Extract the wire header from a packet and translate the byte order.
 */
static noinline
int rxrpc_extract_header(struct rxrpc_skb_priv *sp, struct sk_buff *skb)
{
	struct rxrpc_wire_header whdr;

	/* dig out the RxRPC connection details */
	if (skb_copy_bits(skb, 0, &whdr, sizeof(whdr)) < 0)
		return -EBADMSG;

	memset(sp, 0, sizeof(*sp));
	sp->hdr.epoch		= ntohl(whdr.epoch);
	sp->hdr.cid		= ntohl(whdr.cid);
	sp->hdr.callNumber	= ntohl(whdr.callNumber);
	sp->hdr.seq		= ntohl(whdr.seq);
	sp->hdr.serial		= ntohl(whdr.serial);
	sp->hdr.flags		= whdr.flags;
	sp->hdr.type		= whdr.type;
	sp->hdr.userStatus	= whdr.userStatus;
	sp->hdr.securityIndex	= whdr.securityIndex;
	sp->hdr._rsvd		= ntohs(whdr._rsvd);
	sp->hdr.serviceId	= ntohs(whdr.serviceId);
	sp->offset = sizeof(whdr);
	return 0;
}

/*
 * handle data received on the local endpoint
 * - may be called in interrupt context
 *
 * The socket is locked by the caller and this prevents the socket from being
 * shut down and the local endpoint from going away, thus sk_user_data will not
 * be cleared until this function returns.
 */
void rxrpc_data_ready(struct sock *udp_sk)
{
	struct rxrpc_connection *conn;
	struct rxrpc_channel *chan;
	struct rxrpc_call *call;
	struct rxrpc_skb_priv *sp;
	struct rxrpc_local *local = udp_sk->sk_user_data;
	struct sk_buff *skb;
	unsigned int channel;
	int ret, skew;

	_enter("%p", udp_sk);

	ASSERT(!irqs_disabled());

	skb = skb_recv_datagram(udp_sk, 0, 1, &ret);
	if (!skb) {
		if (ret == -EAGAIN)
			return;
		_debug("UDP socket error %d", ret);
		return;
	}

	rxrpc_new_skb(skb);

	_net("recv skb %p", skb);

	/* we'll probably need to checksum it (didn't call sock_recvmsg) */
	if (skb_checksum_complete(skb)) {
		rxrpc_free_skb(skb);
		__UDP_INC_STATS(&init_net, UDP_MIB_INERRORS, 0);
		_leave(" [CSUM failed]");
		return;
	}

	__UDP_INC_STATS(&init_net, UDP_MIB_INDATAGRAMS, 0);

	/* The socket buffer we have is owned by UDP, with UDP's data all over
	 * it, but we really want our own data there.
	 */
	skb_orphan(skb);
	sp = rxrpc_skb(skb);

	_net("Rx UDP packet from %08x:%04hu",
	     ntohl(ip_hdr(skb)->saddr), ntohs(udp_hdr(skb)->source));

	/* dig out the RxRPC connection details */
	if (rxrpc_extract_header(sp, skb) < 0)
		goto bad_message;
	trace_rxrpc_rx_packet(sp);

	_net("Rx RxRPC %s ep=%x call=%x:%x",
	     sp->hdr.flags & RXRPC_CLIENT_INITIATED ? "ToServer" : "ToClient",
	     sp->hdr.epoch, sp->hdr.cid, sp->hdr.callNumber);

	if (sp->hdr.type >= RXRPC_N_PACKET_TYPES ||
	    !((RXRPC_SUPPORTED_PACKET_TYPES >> sp->hdr.type) & 1)) {
		_proto("Rx Bad Packet Type %u", sp->hdr.type);
		goto bad_message;
	}

	switch (sp->hdr.type) {
	case RXRPC_PACKET_TYPE_VERSION:
		rxrpc_post_packet_to_local(local, skb);
		goto out;

	case RXRPC_PACKET_TYPE_BUSY:
		if (sp->hdr.flags & RXRPC_CLIENT_INITIATED)
			goto discard;

	case RXRPC_PACKET_TYPE_DATA:
		if (sp->hdr.callNumber == 0)
			goto bad_message;
		if (sp->hdr.flags & RXRPC_JUMBO_PACKET &&
		    !rxrpc_validate_jumbo(skb))
			goto bad_message;
		break;
	}

	rcu_read_lock();

	conn = rxrpc_find_connection_rcu(local, skb);
	if (conn) {
		if (sp->hdr.securityIndex != conn->security_ix)
			goto wrong_security;

		if (sp->hdr.callNumber == 0) {
			/* Connection-level packet */
			_debug("CONN %p {%d}", conn, conn->debug_id);
			rxrpc_post_packet_to_conn(conn, skb);
			goto out_unlock;
		}

		/* Note the serial number skew here */
		skew = (int)sp->hdr.serial - (int)conn->hi_serial;
		if (skew >= 0) {
			if (skew > 0)
				conn->hi_serial = sp->hdr.serial;
		} else {
			skew = -skew;
			skew = min(skew, 65535);
		}

		/* Call-bound packets are routed by connection channel. */
		channel = sp->hdr.cid & RXRPC_CHANNELMASK;
		chan = &conn->channels[channel];

		/* Ignore really old calls */
		if (sp->hdr.callNumber < chan->last_call)
			goto discard_unlock;

		if (sp->hdr.callNumber == chan->last_call) {
			/* For the previous service call, if completed successfully, we
			 * discard all further packets.
			 */
			if (rxrpc_conn_is_service(conn) &&
			    (chan->last_type == RXRPC_PACKET_TYPE_ACK ||
			     sp->hdr.type == RXRPC_PACKET_TYPE_ABORT))
				goto discard_unlock;

			/* But otherwise we need to retransmit the final packet from
			 * data cached in the connection record.
			 */
			rxrpc_post_packet_to_conn(conn, skb);
			goto out_unlock;
		}

		call = rcu_dereference(chan->call);
	} else {
		skew = 0;
		call = NULL;
	}

	if (!call || atomic_read(&call->usage) == 0) {
		if (!(sp->hdr.type & RXRPC_CLIENT_INITIATED) ||
		    sp->hdr.callNumber == 0 ||
		    sp->hdr.type != RXRPC_PACKET_TYPE_DATA)
			goto bad_message_unlock;
		if (sp->hdr.seq != 1)
			goto discard_unlock;
		call = rxrpc_new_incoming_call(local, conn, skb);
		if (!call) {
			rcu_read_unlock();
			goto reject_packet;
		}
	}

	rxrpc_input_call_packet(call, skb, skew);
	goto discard_unlock;

discard_unlock:
	rcu_read_unlock();
discard:
	rxrpc_free_skb(skb);
out:
	trace_rxrpc_rx_done(0, 0);
	return;

out_unlock:
	rcu_read_unlock();
	goto out;

wrong_security:
	rcu_read_unlock();
	trace_rxrpc_abort("SEC", sp->hdr.cid, sp->hdr.callNumber, sp->hdr.seq,
			  RXKADINCONSISTENCY, EBADMSG);
	skb->priority = RXKADINCONSISTENCY;
	goto post_abort;

bad_message_unlock:
	rcu_read_unlock();
bad_message:
	trace_rxrpc_abort("BAD", sp->hdr.cid, sp->hdr.callNumber, sp->hdr.seq,
			  RX_PROTOCOL_ERROR, EBADMSG);
	skb->priority = RX_PROTOCOL_ERROR;
post_abort:
	skb->mark = RXRPC_SKB_MARK_LOCAL_ABORT;
reject_packet:
	trace_rxrpc_rx_done(skb->mark, skb->priority);
	rxrpc_reject_packet(local, skb);
	_leave(" [badmsg]");
}
