/*
 * QUICKEN Engine - Bitpacker
 *
 * Write/read arbitrary bit counts to/from a byte buffer.
 * Little-endian bit order within each byte.
 */

#include "netcode/n_internal.h"

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

    for (u32 i = 0; i < num_bits; i++) {
        u32 bit = (value >> i) & 1;
        u32 byte_index = w->bit_pos / 8;
        u32 bit_index = w->bit_pos % 8;
        w->buffer[byte_index] |= (u8)(bit << bit_index);
        w->bit_pos++;
    }
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

    u32 value = 0;
    for (u32 i = 0; i < num_bits; i++) {
        u32 byte_index = r->bit_pos / 8;
        u32 bit_index = r->bit_pos % 8;
        u32 bit = (r->buffer[byte_index] >> bit_index) & 1;
        value |= (bit << i);
        r->bit_pos++;
    }
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
