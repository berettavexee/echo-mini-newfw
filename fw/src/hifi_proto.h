/* Our protocol between core A and core B (ETUDE §19.3), shared by both builds.
 *
 * Memory (core B view = core A view, except HIGHRAM0 which core B sees at 0):
 *   0x01020000  control block (struct hp_ctl), 4 KiB
 *   0x01021000  core B stack (8 KiB, SP at 0x01023000)
 *   0x01023000  codec data and BSS, up to 0x01050000 (180 KiB)
 *   0x01050000  input ring, 32 KiB: compressed data written by core A
 *   0x01058000  output ring, 32 KiB: PCM written by core B, copied by core A into the DAC ring
 *               (192 KiB in SYSRAM, read by DMAC1 channel 5; ETUDE §23)
 * Input positions are byte counters modulo 2^32 (the ring holds 'size' bytes at 'pos % size');
 * output positions are offsets in the ring (0 to size - 1).
 * Core A rings the mailbox (A2B channel 1) after it changes anything core B waits for (new
 * input, output read position, command); core B sleeps in WFI between two pieces of work. */
#ifndef HIFI_PROTO_H
#define HIFI_PROTO_H

#include <stdint.h>

#define HP_CTL_ADDR   0x01020000u
#define HP_STACK_TOP  0x01023000u
#define HP_DATA_START 0x01023000u
#define HP_DATA_END   0x01050000u
#define HP_IN_BASE    0x01050000u
#define HP_IN_SIZE    0x8000u
#define HP_OUT_BASE   0x01058000u
#define HP_OUT_SIZE   0x8000u
#define HP_OUT_GUARD  4096u             /* bytes core B keeps free ahead of core A's copy */

#define HP_MAGIC      0x5452424eu       /* "NBRT" */
#define HP_VERSION    1

enum hp_state {
    HP_BOOT, HP_READY,                  /* runtime up, waiting for OPEN */
    HP_OPENING,                         /* parsing the stream header */
    HP_FORMAT,                          /* format known (rate, channels, frame size): waits PLAY */
    HP_PLAYING, HP_END,                 /* all the input decoded and written */
    HP_ERROR,
};

enum hp_cmd { HP_CMD_NONE, HP_CMD_OPEN, HP_CMD_PLAY, HP_CMD_STOP };

enum hp_err {
    HP_ERR_NONE, HP_ERR_FORMAT, HP_ERR_UNSUPPORTED, HP_ERR_DECODE, HP_ERR_FAULT,
};

struct hp_ctl {
    uint32_t magic, version, codec;     /* B: at start-up */
    uint32_t state, error, error_info;  /* B */
    uint32_t cmd, cmd_seq;              /* A: a new command is a new cmd_seq */
    uint32_t ack_seq;                   /* B: last cmd_seq taken */
    /* input ring, written by A */
    uint32_t in_wr;                     /* A: bytes written */
    uint32_t in_rd;                     /* B: bytes consumed */
    uint32_t in_end;                    /* A: total stream length (0xffffffff: not known yet) */
    /* output ring, written by B */
    uint32_t out_wr;                    /* B: offset of the next byte written */
    uint32_t out_rd;                    /* A: offset of the next byte copied out */
    /* format, set by B before HP_FORMAT */
    uint32_t rate, channels, bits;      /* of the source */
    uint32_t frame_bytes;               /* in the output ring: 4 (16-bit packed) or 8 (32-bit) */
    /* B: progress and load */
    uint32_t frames;                    /* frames written to the output ring */
    uint32_t wakeups, work_loops;
    uint32_t fault_pc, fault_lr, fault_cfsr;
    uint32_t total_frames;              /* B, before HP_FORMAT: length of the stream (0: unknown) */
    uint32_t end_granule_lo, end_granule_hi;    /* A, before OPEN, Ogg only: granule position of
                                                   the last page (the stream length), 0: unknown */
    /* B: time asleep, in TIMER1 ticks (24 MHz, shared with core A), for the load (ETUDE §24) */
    uint32_t idle_ticks;                /* total of the finished sleeps */
    uint32_t sleep_since;               /* TIMER1 when the current sleep began | 1, 0: awake */
    /* A, before OPEN, Ogg only: where the comment header ends, found by the tag reader: file
     * offset of the page that holds its last segment, and the index of the segment after it
     * (the setup header of Vorbis starts there). 0: not known, B reads the comment header. */
    uint32_t comment_end_page, comment_end_seg;
    /* A, before OPEN: where to start, in ms (0: at the start). B jumps forward in the input after
     * the headers (a proportion of the stream, a little before, then the frame or page found
     * there), drops what comes before, and sets frames to the position reached. */
    uint32_t seek_ms;
    uint32_t seek_offset;               /* A, Ogg: page to jump to (granule <= target - 1 s), 0: B
                                           guesses in proportion to the stream */
    uint32_t start_frame;               /* B: position of the first frame written (a seek) */
    /* B asks A to read the input again from an earlier offset (0.80, ETUDE §61: Musepack SV8
     * reads its seek table at the end of the file, then comes back): B sets rewind_pos and
     * increments rewind_seq; A empties the ring, feeds from rewind_pos and sets rewind_ack */
    uint32_t rewind_pos, rewind_seq, rewind_ack;
    uint32_t hacc_sub, hacc_soft;       /* FLAC: LPC subframes on HIFIACC, in software after a
                                           timeout (ETUDE §67) */
#ifdef ECHO_HWTEST
    /* Test build only (ETUDE §66): HIFIACC's LPC against dr_flac, per track, written by core B
     * and logged by core A at the end of the track */
    struct {
        uint32_t magic;                 /* HWT_MAGIC once the test ran */
        uint32_t sub, exact, diff, timeout, skipped;    /* LPC subframes */
        uint32_t wide, wide_diff;       /* those needing more than 32 bits in the products */
        uint32_t max_order, max_shift, max_n;
        uint32_t hw_ticks, sw_ticks;    /* TIMER1 (24 MHz), summed over the compared subframes */
        uint32_t f_order, f_shift, f_prec, f_bps, f_n, f_idx;   /* the first difference */
        int32_t f_sw, f_hw;
        uint32_t diff_orders;           /* bit k: a difference at order k + 1 */
        uint32_t split, max_seg;        /* blocks over HWT_SEG run in two passes (HW4) */
        uint16_t ok_o[32], diff_o[32];  /* per order */
        int32_t o1_x[6], o1_sw[6], o1_hw[6];    /* the first order-1 difference: input, outputs */
        int32_t o1_coef, o1_shift;
    } hwt;
#endif
};
#define HWT_MAGIC 0x48575431u           /* "HWT1" */

#define HP_CTL ((volatile struct hp_ctl *)HP_CTL_ADDR)

/* Codec pack in our firmware image (header +0x148: count 1, +0x14c offset, +0x150 length):
 * "NNCP", count, then entries; each blob at 'offset' from the pack start, 512-aligned:
 * code (loaded at HIGHRAM0), then data (loaded at data_addr). */
#define HP_PACK_MAGIC 0x50434e4eu       /* "NNCP" */
/* HP_CODEC_AIFF: core A's own (file type, tags, screen); core B runs the HP_CODEC_WAV image for
 * it, which reads both containers (hifi/codec_pcm.c). HP_CODEC_AAC and HP_CODEC_ALAC: in MP4, fed
 * by core A as a packet stream (below); the browser gives AAC to every M4A, the container says. */
enum hp_codec { HP_CODEC_WAV = 1, HP_CODEC_FLAC, HP_CODEC_MP3, HP_CODEC_VORBIS, HP_CODEC_OPUS,
                HP_CODEC_AIFF, HP_CODEC_AAC, HP_CODEC_ALAC, HP_CODEC_MPC };

/* Packet stream (MP4 audio, ETUDE §55): core A reads the container and its tables and writes
 * into the input ring this header, the decoder configuration (AAC: the AudioSpecificConfig),
 * then each audio frame as a u16 length (little-endian) and its bytes. Seeking is core A's: the
 * stream starts at the frame wanted, 'skip' and 'start_frame' say what to drop and where it is. */
#define HP_PKT_MAGIC 0x50345048u        /* "HP4P" */
struct hp_pkt_header {
    uint32_t magic;
    uint32_t rate, channels;
    uint32_t skip;                      /* decoded frames to drop first (encoder delay, seek) */
    uint32_t length;                    /* frames to write in all (0xffffffff: up to the end) */
    uint32_t start_frame;               /* position of the first frame written */
    uint32_t config_len;                /* bytes of decoder configuration that follow */
};

struct hp_pack_entry {
    uint32_t codec, offset, code_size, data_addr, data_size, bss_end;
};

#endif
