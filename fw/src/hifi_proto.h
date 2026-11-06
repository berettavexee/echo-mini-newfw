/* Our protocol between core A and core B (ETUDE §19.3), shared by both builds.
 *
 * Memory (core B view = core A view, except HIGHRAM0 which core B sees at 0):
 *   0x01020000  control block (struct hp_ctl), 4 KiB
 *   0x01021000  core B stack (8 KiB, SP at 0x01023000)
 *   0x01023000  codec data and BSS, up to 0x01048000 (148 KiB)
 *   0x01048000  output ring, 96 KiB: PCM written by core B, read by DMAC1 channel 5 (the DAC
 *               ring since version 2, ETUDE §103; before, core A copied it into SYSRAM)
 *   0x03020000  input ring, 96 KiB, in core A's SYSRAM (core B reads it: 0.99r17, ETUDE §102):
 *               compressed data written by core A
 * Input positions are byte counters (file offsets, below 4 GiB; the ring holds 'size' bytes
 * at 'pos % size');
 * output positions are offsets in the ring (0 to size - 1).
 * Core A rings the mailbox (A2B channel 1) after it changes anything core B waits for (new
 * input, output read position, command); core B sleeps in WFI between two pieces of work. */
#ifndef HIFI_PROTO_H
#define HIFI_PROTO_H

#include <stdint.h>

#define HP_CTL_ADDR   0x01020000u
#define HP_STACK_TOP  0x01023000u
#define HP_DATA_START 0x01023000u
#define HP_DATA_END   0x01048000u
#define HP_IN_BASE    0x03020000u       /* SYSRAM (link.ld: .hpinring) */
#define HP_IN_SIZE    0x18000u
#define HP_OUT_BASE   0x01048000u
#define HP_OUT_SIZE   0x18000u
#define HP_OUT_GUARD  4096u             /* bytes core B keeps free behind the DMA */
#define HP_DMA_SAR    0x600001b8u       /* DMAC1 channel 5 SAR: the DMA's read address */

#define HP_MAGIC      0x5452424eu       /* "NBRT" */
#define HP_VERSION    3                 /* 2: core B writes the ring the DMA reads (0.99r19);
                                           3: segments in the input ring, the track (0.99r24) */
#define HP_SEGS       4

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

/* Why a file is refused, for the "Cannot play" screen (playback.c): set with the error by core B
 * (REFUSE, rt.h) or by core A's MP4 reader; HP_WHY_NONE: the error alone says it */
enum hp_why {
    HP_WHY_NONE, HP_WHY_CHANNELS, HP_WHY_BITS, HP_WHY_FLOAT, HP_WHY_COMPRESSED, HP_WHY_HEAAC,
    HP_WHY_AAC_TYPE, HP_WHY_MP4_CODEC, HP_WHY_OPUS_MAPPING, HP_WHY_MEMORY, HP_WHY_RATE,
};

struct hp_ctl {
    uint32_t magic, version, codec;     /* B: at start-up */
    uint32_t state, error, error_info;  /* B */
    uint32_t cmd, cmd_seq;              /* A: a new command is a new cmd_seq */
    uint32_t ack_seq;                   /* B: last cmd_seq taken */
    /* input ring, written by A */
    /* Version 3 (0.99r24, ETUDE §105): the ring holds segments of the stream, each a run of
     * bytes contiguous both in the stream (file offsets) and in the ring; their starts are in
     * seg[] (A). in_rd stays the stream offset core B reads at (the codecs see the file). */
    uint32_t in_wr;                     /* A: ring bytes written (a counter, the ring holds 'size'
                                           bytes at 'counter % size') */
    uint32_t in_rd;                     /* B: the stream offset read up to */
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
    /* B asks A for the stream from another offset (version 3: forward past what is written, or
     * back: Musepack SV8 reads its seek table at the end of the file, then comes back, ETUDE
     * §61): B sets req_pos, then moves req_seq on; A answers with a segment of that seq starting
     * at req_pos. B moves rb_rd: the ring bytes it is done with (A's room). */
    uint32_t req_pos, req_seq, rb_rd;
    uint32_t hacc_sub, hacc_soft;       /* FLAC: LPC subframes on HIFIACC, in software after a
                                           timeout (ETUDE §67) */
    uint32_t synth_gran, synth_soft;    /* MP3: granules on SYNTH, in software (ETUDE §71) */
    uint32_t why;                       /* B: enum hp_why, with an HP_ERR_* of the open */
    /* The PCM ring read by the DMA (version 2, rework step 4, ETUDE §103): core B writes its
     * frames straight into the ring DMAC1 channel 5 plays (the output ring above), and follows
     * the DMA's position itself (its SAR). Byte counters modulo 2^32, the ring holds 'size'
     * bytes at 'counter % size'. Core A sets them all after HP_READY and before it clears
     * pcm_hold; from then on only core B moves pcm_wr, pcm_rd and pcm_pos, until core A stops
     * it (no index is ever moved by both cores at once: ReChord's crackle, ETUDE §98). */
    uint32_t pcm_hold;                  /* A: 1: core B writes nothing (the DACs not ready for
                                           this track yet: started, or its rate changed) */
    uint32_t pcm_wr;                    /* B: bytes written (A: where to start) */
    uint32_t pcm_rd;                    /* B: bytes the DMA has read (A: its value at the start) */
    uint32_t pcm_pos;                   /* B: the DMA's offset in the ring at the last look */
    uint32_t pcm_t;                     /* B: TIMER1 then (laps the position hides, from the time) */
    uint32_t pcm_bps;                   /* A: bytes a second the DMA reads (0: the DMA stands) */
    uint32_t pcm_ur, pcm_lost;          /* B: underruns (the DMA overtook core B), bytes */
    uint32_t pcm_min;                   /* B: the lowest fill seen, bytes (after pcm_min_from) */
    uint32_t b_hz;                      /* A: core B's clock (its SysTick: a wake every 2 ms) */
    /* A battery beep mixed by core B into what it writes next (0.99r20; core A's own mixer for
     * the SYSRAM ring): the pattern in frames at the DACs' rate, a new one each beep_seq */
    uint32_t beep_seq;                  /* A: moved on after the fields below are set */
    uint32_t beep_on, beep_period, beep_total, beep_step, beep_fade;
    uint32_t beep_from;                 /* A: the pattern's frame to start at (a track change
                                           restarts core B: the rest of the pattern goes on) */
    uint32_t beep_frame;                /* B: the pattern's frames written so far */
    /* Version 3: the segments of the input ring (A appends, B takes them in order) */
    struct hp_seg {
        uint32_t seq;                   /* 0 from the open; a req_seq answered */
        uint32_t ring0;                 /* the ring counter of its first byte */
        uint32_t off0;                  /* its stream offset (ring0 = off0 modulo 512: the reads
                                           stay whole sectors) */
        uint32_t prev_end;              /* the ring counter where the segment before it ends
                                           (ring0 - prev_end bytes of padding between) */
    } seg[HP_SEGS];
    uint32_t seg_wr;                    /* A: segments appended */
    uint32_t seg_rd;                    /* B: segments done with (the one read: seg_rd) */
    /* Version 3: the track, written by A before OPEN (the hints the codecs read; B copies them
     * into the fields above when it opens) */
    struct hp_trk {
        uint32_t codec, size;
        uint32_t seek_ms, seek_offset;
        uint32_t comment_end_page, comment_end_seg;
        uint32_t end_granule_lo, end_granule_hi;
    } trk;
#if defined(ECHO_HWTEST) || defined(ECHO_HWSYN) || defined(ECHO_FFTCMP)
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
    /* Test build only (ETUDE §69): MP3 SYNTH accelerator against pvmp3dec's synthesis */
    struct {
        uint32_t magic, trial;          /* trial: moved on by core A after each track */
        int32_t shift;                  /* input shift of this track (x >> shift) */
        uint32_t slots, polled, timeouts;
        uint32_t exact, within1, within4;   /* samples, direct order */
        uint32_t r_exact, r_within4;        /* samples, reversed order */
        uint32_t max_err;
        uint64_t sum_sw, sum_hw;            /* sum of |samples| */
        int64_t dot;                        /* sum sw * hw: the sign */
        int32_t x[4], sw[4], hw[4];         /* the first slot */
        uint32_t st_before, st_after;       /* +0x80c before writing, after the wait */
        uint32_t syn_hw_ticks, syn_sw_ticks;    /* HS3: SYNTH (18 slots) against pvmp3dec's */
        uint32_t syn_timed;                     /* synthesis calls timed */
        /* HS3: IMDCT36 against an exact IMDCT of 36 points, one transform in 16 */
        uint32_t im_mode;                       /* +0x800 of this track */
        int32_t im_shift;
        uint32_t im_calls, im_polled, im_timeouts;
        uint32_t im_exact[17], im_within1[17];  /* hw against ref scaled by 2^(e - 8) */
        uint64_t im_sum_ref, im_sum_hw;
        int64_t im_dot;
        uint32_t im_hw_ticks, im_sw_ticks;      /* IMDCT36 against pvmp3_mdct_18 */
        int32_t im_x[3], im_ref[3], im_hw[3];   /* the first transform */
        /* ECHO_HWSYN (ETUDE §71): SYNTH plays; the first granules compared with the software */
        uint32_t p_gran, p_cmp, p_exact, p_within1, p_max, p_hw_ticks, p_polled, p_timeouts;
    } hws;
    /* Vorbis IMDCT on HIFIACC's FFT against Tremor (ETUDE §74), AAC FFT against pvaacdec's
     * (§76): blocks, output samples on the 16-bit scale, TIMER1 ticks of each */
    struct {
        uint32_t magic, blocks, short_blocks, samples, exact, within1, max, timeouts;
        uint32_t hw_ticks, sw_ticks;
        /* ECHO_FFTCMP: the worst block, e_hw | e_sw << 8 | bits << 16, the OR of |hw| */
        uint32_t worst_at, worst_info, worst_m, worst_i;
    } hwf;
#endif
#if !defined(ECHO_NO_HWFFT) || defined(ECHO_HWTEST)
    uint32_t fft_blocks, fft_soft;      /* Vorbis, AAC: IMDCT blocks on HIFIACC, in software
                                           after a timeout (ETUDE §74, §76) */
#endif
#ifdef ECHO_BPROBE
    /* Test build only (0.99r17, rework step 4, ETUDE §102): what core B can reach. Core A sets
     * the addresses and the words it put there before releasing core B; core B runs the probe
     * before HP_READY, each access its own step (a fault is caught, recorded and passed over;
     * a hang leaves 'step' where it stopped) */
    struct {
        uint32_t req;                   /* A: BP_REQ: run the probe */
        uint32_t sys_addr, sys_word;    /* A: a word in SYSRAM (core A's BSS) and its value */
        uint32_t dram_addr, dram_word;  /* A: a constant in core A's code area (SYSRAM, DRAM) */
        uint32_t pmu_addr, pmu_word;    /* A: a word in PMU-SRAM and its value */
        uint32_t step;                  /* B: the step running (BP_* below), BP_DONE at the end */
        uint32_t got[8];                /* B: the word read at each step */
        uint32_t faults;                /* B: bit k: step k faulted */
        uint32_t cfsr, bfar, hfsr;      /* B: of the first fault */
        uint32_t sar0, sar1, sar_dt;    /* B: DMAC1 channel 5 SAR twice, TIMER1 ticks apart */
        uint32_t st_calib, st_core, st_ref;  /* B: SysTick CALIB; counts in 1 ms of TIMER1 on the
                                               core clock and on the reference clock */
        uint32_t st_irqs, st_load;      /* B: SysTick interrupts in 20 ms, its reload value */
        uint32_t b_div;                 /* A: core B's clock divider at the start */
        /* 0.99r27: core B's cycles (SysTick on its clock) for 32 KiB read from SYSRAM, HIGHRAM0,
         * HIGHRAM1, and for 1,000 reads of the DMA's SAR and of TIMER1 */
        uint32_t c_sys, c_hr0, c_hr1, c_sar, c_t1;
    } bp;
#endif
};
#define BP_REQ 0x42505231u              /* "BPR1" */
enum { BP_SYS_RD = 1, BP_SYS_WR, BP_DRAM_RD, BP_PMU_RD, BP_PMU_WR, BP_SAR, BP_SYSTICK, BP_DONE };
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
