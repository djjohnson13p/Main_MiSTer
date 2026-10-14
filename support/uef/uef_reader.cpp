/*--------------------------------------------------------------------
 *  Modified for MiSTer by alanswx 2022
 *
 *  Originally from Replay Firmware
*/

/*--------------------------------------------------------------------
 *                       Replay Firmware
 *                      www.fpgaarcade.com
 *                     All rights reserved.
 *
 *                     admin@fpgaarcade.com
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *--------------------------------------------------------------------
 *
 * Copyright (c) 2020, The FPGAArcade community (see AUTHORS.txt)
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>
#include <fcntl.h>
#include <time.h>
#include <assert.h>

#include "../../file_io.h"
#include "../../user_io.h"
#include "../../menu.h"
#include "zlib.h"

#define UEF_ChunkHeaderSize (sizeof(uint16_t) + sizeof(uint32_t))
#define UEF_infoID      0x0000
#define UEF_tapeID      0x0100
#define UEF_highToneID  0x0110
#define UEF_highDummyID 0x0111
#define UEF_gapID       0x0112
#define UEF_freqChgID   0x0113
#define UEF_securityID  0x0114
#define UEF_floatGapID  0x0116
#define UEF_startBit    0
#define UEF_stopBit     1
#define UEF_Baud        (1000000.0/(16.0*52.0))

typedef struct {
    // UEF header
    uint16_t    id;
    uint32_t    length;
    // chunk data
    uint32_t    file_offset;
    uint32_t    bit_offset_start;
    uint32_t    bit_offset_end;
    uint32_t    pre_carrier;
} __attribute__((packed)) ChunkInfo;

// A send owns its parser cache and error state; no failed read becomes padding.
struct UefReadContext {
    FILE *file;
    uint32_t size;
    uint32_t bits;
    bool error;
    ChunkInfo chunk;
};

static bool UefSeek(UefReadContext &ctx, uint32_t offset)
{
    if (offset > ctx.size || fseek(ctx.file, offset, SEEK_SET)) ctx.error = true;
    return !ctx.error;
}

static bool UefRead(UefReadContext &ctx, void *data, size_t length)
{
    long pos = ftell(ctx.file);
    if (pos < 0 || (uint64_t)pos + length > ctx.size || fread(data, 1, length, ctx.file) != length)
        ctx.error = true;
    return !ctx.error;
}

static ChunkInfo* GetChunkAtPosFile(UefReadContext &ctx, uint32_t* p_bit_pos)
{
    uint32_t bit_pos = *p_bit_pos;
    bool length_check = bit_pos == UINT32_MAX;
    ChunkInfo *chunk = &ctx.chunk;
    if (chunk->bit_offset_start <= bit_pos && bit_pos < chunk->bit_offset_end) {
        *p_bit_pos = bit_pos - chunk->bit_offset_start;
        return chunk;
    }
    uint32_t chunk_start = 0;
    if (chunk->bit_offset_end && bit_pos >= chunk->bit_offset_end) {
        chunk_start = chunk->bit_offset_end;
        bit_pos -= chunk_start;
        if (!UefSeek(ctx, chunk->file_offset + chunk->length)) return NULL;
    } else if (!UefSeek(ctx, 12)) return NULL;
    chunk->bit_offset_end = 0;
    while (!ctx.error) {
        long position = ftell(ctx.file);
        if (position < 0) { ctx.error = true; break; }
        if ((uint32_t)position == ctx.size) break;
        if (!UefRead(ctx, chunk, UEF_ChunkHeaderSize)) break;
        chunk->file_offset = position + UEF_ChunkHeaderSize;
        if (chunk->length > ctx.size - chunk->file_offset) { ctx.error = true; break; }
        uint64_t chunk_bitlen = 0;
        uint16_t id = chunk->id;
        if (id == UEF_tapeID) {
            chunk_bitlen = (uint64_t)chunk->length * 10;
        } else if (id == UEF_gapID || id == UEF_highToneID || id == UEF_highDummyID) {
            uint16_t ms = 0;
            size_t needed = id == UEF_highDummyID ? 4 : 2;
            if (chunk->length < needed || !UefRead(ctx, &ms, sizeof(ms))) { ctx.error = true; break; }
            chunk_bitlen = ms * (UEF_Baud / 1000.0);
            if (id == UEF_highDummyID) {
                chunk->pre_carrier = chunk_bitlen;
                if (!UefRead(ctx, &ms, sizeof(ms))) break;
                chunk_bitlen += 20 + (uint32_t)(ms * (UEF_Baud / 1000.0));
            }
        } else if (length_check) {
            if (id == UEF_infoID) {
                char buffer[64];
                uint32_t remaining = chunk->length;
                while (remaining && !ctx.error) {
                    uint32_t count = remaining < sizeof(buffer)-1 ? remaining : sizeof(buffer)-1;
                    if (!UefRead(ctx, buffer, count)) break;
                    buffer[count] = 0;
                    fprintf(stderr, "Drv02:UEF Info : '%s'", buffer);
                    remaining -= count;
                }
            } else if (id == UEF_freqChgID || id == UEF_floatGapID) {
                float ignored;
                if (chunk->length < sizeof(ignored) || !UefRead(ctx, &ignored, sizeof(ignored))) ctx.error = true;
            }
        }
        // Reserve room for the byte-rounding/padding arithmetic.
        if (chunk_bitlen > UINT32_MAX - 7ULL - chunk_start) { ctx.error = true; break; }
        if (bit_pos < chunk_bitlen) {
            chunk->bit_offset_start = chunk_start;
            chunk->bit_offset_end = chunk_start + chunk_bitlen;
            *p_bit_pos = bit_pos;
            return chunk;
        }
        bit_pos -= chunk_bitlen;
        chunk_start += chunk_bitlen;
        if (!UefSeek(ctx, chunk->file_offset + chunk->length)) break;
    }
    if (length_check && !ctx.error) ctx.bits = chunk_start;
    *p_bit_pos = bit_pos;
    return NULL;
}

static uint8_t GetBitAtPos(UefReadContext &ctx, uint32_t bit_pos)
{
    if (bit_pos >= ctx.bits) return 0; // Valid last-byte padding.
    ChunkInfo *info = GetChunkAtPosFile(ctx, &bit_pos);
    if (!info) { ctx.error = true; return 0; }
    uint16_t id = info->id;
    if (id == UEF_gapID) return 0;
    if (id == UEF_highToneID) return 1;
    if (id == UEF_tapeID) {
        uint32_t byte_offset = bit_pos / 10;
        uint32_t bit_offset = bit_pos % 10;
        if (bit_offset == 0) return UEF_startBit;
        if (bit_offset == 9) return UEF_stopBit;
        uint8_t byte = 0;
        if (!UefSeek(ctx, info->file_offset + byte_offset) || !UefRead(ctx, &byte, 1)) return 0;
        return (byte >> (bit_offset - 1)) & 1;
    }
    if (id != UEF_highDummyID) { ctx.error = true; return 0; }
    if (bit_pos < info->pre_carrier || bit_pos >= info->pre_carrier + 20) return 1;
    bit_pos = (bit_pos - info->pre_carrier) % 10;
    if (bit_pos == 0) return UEF_startBit;
    if (bit_pos == 9) return UEF_stopBit;
    return ('A' >> (bit_pos - 1)) & 1;
}

#define BUFLEN      16384
#define CHUNK 16384

#define kBufferSize 4096

static int uef_copy_file(fileTYPE *source, FILE *dest)
{
    unsigned char in[CHUNK];
    if (source->offset < 0 || source->offset > source->size) return -1;
    uint64_t remaining = source->size - source->offset;
    while (remaining) {
        int count = remaining > CHUNK ? CHUNK : remaining;
        if (FileReadAdv(source, in, count, -1) != count || fwrite(in, 1, count, dest) != (size_t)count)
            return -1;
        remaining -= count;
    }
    return ferror(dest) ? -1 : 0;
}

/* Decompress from file source to file dest until stream ends or EOF.
   inf() returns Z_OK on success, Z_MEM_ERROR if memory could not be
   allocated for processing, Z_DATA_ERROR if the deflate data is
   invalid or incomplete, Z_VERSION_ERROR if the version of zlib.h and
   the version of the library linked do not match, or Z_ERRNO if there
   is an error reading or writing the files. */
static int uef_inflate_file(fileTYPE *source, FILE *dest)
{

    int ret;
    unsigned have;
    z_stream strm;
    unsigned char in[CHUNK];
    unsigned char out[CHUNK];

    /* allocate inflate state */
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.avail_in = 0;
    strm.next_in = Z_NULL;
    ret = inflateInit2(&strm,MAX_WBITS|16); // make sure to add the 16 to get it to accept gz header

    if (ret != Z_OK)
        return ret;

    /* decompress until deflate stream ends or end of file */
    do {

        if (source->offset < 0 || source->offset > source->size) {
            (void)inflateEnd(&strm);
            return Z_ERRNO;
        }
        uint64_t remaining = source->size - source->offset;
        int count = remaining > CHUNK ? CHUNK : remaining;
        int res = count ? FileReadAdv(source, in, count, -1) : 0;
        if (res != count) {
            (void)inflateEnd(&strm);
            return Z_ERRNO;
        }
        strm.avail_in = res;
        if (strm.avail_in == 0)
            break;
        strm.next_in = in;

        /* run inflate() on input until output buffer not full */
        do {

            strm.avail_out = CHUNK;
            strm.next_out = out;



            ret = inflate(&strm, Z_NO_FLUSH);
            assert(ret != Z_STREAM_ERROR);  /* state not clobbered */
            switch (ret) {
            case Z_NEED_DICT:
                ret = Z_DATA_ERROR;     /* and fall through */
                (void)inflateEnd(&strm);
                return ret;
            break;
            case Z_DATA_ERROR:
                (void)inflateEnd(&strm);
                return ret;
            break;
            case Z_MEM_ERROR:
                (void)inflateEnd(&strm);
                return ret;
            break;
            }

            have = CHUNK - strm.avail_out;
            if (fwrite(out, 1, have, dest) != have || ferror(dest)) {
                (void)inflateEnd(&strm);
                return Z_ERRNO;
            }

        } while (strm.avail_out == 0);

        /* done when inflate() says it's done */
    } while (ret != Z_STREAM_END);

    /* clean up and return */
    (void)inflateEnd(&strm);
    return ret == Z_STREAM_END ? Z_OK : Z_DATA_ERROR;
}

int UEF_FileSend(fileTYPE *inputfile, int use_progress)
{
    unsigned char fbuf[kBufferSize];
    if (FileReadAdv(inputfile, fbuf, 2) != 2 || !FileSeek(inputfile, 0, SEEK_SET)) return 0;
    bool compressed = fbuf[0] == 0x1f && fbuf[1] == 0x8b;
    FILE *uncompressed_file = tmpfile();
    if (!uncompressed_file) return 0;
    bool completed = [&]() -> bool {
        if (compressed ? uef_inflate_file(inputfile, uncompressed_file) != Z_OK :
                         uef_copy_file(inputfile, uncompressed_file) != 0) return false;
        if (fflush(uncompressed_file) || fseek(uncompressed_file, 0, SEEK_END)) return false;
        long length = ftell(uncompressed_file);
        if (length < 12 || (uint64_t)length > UINT32_MAX || fseek(uncompressed_file, 0, SEEK_SET)) return false;
        char header[12];
        if (fread(header, 1, sizeof(header), uncompressed_file) != sizeof(header) ||
            memcmp(header, "UEF File!\0", 10)) return false;
        UefReadContext ctx = { uncompressed_file, (uint32_t)length, 0, false, {} };
        uint32_t scan = UINT32_MAX;
        GetChunkAtPosFile(ctx, &scan);
        if (ctx.error) return false;
        uint32_t size = (ctx.bits + 7) / 8;
        uint32_t original_size = size, addr = 0;
        memset(&ctx.chunk, 0, sizeof(ctx.chunk));
        while (size) {
            uint32_t count = size < sizeof(fbuf) ? size : sizeof(fbuf);
            for (uint32_t pos = 0; pos < count; ++pos) {
                uint8_t value = 0;
                for (uint32_t bit = 0; bit < 8; ++bit)
                    value = (value << 1) | GetBitAtPos(ctx, ((addr + pos) << 3) + bit);
                fbuf[pos] = value;
                if (ctx.error) return false;
            }
            if (use_progress) ProgressMessage("Loading", inputfile->name, original_size-size, original_size);
            user_io_file_tx_data(fbuf, count);
            size -= count;
            addr += count;
        }
        return true;
    }();
    if (fclose(uncompressed_file)) completed = false;
    return completed ? 1 : 0;
}
