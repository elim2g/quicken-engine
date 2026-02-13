/*
 * QUICKEN Engine - Bitpacker
 *
 * Write/read arbitrary bit counts to/from a byte buffer.
 * Little-endian bit order within each byte.
 */

#include "n_internal.h"

/* ---- Writer ---- */

void n_bitwriter_init(n_bitwriter_t *w, u8 *buffer, u32 max_bytes) {
    w->buffer = buffer;
    w->bit_pos = 0;
    w->max_bits = max_bytes * 8;
    memset(buffer, 0, max_bytes);
}

void n_write_bits(n_bitwriter_t *w, u32 value, u32 num_bits) {
    QK_ASSERT(num_bits <= 32);
    if (w->bit_pos + num_bits > w->max_bits) {
        w->bit_pos = w->max_bits + 1; /* mark overflow */
        return;
    }

    u32 byte_idx = w->bit_pos / 8;
    u32 bit_off  = w->bit_pos % 8;

    /* Merge value into the buffer starting at the current bit offset.
     * We write up to one byte at a time, filling from bit_off upward. */
    u32 bits_remaining = num_bits;
    while (bits_remaining > 0) {
        u32 space = 8 - bit_off;
        u32 chunk = bits_remaining < space ? bits_remaining : space;
        u8 mask = (u8)((1u << chunk) - 1);
        w->buffer[byte_idx] |= (u8)((value & mask) << bit_off);
        value >>= chunk;
        bits_remaining -= chunk;
        bit_off = 0;
        byte_idx++;
    }

    w->bit_pos += num_bits;
}

void n_write_bool(n_bitwriter_t *w, bool value) {
    n_write_bits(w, value ? 1 : 0, 1);
}

void n_write_u8(n_bitwriter_t *w, u8 value) {
    n_write_bits(w, value, 8);
}

void n_write_u16(n_bitwriter_t *w, u16 value) {
    n_write_bits(w, value, 16);
}

void n_write_u32(n_bitwriter_t *w, u32 value) {
    n_write_bits(w, value, 32);
}

void n_write_i16(n_bitwriter_t *w, i16 value) {
    n_write_bits(w, (u32)(u16)value, 16);
}

void n_write_f64(n_bitwriter_t *w, f64 value) {
    u64 bits;
    memcpy(&bits, &value, sizeof(bits));
    n_write_bits(w, (u32)(bits & 0xFFFFFFFFu), 32);
    n_write_bits(w, (u32)(bits >> 32), 32);
}

u32 n_bitwriter_bytes_written(const n_bitwriter_t *w) {
    return (w->bit_pos + 7) / 8;
}

/* ---- Reader ---- */

void n_bitreader_init(n_bitreader_t *r, const u8 *buffer, u32 num_bytes) {
    r->buffer = buffer;
    r->bit_pos = 0;
    r->max_bits = num_bytes * 8;
}

u32 n_read_bits(n_bitreader_t *r, u32 num_bits) {
    QK_ASSERT(num_bits <= 32);
    if (r->bit_pos + num_bits > r->max_bits) {
        r->bit_pos = r->max_bits + 1; /* mark overflow */
        return 0;
    }

    u32 byte_idx = r->bit_pos / 8;
    u32 bit_off  = r->bit_pos % 8;
    u32 value = 0;
    u32 bits_read = 0;

    while (bits_read < num_bits) {
        u32 avail = 8 - bit_off;
        u32 chunk = (num_bits - bits_read) < avail ? (num_bits - bits_read) : avail;
        u8 mask = (u8)((1u << chunk) - 1);
        u32 extracted = (u32)((r->buffer[byte_idx] >> bit_off) & mask);
        value |= extracted << bits_read;
        bits_read += chunk;
        bit_off = 0;
        byte_idx++;
    }

    r->bit_pos += num_bits;
    return value;
}

bool n_read_bool(n_bitreader_t *r) {
    return n_read_bits(r, 1) != 0;
}

u8 n_read_u8(n_bitreader_t *r) {
    return (u8)n_read_bits(r, 8);
}

u16 n_read_u16(n_bitreader_t *r) {
    return (u16)n_read_bits(r, 16);
}

u32 n_read_u32(n_bitreader_t *r) {
    return n_read_bits(r, 32);
}

i16 n_read_i16(n_bitreader_t *r) {
    return (i16)(u16)n_read_bits(r, 16);
}

f64 n_read_f64(n_bitreader_t *r) {
    u64 lo = n_read_bits(r, 32);
    u64 hi = n_read_bits(r, 32);
    u64 bits = lo | (hi << 32);
    f64 value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

bool n_bitreader_overflowed(const n_bitreader_t *r) {
    return r->bit_pos > r->max_bits;
}
