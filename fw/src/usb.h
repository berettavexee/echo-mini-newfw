/* USB device on the Synopsys DWC2 OTG core (0x60080000), slave mode, polled, shared TX FIFO.
 * Endpoint 0 (control) and endpoint 1 IN and OUT (bulk) for a mass storage interface. */
#ifndef USB_H
#define USB_H

#include <stdint.h>

struct usb_state {
    int configured;             /* SET_CONFIGURATION 1 received */
    int high_speed;             /* speed of the last enumeration */
    uint32_t mps;               /* bulk packet size: 512 (high speed) or 64 */
    uint32_t resets, setups;    /* counters for the screen */
    int bot_reset;              /* mass storage reset request seen, cleared by the class code */
    int suspended;              /* no bus activity (host gone, port off, or asleep) */
    uint32_t suspend_since;     /* ticks() at the suspend */
};

extern struct usb_state usb;

enum { USB_EABORT = -20, USB_ETIMEOUT = -21 };

void usb_start(void);           /* clocks, PHY, core reset, then connect */
void usb_stop(void);            /* soft disconnect */
void usb_service(void);         /* handles pending core events; call often */

/* Bulk OUT (endpoint 1): arm for up to len bytes, then poll; done when a short packet or len
 * bytes arrived. */
void usb_out_arm(void *buf, uint32_t len);
int usb_out_done(uint32_t *got);        /* 1 = done */
int usb_out(void *buf, uint32_t len, uint32_t *got);    /* blocking, with timeout */
/* Bulk IN (endpoint 1), blocking: len bytes, a zero-length packet when len is 0 */
int usb_in(const void *buf, uint32_t len);

#endif
