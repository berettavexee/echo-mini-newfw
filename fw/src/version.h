/* Firmware identity, shown on screen and written in the image header */
#ifndef VERSION_H
#define VERSION_H

#define FW_NAME    "ECHO MINI newfw"
#ifdef ECHO_FFTCMP
#define FW_VERSION "0.98fc"            /* test build: AAC sound from HIFIACC, compared */
#elif defined(ECHO_HWTEST)
#define FW_VERSION "0.98hwt"           /* test build: accelerators against the software */
#else
#define FW_VERSION "0.98"
#endif
#define FW_DATE    "2026-10-29"

#endif
