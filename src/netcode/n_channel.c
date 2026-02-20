/*
 * QUICKEN Engine - Reliable Ordered Channel
 *
 * Stop-and-wait reliable delivery layered on top of the unreliable
 * packet protocol. Only one unacked reliable message in flight at a time.
 *
 * Reliable message payload format:
 *   [reliable_sequence: u16][reliable_ack: u16][payload_data]
 *
 * An empty ack-only message has payload_data length 0.
 */

#include "n_internal.h"

void n_reliable_init(n_reliable_channel_t *channel) {
    memset(channel, 0, sizeof(*channel));
}

void n_reliable_send(n_reliable_channel_t *channel, const u8 *data, u16 len) {
    if (len > N_RELIABLE_MAX_PAYLOAD - 4) return; // 4 bytes for seq+ack header
    if (channel->has_unacked) return; // stop-and-wait: wait for ack first

    channel->reliable_sequence++;
    channel->unacked_sequence = channel->reliable_sequence;
    channel->unacked_len = len;
    if (len > 0) {
        memcpy(channel->unacked_buffer, data, len);
    }
    channel->has_unacked = true;
    channel->last_send_time = 0.0; // will be set when actually written to packet
}

bool n_reliable_needs_retransmit(const n_reliable_channel_t *channel, f64 now) {
    if (!channel->has_unacked) return false;
    if (channel->last_send_time <= 0.0) return true; // never sent
    return (now - channel->last_send_time) >= N_RELIABLE_RETRANSMIT_SEC;
}

void n_reliable_on_ack(n_reliable_channel_t *channel, u16 ack_seq) {
    if (channel->has_unacked && ack_seq == channel->unacked_sequence) {
        channel->has_unacked = false;
    }
}

void n_reliable_write_to_packet(n_reliable_channel_t *channel, n_bitwriter_t *writer, f64 now) {
    /*
     * Write a COMMAND message containing:
     *   [reliable_seq: u16][reliable_ack: u16][payload if unacked]
     *
     * If there is an unacked message to send/retransmit, include its payload.
     * Otherwise, just send the ack (empty payload).
     */
    bool send_payload = channel->has_unacked && n_reliable_needs_retransmit(channel, now);

    u16 payload_total = 4; // seq + ack
    if (send_payload) {
        payload_total += channel->unacked_len;
    }

    n_msg_header_write(writer, N_MSG_COMMAND, payload_total);
    if (send_payload) {
        n_write_u16(writer, channel->unacked_sequence);
    } else {
        n_write_u16(writer, 0); // no data sequence
    }
    n_write_u16(writer, channel->reliable_ack);

    if (send_payload) {
        for (u16 i = 0; i < channel->unacked_len; i++) {
            n_write_u8(writer, channel->unacked_buffer[i]);
        }
        channel->last_send_time = now;
    }
}

bool n_reliable_read_from_packet(n_reliable_channel_t *channel, n_bitreader_t *reader,
                                  u16 *out_len) {
    u16 remote_seq = n_read_u16(reader);
    u16 remote_ack = n_read_u16(reader);

    if (n_bitreader_overflowed(reader)) return false;

    // Process their ack of our messages
    n_reliable_on_ack(channel, remote_ack);

    *out_len = 0;

    // If remote_seq is 0, this is just an ack, no data
    if (remote_seq == 0) return true;

    // Check if this is a new message
    if (n_sequence_more_recent(remote_seq, channel->reliable_ack)) {
        channel->reliable_ack = remote_seq;
        return true; // caller should read remaining bytes as payload
    }

    // Duplicate, already received
    return true;
}
