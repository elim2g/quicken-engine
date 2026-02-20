/*
 * QUICKEN Engine - Bitpacker
 *
 * Write/read arbitrary bit counts to/from a byte buffer.
 * Little-endian bit order within each byte.
 */

#include "n_internal.h"

// --- Writer ---

void n_bitwriter_init(n_bitwriter_t *writer, u8 *buffer, u32 max_bytes) {
    writer->buffer = buffer;
    writer->bit_pos = 0;
    writer->max_bits = max_bytes * 8;
    memset(buffer, 0, max_bytes);
}

void n_write_bits(n_bitwriter_t *writer, u32 value, u32 num_bits) {
    QK_ASSERT(num_bits <= 32);
    if (writer->bit_pos + num_bits > writer->max_bits) {
        writer->bit_pos = writer->max_bits + 1; // mark overflow
        return;
    }

    u32 byte_idx = writer->bit_pos / 8;
    u32 bit_off  = writer->bit_pos % 8;

    // Merge value into the buffer starting at the current bit offset.
    // We write up to one byte at a time, filling from bit_off upward.
    u32 bits_remaining = num_bits;
    while (bits_remaining > 0) {
        u32 space = 8 - bit_off;
        u32 chunk = bits_remaining < space ? bits_remaining : space;
        u8 mask = (u8)((1u << chunk) - 1);
        writer->buffer[byte_idx] |= (u8)((value & mask) << bit_off);
        value >>= chunk;
        bits_remaining -= chunk;
        bit_off = 0;
        byte_idx++;
    }

    writer->bit_pos += num_bits;
}

void n_write_bool(n_bitwriter_t *writer, bool value) {
    n_write_bits(writer, value ? 1 : 0, 1);
}

void n_write_u8(n_bitwriter_t *writer, u8 value) {
    n_write_bits(writer, value, 8);
}

void n_write_u16(n_bitwriter_t *writer, u16 value) {
    n_write_bits(writer, value, 16);
}

void n_write_u32(n_bitwriter_t *writer, u32 value) {
    n_write_bits(writer, value, 32);
}

void n_write_i16(n_bitwriter_t *writer, i16 value) {
    n_write_bits(writer, (u32)(u16)value, 16);
}

void n_write_f64(n_bitwriter_t *writer, f64 value) {
    u64 bits;
    memcpy(&bits, &value, sizeof(bits));
    n_write_bits(writer, (u32)(bits & 0xFFFFFFFFu), 32);
    n_write_bits(writer, (u32)(bits >> 32), 32);
}

u32 n_bitwriter_bytes_written(const n_bitwriter_t *writer) {
    return (writer->bit_pos + 7) / 8;
}

// --- Reader ---

void n_bitreader_init(n_bitreader_t *reader, const u8 *buffer, u32 num_bytes) {
    reader->buffer = buffer;
    reader->bit_pos = 0;
    reader->max_bits = num_bytes * 8;
}

u32 n_read_bits(n_bitreader_t *reader, u32 num_bits) {
    QK_ASSERT(num_bits <= 32);
    if (reader->bit_pos + num_bits > reader->max_bits) {
        reader->bit_pos = reader->max_bits + 1; // mark overflow
        return 0;
    }

    u32 byte_idx = reader->bit_pos / 8;
    u32 bit_off  = reader->bit_pos % 8;
    u32 value = 0;
    u32 bits_read = 0;

    while (bits_read < num_bits) {
        u32 avail = 8 - bit_off;
        u32 chunk = (num_bits - bits_read) < avail ? (num_bits - bits_read) : avail;
        u8 mask = (u8)((1u << chunk) - 1);
        u32 extracted = (u32)((reader->buffer[byte_idx] >> bit_off) & mask);
        value |= extracted << bits_read;
        bits_read += chunk;
        bit_off = 0;
        byte_idx++;
    }

    reader->bit_pos += num_bits;
    return value;
}

bool n_read_bool(n_bitreader_t *reader) {
    return n_read_bits(reader, 1) != 0;
}

u8 n_read_u8(n_bitreader_t *reader) {
    return (u8)n_read_bits(reader, 8);
}

u16 n_read_u16(n_bitreader_t *reader) {
    return (u16)n_read_bits(reader, 16);
}

u32 n_read_u32(n_bitreader_t *reader) {
    return n_read_bits(reader, 32);
}

i16 n_read_i16(n_bitreader_t *reader) {
    return (i16)(u16)n_read_bits(reader, 16);
}

f64 n_read_f64(n_bitreader_t *reader) {
    u64 lo = n_read_bits(reader, 32);
    u64 hi = n_read_bits(reader, 32);
    u64 bits = lo | (hi << 32);
    f64 value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

bool n_bitreader_overflowed(const n_bitreader_t *reader) {
    return reader->bit_pos > reader->max_bits;
}
