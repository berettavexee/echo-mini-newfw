/* Audio output (steps 5a, 5b): the two CS43131 DACs (I2C0), the ACODEC PLL and I2S1, and
 * DMAC1 channel 5 looping over a PCM ring of stereo frames: two 32-bit words (left, right,
 * data in the high bits), or one 32-bit word with two 16-bit samples (left in the low half):
 * I2S VDW = 16, HWT = 0; the I2S frame stays 64 SCLK, the DACs unchanged. The ring: core B's
 * output ring (96 KiB in HIGHRAM1, core B writes it, ETUDE §103) for our codecs, 96 KiB in SYSRAM
 * for the USB sound card (ETUDE §9, §20, §23). */
#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

struct audio_state {
    int on;
    uint8_t id[2][6];           /* DAC identification registers 0x010000-0x010005 */
    int id_err[2];
    uint8_t vol;                /* PCM_VOL value: attenuation = vol / 2 dB, 0xff = mute */
    uint32_t i2c_errors;
    uint32_t rate;              /* sample rate in use */
    /* ring holds packed 16-bit frames (4 bytes) instead of 32-bit (8) */
    int bits16;
    uint32_t frame_bytes;
    uint32_t ring, ring_size;   /* ring in use */
    /* stream (byte counters, modulo 2^32): rd from the DMA position, wr by the producer */
    uint32_t rd, wr, last_pos;
    uint32_t ring_ur, i2s_ur;   /* underrun episodes: producer behind the DMA; I2S FIFO empty */
    uint32_t ring_lost;         /* bytes the DMA played again because the producer was late */
    /* the same since boot, never reset: per track by difference */
    uint32_t ur_all, i2s_all, lost_all;
    int in_ur;
    int32_t min_fill;           /* lowest fill seen since the counters were reset, bytes */
    uint32_t t_upd;             /* ticks() of the last audio_update: laps the DMA position hides */
    uint32_t laps_lost;         /* whole ring laps found that way (since boot) */
};

extern struct audio_state audio;

/* power, DAC set-up, clocks, I2S, DMA, at 'rate' (44.1/48/88.2/96/176.4/192 kHz); muted */
int audio_start(uint32_t rate, int bits16);
void audio_stop(void);
int audio_change_rate(uint32_t rate, int bits16);   /* output on: only the rate-dependent part */
void audio_power_service(void);  /* DACs powered down 10 s into a pause */
void audio_pause(int on);       /* the DMA waits in place; the volume is kept */
void audio_flush(void);         /* drops what is queued (a skip) */
extern int audio_paused;
void audio_volume(uint8_t vol, int ramp);   /* both DACs, both channels */
void audio_update(void);        /* read position and underrun counters only */
uint32_t audio_free(void);      /* bytes that can be written now (whole 32-bit words) */
void audio_write(const void *src, uint32_t n);
/* A beep pattern mixed into what audio_write() writes next (music lowered meanwhile) */
enum { BEEP_WARN, BEEP_REMIND, BEEP_OFF };   /* 3 short, 1 short, 2 low */
void audio_beep(int kind);
extern void (*ramp_hook)(void); /* keeps the ring fed during volume ramps (else silence) */
int32_t audio_fill(void);       /* bytes written and not yet read */
/* A stream from outside (uac.c): the write position 'ahead' bytes after the DMA, silence in
 * between; silence for 'bytes' after the write position; silence when nothing comes */
void audio_stream_align(uint32_t ahead);
void audio_zero_ahead(uint32_t bytes);
void audio_idle(void);
void audio_reset_stats(void);
/* Our codecs (ETUDE §103): audio_start() on core B's output ring (else the SYSRAM ring); core B
 * then owns the write position (its pcm_wr) between attach and detach */
void audio_ring_direct(int on);
int audio_is_direct(void);
void audio_attach(volatile uint32_t *wr);
void audio_detach(void);
void audio_zero(uint32_t from, uint32_t n);
uint32_t audio_bps(void);
uint32_t audio_rebase(void);    /* the counters back by whole rings past 2^30; what was taken */

#endif
