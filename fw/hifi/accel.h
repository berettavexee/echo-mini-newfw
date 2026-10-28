/* Core B's accelerators (docs/HIFIACC.md, docs/MP3ACC.md): HIFIACC (LPC, FFT), its DMA, the MP3
 * synthesis (SYNTH) and Imdct36. Addresses seen on the device. */
#ifndef ACCEL_H
#define ACCEL_H

#define HACC        0x01060000u
#define HACC_FIN    0x01060040u                 /* input FIFO */
#define HACC_FOUT   0x010600a0u                 /* output FIFO */
#define FFT_IM      0x01061000u                 /* FFT memory, imaginary parts */
#define FFT_RE      0x01062000u                 /* real parts */
#define HACC_COEF   0x0106b000u                 /* LPC coefficients */
#define DMA         0x01070000u
#define DMA_CH(c)   (DMA + (c) * 0x58u)
#define IMD         0x60010000u                 /* Imdct36 */
#define SYN         0x60050000u                 /* SYNTH: the 32 inputs and outputs */
#define SYN_HIST    0x60051000u                 /* its history */

#endif
