/*
 * QUICKEN Engine - Packet Protocol
 *
 * Packet header encode/decode (8 bytes: sequence, ack, ack_bitfield).
 * Message framing: 4-bit type + 12-bit length prefix.
 */

#include "n_internal.h"

/* ---- Packet header (8 bytes, little-endian) ---- */

void n_packet_header_write(u8 *buf, const n_packet_header_t *hdr) {
    buf[0] = (u8)(hdr->sequence & 0xFF);
    buf[1] = (u8)(hdr->sequence >> 8);
    buf[2] = (u8)(hdr->ack & 0xFF);
    buf[3] = (u8)(hdr->ack >> 8);
    buf[4] = (u8)(hdr->ack_bitfield & 0xFF);
    buf[5] = (u8)((hdr->ack_bitfield >> 8) & 0xFF);
    buf[6] = (u8)((hdr->ack_bitfield >> 16) & 0xFF);
    buf[7] = (u8)((hdr->ack_bitfield >> 24) & 0xFF);
}

void n_packet_header_read(const u8 *buf, n_packet_header_t *hdr) {
    hdr->sequence = (u16)buf[0] | ((u16)buf[1] << 8);
    hdr->ack = (u16)buf[2] | ((u16)buf[3] << 8);
    hdr->ack_bitfield = (u32)buf[4]
                      | ((u32)buf[5] << 8)
                      | ((u32)buf[6] << 16)
                      | ((u32)buf[7] << 24);
}

/* ---- Message framing ---- */
/* 16 bits total: [type: 4 bits][length: 12 bits] */

void n_msg_header_write(n_bitwriter_t *w, u8 type, u16 length) {
    u32 combined = ((u32)type & 0xF) | (((u32)length & 0xFFF) << 4);
    n_write_bits(w, combined, 16);
}

bool n_msg_header_read(n_bitreader_t *r, n_msg_header_t *hdr) {
    if (n_bitreader_overflowed(r)) return false;

    u32 combined = n_read_bits(r, 16);
    if (n_bitreader_overflowed(r)) return false;

    hdr->type = (u8)(combined & 0xF);
    hdr->length = (u16)((combined >> 4) & 0xFFF);
    return true;
}
