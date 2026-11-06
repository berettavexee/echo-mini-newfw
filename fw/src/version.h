/* Firmware identity, shown on screen and written in the image header */
#ifndef VERSION_H
#define VERSION_H

#define FW_NAME    "ECHO MINI newfw"
#ifdef ECHO_UACDIAG
#define FW_VERSION "0.99ud"            /* test build: USB sound card diagnostics */
#elif defined(ECHO_FFTCMP)
#define FW_VERSION "0.99fc"            /* test build: AAC sound from HIFIACC, compared */
#elif defined(ECHO_BPROBE)
#define FW_VERSION "0.99r27p"          /* test build: what core B can reach, its costs (rework) */
#elif defined(ECHO_HWTEST)
#define FW_VERSION "0.99hwt"           /* test build: accelerators against the software */
#else
#define FW_VERSION "0.99r37"        /* the rework branch (ROADMAP, ETUDE §97) */
#endif
#define FW_DATE    "2026-11-01"

#endif
