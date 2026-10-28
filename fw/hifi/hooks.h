/* What our core B code provides to the third-party decoders through the hooks of our patches
 * (each patch declares its own; these prototypes let -Wmissing-prototypes check ours) */
#ifndef HOOKS_H
#define HOOKS_H

#include <stdint.h>

/* Tremor's mdct_backward (hififft.c): 1 = done on HIFIACC */
int echo_fft_imdct(int n, int32_t *in);
void echo_fft_check(int n, const int32_t *in);
/* pvmp3dec's synthesis (mp3synth.c, hwtest_synth.c): 1 = done on SYNTH */
int echo_synth_granule(const int32_t *work, int16_t *out, int nch, int ch);
void echo_synth_reset(void);
/* dr_flac's LPC subframes (codec_flac.c): 1 = done on HIFIACC */
int echo_hacc_block(int *y, unsigned n, unsigned order, int shift, const int *qlp);
/* libmpcdec's stream header check (codec_mpc.c) */
unsigned long mpc_crc32(unsigned char *buf, int len);

/* Tremor's malloc (heap.c): blocks below the floor are kept from one track to the next */
void heap_reset(void);
uint32_t heap_keep(void);
void heap_resume(uint32_t floor, uint32_t cur);
const uint8_t *heap_base(void);
extern uint32_t heap_peak, heap_cur;

/* Test builds only (ECHO_HWTEST, ECHO_HWSYN): hwtest_lpc.c, hwtest_synth.c */
void echo_lpc_hook(const int *y, unsigned n, unsigned bps, unsigned order, int shift,
                   unsigned prec, const int *qlp, unsigned sw_ticks);
void echo_synth_play(const int32_t *work, int16_t *out, int nch, int ch);
void echo_synth_cmp(const int16_t *hw, const int16_t *sw, int n);
void echo_synth_hook(const int32_t *work, const int16_t *out, int nch, int ch, uint32_t sw);
void echo_imdct_hook(const int32_t *x, uint32_t sw);
uint32_t echo_now(void);

#endif
