/*
 * v4_proto.h - I2C protocol between the Vampire V4 (master) and the ESP32 (slave).
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * ============================================================================
 *  DESIGN RULES - read this before changing anything
 * ============================================================================
 *
 *  1. ONE COMMAND WRITE => EXACTLY ONE RESPONSE READ.
 *     The ESP32 never sends anything the master did not ask for. This is what
 *     keeps the link in sync: the I2C slave TX path is a byte FIFO/ringbuffer,
 *     not a message queue, so any byte the master does not read would otherwise
 *     be prepended to the next response.
 *
 *  2. The ESP32 cannot do clock stretching as an I2C slave (ESP32 specific,
 *     see ESP-IDF docs "I2C Slave Controller"). Therefore the master MUST NOT
 *     start a read before the response is ready. The sequence is:
 *         master: WRITE 32 byte command
 *         master: wait (fixed delay, or poll STATUS)
 *         master: READ  128 byte response
 *
 *  3. Commands are idempotent. On CRC error, bad magic or bad SEQ the master
 *     simply sends the same command again with the same SEQ.
 *
 *  4. STATUS_BUSY means "response produced, but the work is not finished".
 *     The master re-sends the WHOLE command (not just the read).
 *
 *  5. ALL multi-byte fields are explicitly little-endian byte order, without
 *     exception. The Vampire is a big-endian m68k, the ESP32 is little-endian,
 *     so never memcpy a struct across the link - always use the put/get
 *     helpers below.
 *
 * ============================================================================
 *  FRAME LAYOUTS
 * ============================================================================
 *
 *  WRITE frame (V4 -> ESP32), fixed 32 bytes:
 *    +0   magic   0xA5
 *    +1   cmd     command id
 *    +2   seq     0..255, echoed back in the response
 *    +3   len     payload length, 0..27
 *    +4   .. +30  payload (27 bytes)
 *    +31  crc8    CRC-8 (poly 0x07, init 0x00) over bytes 0..30
 *
 *  READ frame (ESP32 -> V4), fixed 128 bytes:
 *    +0   magic   0x5A
 *    +1   cmd     echoed command id
 *    +2   seq     echoed sequence number
 *    +3   status  V4P_ST_*
 *    +4   len     payload length, 0..121
 *    +5   .. +125 payload (121 bytes)
 *    +126 flags   V4P_FL_*
 *    +127 crc8    CRC-8 over bytes 0..126
 *
 *  The READ payload was 57 bytes in the first revision, which capped file names
 *  at 48 characters. Amiga scene names ("Games, The - Summer Edition
 *  (1989)(U.S. Gold)[cr SR][a2].lha") blow straight through that, and since
 *  FILE_OPEN needs the FULL name, such files were listable but not openable.
 *  It is 121 bytes now, so names up to 113 characters survive.
 *
 *  BULK frame (ESP32 -> V4), FIXED size = 12 + chunk bytes:
 *    +0   magic   0x5B
 *    +1   cmd     echoed command id (always FILE_READ)
 *    +2   status  V4P_ST_*
 *    +3   handle  file handle
 *    +4   block   block index, u16 LE
 *    +6   len     number of VALID data bytes, u16 LE (0..chunk)
 *    +8   data    ALWAYS chunk bytes, zero padded past len
 *    +8+chunk  crc32 (u32 LE, poly 0xEDB88320, init/xorout 0xFFFFFFFF)
 *              over bytes 0 .. 7+chunk
 *
 *  The data area is padded to the full chunk size even for the last, shorter
 *  block, and the CRC covers the padding. That is deliberate: it makes the
 *  response length a constant the master knows in advance, so the master never
 *  has to read past the end of the available data - which is exactly the
 *  situation the ESP32 slave cannot signal without clock stretching.
 */

#ifndef V4_PROTO_H
#define V4_PROTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Version                                                            */
/* ------------------------------------------------------------------ */

#define V4P_PROTO_VERSION       3       /* 3 = 128-byte READ frame (2 used 64 bytes) */
#define V4P_FW_VERSION          0x02    /* firmware capability version */

/* ------------------------------------------------------------------ */
/* Frame geometry                                                     */
/* ------------------------------------------------------------------ */

#define V4P_MAGIC_WRITE         0xA5
#define V4P_MAGIC_READ          0x5A
#define V4P_MAGIC_BULK          0x5B

#define V4P_WRITE_FRAME_LEN     32
#define V4P_READ_FRAME_LEN      128
#define V4P_BULK_HDR_LEN        8
#define V4P_BULK_CRC_LEN        4

/* payload capacities */
#define V4P_WRITE_PAYLOAD_MAX   27
#define V4P_READ_PAYLOAD_MAX    121
#define V4P_BULK_PAYLOAD_MAX    1024
#define V4P_BULK_FRAME_MAX      (V4P_BULK_HDR_LEN + V4P_BULK_PAYLOAD_MAX + V4P_BULK_CRC_LEN)

/* default and minimum chunk size for FILE_READ, negotiable via SET_CHUNK */
#define V4P_CHUNK_DEFAULT       128
#define V4P_CHUNK_MIN           16
#define V4P_CHUNK_MAX           1024

/* path assembly buffer used by PATH_CLEAR / PATH_APPEND */
#define V4P_PATH_MAX            128

/* maximum file name reported through DIR_NEXT (long names are truncated) */
#define V4P_NAME_MAX            113

/* BT device table size */
#define V4P_MAX_DEVICES         16

/* ------------------------------------------------------------------ */
/* WRITE frame offsets                                                */
/* ------------------------------------------------------------------ */

#define V4P_W_MAGIC             0
#define V4P_W_CMD               1
#define V4P_W_SEQ               2
#define V4P_W_LEN               3
#define V4P_W_DATA              4
#define V4P_W_CRC               31

/* ------------------------------------------------------------------ */
/* READ frame offsets                                                 */
/* ------------------------------------------------------------------ */

#define V4P_R_MAGIC             0
#define V4P_R_CMD               1
#define V4P_R_SEQ               2
#define V4P_R_STATUS            3
#define V4P_R_LEN               4
#define V4P_R_DATA              5
#define V4P_R_FLAGS             126
#define V4P_R_CRC               127

/* ------------------------------------------------------------------ */
/* BULK frame offsets                                                 */
/* ------------------------------------------------------------------ */

#define V4P_B_MAGIC             0
#define V4P_B_CMD               1
#define V4P_B_STATUS            2
#define V4P_B_HANDLE            3
#define V4P_B_BLOCK             4       /* u16 LE */
#define V4P_B_LEN               6       /* u16 LE */
#define V4P_B_DATA              8

/* ------------------------------------------------------------------ */
/* Status codes                                                       */
/* ------------------------------------------------------------------ */

typedef enum {
    V4P_ST_OK        = 0x00,   /* command completed, payload valid            */
    V4P_ST_BUSY      = 0x01,   /* not finished yet - re-send the whole command */
    V4P_ST_BAD_CRC   = 0x02,   /* request CRC or framing invalid              */
    V4P_ST_BAD_CMD   = 0x03,   /* unknown command id                          */
    V4P_ST_BAD_ARG   = 0x04,   /* argument out of range                       */
    V4P_ST_BAD_STATE = 0x05,   /* not valid in the current state              */
    V4P_ST_NO_SD     = 0x06,   /* no card mounted                             */
    V4P_ST_NOT_FOUND = 0x07,   /* device / path / entry not found             */
    V4P_ST_IO_ERR    = 0x08,   /* filesystem or driver level error            */
    V4P_ST_END       = 0x09,   /* end of listing / end of file                */
    V4P_ST_TOO_LONG  = 0x0A,   /* path or name too long                       */
    V4P_ST_NO_HANDLE = 0x0B,   /* handle not open / already closed            */
    V4P_ST_BT_ERR    = 0x0C,   /* bluetooth stack refused the operation       */
} v4p_status_t;

/* ------------------------------------------------------------------ */
/* READ frame flag bits                                               */
/* ------------------------------------------------------------------ */

#define V4P_FL_NONE             0x00
#define V4P_FL_NAME_TRUNCATED   0x01    /* DIR_NEXT: name did not fit in full  */
#define V4P_FL_MORE             0x02    /* more entries / more blocks follow   */

/* GET_STATUS offset V4P_ST_OFF_AUDIO is a bitfield, not a plain boolean. */
#define V4P_AUDIO_A2DP_STREAMING    0x01    /* an A2DP link is up and fed      */
#define V4P_AUDIO_SD_PLAYBACK       0x02    /* a file from the SD card is the source */

/* ------------------------------------------------------------------ */
/* Bluetooth state machine, reported in GET_STATUS                    */
/* ------------------------------------------------------------------ */

typedef enum {
    V4P_BT_IDLE       = 0,
    V4P_BT_SCANNING   = 1,
    V4P_BT_CONNECTING = 2,
    V4P_BT_CONNECTED  = 3,
    V4P_BT_SUSPENDED  = 4,
} v4p_bt_state_t;

/* special value for "no device connected" */
#define V4P_BT_NO_INDEX         0xFF

/* ------------------------------------------------------------------ */
/* Commands                                                           */
/* ------------------------------------------------------------------ */

typedef enum {
    /* --- identification ------------------------------------------- */
    V4P_CMD_PING          = 0x01,   /* -            -> ver, fw, frame sizes    */
    V4P_CMD_GET_STATUS    = 0x02,   /* -            -> v4p_status_payload_t    */
    V4P_CMD_GET_INFO      = 0x03,   /* -            -> frame sizes, chunk size */

    /* --- bluetooth discovery --------------------------------------- */
    V4P_CMD_SCAN_START    = 0x10,   /* [len_s u8]   -> -                       */
    V4P_CMD_SCAN_STOP     = 0x11,   /* -            -> -                       */
    V4P_CMD_DEV_COUNT     = 0x12,   /* -            -> count u8                */
    V4P_CMD_DEV_GET       = 0x13,   /* [idx u8]     -> idx,bda[6],name_len,name*/

    /* --- bluetooth connection -------------------------------------- */
    V4P_CMD_CONNECT       = 0x20,   /* [idx u8]     -> -                       */
    V4P_CMD_CONNECT_BDA   = 0x21,   /* [bda 6]      -> -                       */
    V4P_CMD_DISCONNECT    = 0x22,   /* -            -> -                       */
    V4P_CMD_FORGET        = 0x23,   /* -            -> - (clears NVS address)  */

    /* --- sd card --------------------------------------------------- */
    V4P_CMD_SD_MOUNT      = 0x30,   /* [force u8]   -> - (force=1 retries)     */
    V4P_CMD_SD_INFO       = 0x31,   /* -            -> total/free kb, fat type */
    V4P_CMD_SET_CHUNK     = 0x32,   /* [chunk u16]  -> chunk u16               */

    /* --- path assembly (for paths longer than 27 bytes) ------------ */
    V4P_CMD_PATH_CLEAR    = 0x38,   /* -            -> -                       */
    V4P_CMD_PATH_APPEND   = 0x39,   /* [fragment]   -> -                       */

    /* --- directory listing ----------------------------------------- */
    V4P_CMD_DIR_OPEN      = 0x40,   /* [path]       -> handle u8               */
    V4P_CMD_DIR_NEXT      = 0x41,   /* [h u8][idx u16 LE] -> v4p_dirent_t      */
    V4P_CMD_DIR_CLOSE     = 0x42,   /* [handle u8]  -> -                       */

    /* --- file access ----------------------------------------------- */
    V4P_CMD_FILE_OPEN     = 0x50,   /* [path]       -> handle u8, size u32     */
    V4P_CMD_FILE_READ     = 0x51,   /* [h u8][blk u16] -> BULK frame           */
    V4P_CMD_FILE_CLOSE    = 0x52,   /* [handle u8]  -> -                       */

    /* --- SD -> Bluetooth playback ---------------------------------- */
    V4P_CMD_PLAY_FILE     = 0x60,   /* [path] or assembled -> -  (SD -> BT)    */
    V4P_CMD_STOP_PLAY     = 0x61,   /* -            -> -  (back to the I2S input)*/
    /*
     * A2DP-Uebertragung starten (0.9.65).
     *
     * Bis 0.9.64 liess sich die Uebertragung nur auf der Konsole des ESP32
     * starten ('start_media'). Ueber I2C kam die Vampire damit bis zum
     * Datei-Zweig, der aber keinen Abnehmer hatte:
     *
     *   I v4_link: SD playback started: /sdcard/test2.mp3
     *   E STREAM_PROC: Wiedergabe beendet - stoppe den Datei-Zweig (ERROR)
     *
     * Antworten: OK (laeuft/laeuft jetzt), NO_DEVICE... siehe docs/I2C_BRUECKE.md.
     * PLAY_FILE startet die Uebertragung ebenfalls, wenn sie noch nicht laeuft -
     * dann funktionieren auch Master, die dieses Kommando nicht kennen.
     */
    V4P_CMD_MEDIA_START   = 0x62,   /* -            -> -  (start A2DP stream)  */

    /*
     * Equalizer hinter dem Mischer (0.9.72).
     *
     * Bis 0.9.71 gab es dafuer nur Konsolenbefehle - und keinen Weg, die
     * Einstellung ZURUECKZULESEN (in docs/I2C_ERWEITERN.md als Luecke
     * vermerkt). Der Block 0x70..0x73 ist dafuer reserviert.
     */
    V4P_CMD_EQ_INFO       = 0x70,   /* -            -> bands u8, aktiv u8      */
    V4P_CMD_EQ_BANDS      = 0x71,   /* [aktiv u8]   -> aktiv u8               */
    V4P_CMD_EQ_GET        = 0x72,   /* [idx u8]     -> v4p_eq_band_t          */
    V4P_CMD_EQ_SET        = 0x73,   /* [idx u8][typ u8][fc u32][q u16][gain s16] -> - */

    V4P_CMD_RESET         = 0x7E,   /* -            -> -  (soft protocol reset)*/
} v4p_cmd_t;
/*
 * Ein Equalizer-Band auf der Leitung (v4p_eq_band_t, 12 Byte, little-endian).
 *
 *   +0  idx      u8    Bandindex 0..bands-1
 *   +1  typ      u8    1 HighPass, 2 LowPass, 3 Peak, 4 HighShelf, 5 LowShelf
 *   +2  enabled  u8    1 = dieses Band filtert
 *   +3  reserved u8    0
 *   +4  fc       u32  Grenz-/Mittenfrequenz in Hz
 *   +8  q        u16  Guete Q x 100   (Q 0,70 -> 70)
 *   +10 gain     s16  Verstaerkung in dB x 10, vorzeichenbehaftet
 *
 * Q und Gain als Ganzzahlen, damit beide Seiten ohne Fliesskomma ueberein
 * kommen (die V4 ist Big Endian, der ESP32 Little Endian).
 */
#define V4P_EQ_BAND_LEN        12
#define V4P_EQ_OFF_IDX          0
#define V4P_EQ_OFF_TYP          1
#define V4P_EQ_OFF_ENABLED      2
#define V4P_EQ_OFF_FC           4
#define V4P_EQ_OFF_Q            8
#define V4P_EQ_OFF_GAIN        10


/* ------------------------------------------------------------------ */
/* Payload layouts                                                    */
/* ------------------------------------------------------------------ */

/* GET_INFO / PING response payload */
typedef struct {
    uint8_t  proto_ver;         /* V4P_PROTO_VERSION                          */
    uint8_t  fw_ver;            /* V4P_FW_VERSION                             */
    uint8_t  write_frame_len;   /* 32                                         */
    uint8_t  read_frame_len;    /* V4P_READ_FRAME_LEN                         */
    uint16_t bulk_payload_max;  /* 1024, little endian                        */
    uint16_t chunk_size;        /* current FILE_READ chunk, little endian     */
    uint8_t  max_devices;       /* V4P_MAX_DEVICES                            */
    uint8_t  path_max;          /* V4P_PATH_MAX                               */
} v4p_info_payload_t;           /* 12 bytes, byte-packed below */

/* GET_STATUS response payload, byte offset based - do not cast blindly */
#define V4P_ST_OFF_STATE        0
#define V4P_ST_OFF_CONN_IDX     1
#define V4P_ST_OFF_DEV_COUNT    2
#define V4P_ST_OFF_SCAN_ACTIVE  3
#define V4P_ST_OFF_SD_MOUNTED   4
#define V4P_ST_OFF_AUDIO        5
#define V4P_ST_OFF_SCAN_GEN     6   /* u16 LE */
#define V4P_ST_OFF_SD_FREE_KB   8   /* u32 LE */
#define V4P_ST_OFF_CHUNK        12  /* u16 LE */
#define V4P_ST_OFF_PROTO_VER    14
#define V4P_ST_OFF_FW_VER       15
#define V4P_ST_OFF_SD_CARD      16  /* 1 = card-detect says a card is inserted */
#define V4P_ST_PAYLOAD_LEN      17

/* DEV_GET response payload */
#define V4P_DEV_OFF_INDEX       0
#define V4P_DEV_OFF_BDA         1   /* 6 bytes */
#define V4P_DEV_OFF_NAME_LEN    7
#define V4P_DEV_OFF_NAME        8   /* up to 49 bytes */

/* DIR_NEXT response payload */
#define V4P_DE_OFF_INDEX        0   /* u16 LE, entry counter */
#define V4P_DE_OFF_ATTR         2   /* u8, FAT attribute bits */
#define V4P_DE_OFF_SIZE         3   /* u32 LE */
#define V4P_DE_OFF_NAME_LEN     7
#define V4P_DE_OFF_NAME         8   /* up to 49 bytes */

/* FAT attribute bits (as in FF_AM_DIR etc.) */
#define V4P_ATTR_RDONLY         0x01
#define V4P_ATTR_HIDDEN         0x02
#define V4P_ATTR_SYSTEM         0x04
#define V4P_ATTR_VOLUME         0x08
#define V4P_ATTR_DIR            0x10
#define V4P_ATTR_ARCHIVE        0x20
#define V4P_ATTR_LFN            0x0F    /* mask of the long-name entry flags */

/* FILE_OPEN response payload */
#define V4P_FO_OFF_HANDLE       0
#define V4P_FO_OFF_SIZE         1   /* u32 LE */
#define V4P_FO_OFF_ATTR         5

/* SD_INFO response payload */
#define V4P_SD_OFF_TOTAL_KB     0   /* u32 LE */
#define V4P_SD_OFF_FREE_KB      4   /* u32 LE */
#define V4P_SD_OFF_SECTOR       8   /* u16 LE */
#define V4P_SD_OFF_FAT_TYPE     10

/* ------------------------------------------------------------------ */
/* Little endian helpers - use these, never cast or memcpy structs     */
/* ------------------------------------------------------------------ */

static inline void v4p_put_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static inline uint16_t v4p_get_u16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline void v4p_put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static inline uint32_t v4p_get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static inline void v4p_put_i32le(uint8_t *p, int32_t v)
{
    v4p_put_u32le(p, (uint32_t)v);
}

/* ------------------------------------------------------------------ */
/* CRC-8: poly 0x07, init 0x00, no reflection, no final xor           */
/* ------------------------------------------------------------------ */

static inline uint8_t v4p_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (uint8_t)((crc & 0x80) ? ((crc << 1) ^ 0x07) : (crc << 1));
        }
    }
    return crc;
}

/* ------------------------------------------------------------------ */
/* CRC-32: reflected, poly 0xEDB88320, init 0xFFFFFFFF, xorout 0xFFFF..*/
/* Identical to zlib/PNG CRC-32. Bitwise on purpose so that the V4     */
/* side can reuse this exact code without a 1 KB table.               */
/* ------------------------------------------------------------------ */

static inline uint32_t v4p_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ */
/* Frame helpers (implemented in v4_proto.c)                          */
/* ------------------------------------------------------------------ */

/**
 * @brief Build a 32 byte WRITE frame from the master. Used by the V4 side and
 *        by host-side tests.
 */
void v4p_wframe_build(uint8_t *out, uint8_t cmd, uint8_t seq,
                      const uint8_t *payload, uint8_t len);

/**
 * @brief Validate an incoming WRITE frame and extract cmd/seq/payload length.
 *
 * @param in    received bytes
 * @param got   number of bytes actually received
 * @param cmd   [out] command id, may be NULL
 * @param seq   [out] sequence number, may be NULL
 * @param len   [out] payload length, may be NULL
 *
 * @return true if framing and CRC are valid.
 */
bool v4p_wframe_check(const uint8_t *in, size_t got,
                      uint8_t *cmd, uint8_t *seq, uint8_t *len);

/**
 * @brief Start a 64 byte READ frame. Returns a pointer to the payload area so
 *        the caller can fill up to V4P_READ_PAYLOAD_MAX bytes into it.
 */
uint8_t *v4p_rframe_begin(uint8_t *out, uint8_t cmd, uint8_t seq,
                          uint8_t status, uint8_t flags);

/**
 * @brief Finish a READ frame: store the payload length and the CRC-8.
 */
void v4p_rframe_finish(uint8_t *out, uint8_t payload_len);

/**
 * @brief Validate an incoming READ frame and extract status/payload.
 *
 * Checks length, magic, CRC-8 and the command/sequence echo. Used by the V4
 * side.
 *
 * @return true if the frame is a valid answer to cmd/seq.
 */
bool v4p_rframe_check(const uint8_t *in, size_t got, uint8_t cmd, uint8_t seq,
                      uint8_t *status, uint8_t *len, uint8_t *flags);

/**
 * @brief Build a BULK frame.
 *
 * The data area is always padded to chunk_size bytes; valid_len says how many
 * of them carry real data.
 *
 * @return total frame length in bytes (12 + chunk_size), or 0 on invalid input.
 */
size_t v4p_bframe_build(uint8_t *out, uint8_t cmd, uint8_t status, uint8_t handle,
                        uint16_t block, const uint8_t *data, uint16_t valid_len,
                        uint16_t chunk_size);

/**
 * @brief Validate a BULK frame of the expected fixed size.
 *
 * @param got  bytes actually read, must be exactly 12 + chunk_size
 *
 * @return total frame length in bytes, or 0 if invalid.
 */
size_t v4p_bframe_check(const uint8_t *in, size_t got, uint16_t chunk_size);

#ifdef __cplusplus
}
#endif

#endif /* V4_PROTO_H */
