/* Firmware identity, shown on screen and written in the image header */
#ifndef VERSION_H
#define VERSION_H

#define FW_NAME    "ECHO MINI newfw"
#ifdef ECHO_UACDIAG
#define FW_VERSION "0.102ud"            /* test build: USB sound card diagnostics */
#elif defined(ECHO_FFTCMP)
#define FW_VERSION "0.102fc"            /* test build: AAC sound from HIFIACC, compared */
#elif defined(ECHO_BPROBE)
#define FW_VERSION "0.102p"          /* test build: what core B can reach, its costs (rework) */
#elif defined(ECHO_LIBBUILD)
#define FW_VERSION "0.102lb"           /* test build: the library's index built at power-on */
#elif defined(ECHO_LIBCREATE)
#define FW_VERSION "0.102lc"           /* test build: the library's file created at power-on */
#elif defined(ECHO_HWTEST)
#define FW_VERSION "0.102hwt"           /* test build: accelerators against the software */
#else
#define FW_VERSION "0.102b6"           /* test build: the library built on the device (§126) */
#endif
#define FW_DATE    "2026-11-07"

#endif
