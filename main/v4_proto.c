/*
 * v4_proto.c - frame assembly and validation for the V4 <-> ESP32 I2C protocol.
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * All helpers are symmetric: the same code is used on the ESP32 side and can be
 * compiled unchanged into the Vampire (m68k) master software, which is why this
 * file only depends on <string.h> and <stdint.h>.
 */

#include <string.h>

#include "v4_proto.h"

/* ------------------------------------------------------------------ */
/* WRITE frames (master -> ESP32)                                     */
/* ------------------------------------------------------------------ */

void v4p_wframe_build(uint8_t *out, uint8_t cmd, uint8_t seq,
                      const uint8_t *payload, uint8_t len)
{
    if (out == NULL) {
        return;
    }
    if (len > V4P_WRITE_PAYLOAD_MAX) {
        len = V4P_WRITE_PAYLOAD_MAX;
    }

    memset(out, 0, V4P_WRITE_FRAME_LEN);
    out[V4P_W_MAGIC] = V4P_MAGIC_WRITE;
    out[V4P_W_CMD]   = cmd;
    out[V4P_W_SEQ]   = seq;
    out[V4P_W_LEN]   = len;
    if (payload != NULL && len > 0) {
        memcpy(&out[V4P_W_DATA], payload, len);
    }
    out[V4P_W_CRC] = v4p_crc8(out, V4P_W_CRC);
}

bool v4p_wframe_check(const uint8_t *in, size_t got,
                      uint8_t *cmd, uint8_t *seq, uint8_t *len)
{
    if (in == NULL || got != V4P_WRITE_FRAME_LEN) {
        return false;
    }
    if (in[V4P_W_MAGIC] != V4P_MAGIC_WRITE) {
        return false;
    }

    uint8_t payload_len = in[V4P_W_LEN];
    if (payload_len > V4P_WRITE_PAYLOAD_MAX) {
        return false;
    }
    if (in[V4P_W_CRC] != v4p_crc8(in, V4P_W_CRC)) {
        return false;
    }

    if (cmd != NULL) {
        *cmd = in[V4P_W_CMD];
    }
    if (seq != NULL) {
        *seq = in[V4P_W_SEQ];
    }
    if (len != NULL) {
        *len = payload_len;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* READ frames (ESP32 -> master)                                      */
/* ------------------------------------------------------------------ */

uint8_t *v4p_rframe_begin(uint8_t *out, uint8_t cmd, uint8_t seq,
                          uint8_t status, uint8_t flags)
{
    if (out == NULL) {
        return NULL;
    }

    memset(out, 0, V4P_READ_FRAME_LEN);
    out[V4P_R_MAGIC]  = V4P_MAGIC_READ;
    out[V4P_R_CMD]    = cmd;
    out[V4P_R_SEQ]    = seq;
    out[V4P_R_STATUS] = status;
    out[V4P_R_LEN]    = 0;
    out[V4P_R_FLAGS]  = flags;
    return &out[V4P_R_DATA];
}

void v4p_rframe_finish(uint8_t *out, uint8_t payload_len)
{
    if (out == NULL) {
        return;
    }
    if (payload_len > V4P_READ_PAYLOAD_MAX) {
        payload_len = V4P_READ_PAYLOAD_MAX;
    }
    out[V4P_R_LEN] = payload_len;
    out[V4P_R_CRC] = v4p_crc8(out, V4P_R_CRC);
}

bool v4p_rframe_check(const uint8_t *in, size_t got, uint8_t cmd, uint8_t seq,
                      uint8_t *status, uint8_t *len, uint8_t *flags)
{
    if (in == NULL || got != V4P_READ_FRAME_LEN) {
        return false;
    }
    if (in[V4P_R_MAGIC] != V4P_MAGIC_READ) {
        return false;
    }
    if (in[V4P_R_CRC] != v4p_crc8(in, V4P_R_CRC)) {
        return false;
    }
    if (in[V4P_R_CMD] != cmd || in[V4P_R_SEQ] != seq) {
        return false;
    }
    uint8_t payload_len = in[V4P_R_LEN];
    if (payload_len > V4P_READ_PAYLOAD_MAX) {
        return false;
    }

    if (status != NULL) {
        *status = in[V4P_R_STATUS];
    }
    if (len != NULL) {
        *len = payload_len;
    }
    if (flags != NULL) {
        *flags = in[V4P_R_FLAGS];
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* BULK frames (ESP32 -> master, FILE_READ payload)                   */
/* ------------------------------------------------------------------ */

size_t v4p_bframe_build(uint8_t *out, uint8_t cmd, uint8_t status, uint8_t handle,
                        uint16_t block, const uint8_t *data, uint16_t valid_len,
                        uint16_t chunk_size)
{
    if (out == NULL || chunk_size == 0 || chunk_size > V4P_BULK_PAYLOAD_MAX) {
        return 0;
    }
    if (valid_len > chunk_size) {
        valid_len = chunk_size;
    }

    out[V4P_B_MAGIC]  = V4P_MAGIC_BULK;
    out[V4P_B_CMD]    = cmd;
    out[V4P_B_STATUS] = status;
    out[V4P_B_HANDLE] = handle;
    v4p_put_u16le(&out[V4P_B_BLOCK], block);
    v4p_put_u16le(&out[V4P_B_LEN], valid_len);

    /* Always fill the whole chunk: zero padding past valid_len keeps the frame
     * length constant, and the CRC below covers the padding too. */
    memset(&out[V4P_B_DATA], 0, chunk_size);
    if (data != NULL && valid_len > 0) {
        memcpy(&out[V4P_B_DATA], data, valid_len);
    }

    size_t crc_off = (size_t)V4P_BULK_HDR_LEN + chunk_size;
    v4p_put_u32le(&out[crc_off], v4p_crc32(out, crc_off));
    return crc_off + V4P_BULK_CRC_LEN;
}

size_t v4p_bframe_check(const uint8_t *in, size_t got, uint16_t chunk_size)
{
    if (in == NULL || chunk_size == 0 || chunk_size > V4P_BULK_PAYLOAD_MAX) {
        return 0;
    }
    if (in[V4P_B_MAGIC] != V4P_MAGIC_BULK) {
        return 0;
    }

    size_t crc_off = (size_t)V4P_BULK_HDR_LEN + chunk_size;
    if (got != crc_off + V4P_BULK_CRC_LEN) {
        return 0;
    }
    if (v4p_get_u16le(&in[V4P_B_LEN]) > chunk_size) {
        return 0;
    }
    if (v4p_get_u32le(&in[crc_off]) != v4p_crc32(in, crc_off)) {
        return 0;
    }
    return crc_off + V4P_BULK_CRC_LEN;
}
