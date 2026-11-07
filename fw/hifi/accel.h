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
#define HACC_ACOEF  0x01069000u                 /* ALAC filter's coefficients (ETUDE §113) */
#define NVIC_ISPR0  0xe000e200u                 /* core B's pending interrupts */
#define NVIC_ICPR0  0xe000e280u                 /* (write 1 to clear) */
#define IRQ_IMD     7                           /* Imdct36's end (stock module 73, §113) */
#define IRQ_SYN     1                           /* SYNTH's */
#define DMA         0x01070000u
#define DMA_CH(c)   (DMA + (c) * 0x58u)
#define IMD         0x60010000u                 /* Imdct36 */
#define SYN         0x60050000u                 /* SYNTH: the 32 inputs and outputs */
#define SYN_HIST    0x60051000u                 /* its history */

#endif
