/* Audio output (steps 5a, 5b): the two CS43131 DACs (I2C0), the ACODEC PLL and I2S1, and
 * DMAC1 channel 5 looping over a PCM ring of stereo frames: two 32-bit words (left, right,
 * data in the high bits), or one 32-bit word with two 16-bit samples (left in the low half):
 * I2S VDW = 16, HWT = 0; the I2S frame stays 64 SCLK, the DACs unchanged. Rings: 64 KiB in bank 3
 * of HIGHRAM1 (stock modules), 192 KiB in SYSRAM (our codecs: core A copies core B's output
 * ring into it, ETUDE §20, §23); the PMU-SRAM and HIGHRAM0 rings are test configurations. */
#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

enum { RING_HIGHRAM1, RING_PMU, RING_HIGHRAM0, RING_SYSRAM, RING_COUNT };

struct audio_state {
    int on;
    uint8_t id[2][6];           /* DAC identification registers 0x010000-0x010005 */
    int id_err[2];
    uint8_t vol;                /* PCM_VOL value: attenuation = vol / 2 dB, 0xff = mute */
    uint32_t i2c_errors;
    uint32_t rate;              /* sample rate in use */
    int bits16;                 /* ring holds packed 16-bit frames (4 bytes) instead of 32-bit (8) */
    uint32_t frame_bytes;
    int ring_sel;
    uint32_t ring, ring_size;   /* ring in use */
    /* stream (byte counters, modulo 2^32): rd from the DMA position, wr by the producer */
    uint32_t rd, wr, last_pos;
    uint32_t ring_ur, i2s_ur;   /* underrun episodes: producer behind the DMA; I2S FIFO empty */
    uint32_t ring_lost;         /* bytes the DMA played again because the producer was late */
    int in_ur;
    int32_t min_fill;           /* lowest fill seen since the counters were reset, bytes */
    int external;               /* wr is set by another producer (core B): never moved here */
};

extern struct audio_state audio;

/* power, DAC set-up, clocks, I2S, DMA, at 'rate' (44.1/48/88.2/96/176.4/192 kHz); muted */
int audio_start(uint32_t rate, int ring_sel, int bits16);
void audio_stop(void);
int audio_change_rate(uint32_t rate, int bits16);   /* output on: only the rate-dependent part */
void audio_power_service(void);  /* DACs powered down 10 s into a pause */
void audio_pause(int on);       /* the DMA waits in place; the volume is kept */
void audio_flush(void);         /* drops what is queued (a skip) */
extern int audio_paused;
void audio_volume(uint8_t vol, int ramp);   /* both DACs, both channels */
uint32_t audio_dma_pos(void);   /* byte offset of the DMA in the ring */
uint32_t audio_dma_err(void);   /* DMAC1 raw error bits */
uint32_t audio_i2s_fifo(void);  /* TX FIFO level */
/* Stream test producer: updates the read position and the underrun counters, then (unless
 * 'stall') writes a 1 kHz tone (or silence) ahead of the DMA up to a 4 KiB guard */
void audio_service(int tone, int stall);
void audio_update(void);        /* read position and underrun counters only */
uint32_t audio_free(void);      /* bytes that can be written now (whole 32-bit words) */
void audio_write(const void *src, uint32_t n);
extern void (*ramp_hook)(void); /* called during volume ramps instead of the tone producer */
int32_t audio_fill(void);       /* bytes written and not yet read */
void audio_reset_stats(void);

#endif
