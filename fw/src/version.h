/* Firmware identity, shown on screen and written in the image header */
#ifndef VERSION_H
#define VERSION_H

#define FW_NAME    "ECHO MINI newfw"
#ifdef ECHO_UACDIAG
#define FW_VERSION "0.104ud"            /* test build: USB sound card diagnostics */
#elif defined(ECHO_FFTCMP)
#define FW_VERSION "0.104fc"            /* test build: AAC sound from HIFIACC, compared */
#elif defined(ECHO_BPROBE)
#define FW_VERSION "0.104p"          /* test build: what core B can reach, its costs (rework) */
#elif defined(ECHO_LIBBUILD)
#define FW_VERSION "0.104lb"           /* test build: the library's index built at power-on */
#elif defined(ECHO_LIBCREATE)
#define FW_VERSION "0.104lc"           /* test build: the library's file created at power-on */
#elif defined(ECHO_LCDCAL)
#define FW_VERSION "0.104cal"         /* test build: the screen's DMA variants (ETUDE §135) */
#elif defined(ECHO_LCDPERF)
#define FW_VERSION "0.104lp"            /* test build: the screen's costs (ETUDE §132) */
#elif defined(ECHO_HWTEST)
#define FW_VERSION "0.104hwt"           /* test build: accelerators against the software */
#else
#define FW_VERSION "0.104v1"
#endif
#define FW_DATE    "2026-11-14"

#endif
