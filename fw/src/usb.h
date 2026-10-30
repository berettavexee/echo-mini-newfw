/* USB device on the Synopsys DWC2 OTG core (0x60080000), slave mode, polled, shared TX FIFO.
 * Endpoint 0 (control) and endpoint 1 IN and OUT (bulk) for a mass storage interface; or another
 * class (struct usb_class: the USB sound card, uac.c) with an isochronous OUT endpoint 1. */
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
    uint32_t iso_incomplete;    /* isochronous OUT frames missed (incomplete ISO OUT) */
    uint32_t iso_in_sent, iso_in_late;  /* isochronous IN packets sent, frames re-aimed */
    uint32_t irqs, irq_max_us;  /* interrupt mode: interrupts taken, the longest, us */
};

/* A class other than mass storage: its descriptors and requests. setup() gets the class requests
 * and SET_INTERFACE / GET_INTERFACE: it returns -1 to stall, 0 for no data, or the number of
 * bytes to send from 'reply' (at most 64); a request with data from the host (SET_CUR) comes to
 * setup_out() once the data is in. iso_out() gets each packet of the isochronous OUT endpoint: n
 * bytes to read from usb_rx_word(), (n + 3) / 4 words, all of them. */
struct usb_class {
    const uint8_t *dev, *cfg;   /* device and configuration descriptors */
    uint16_t cfg_len;
    int full_speed;             /* enumerate at full speed */
    int keep_audio_pins;        /* leave GPIO1_4, 2_0, 2_2 to the DACs (audio.c) */
    uint16_t rx_fifo_words;     /* RX FIFO size (0: the default, 147 of the 308 words) */
    uint16_t ptx_fifo_words;    /* periodic TX FIFO 1 (an isochronous IN endpoint), 0: none */
    int (*setup)(const uint8_t *s, uint8_t *reply);
    void (*setup_out)(const uint8_t *s, const uint8_t *data, uint32_t n);
    void (*iso_out)(uint32_t n);
    void (*iso_in_done)(void);  /* the isochronous IN packet went: usb_iso_in_send() the next */
    void (*reset)(void);        /* a bus reset: the stream stops */
};

#define USB_RX_FIFO 0x60081000u
static inline uint32_t usb_rx_word(void) { return *(volatile uint32_t *)USB_RX_FIFO; }

extern struct usb_state usb;

enum { USB_EABORT = -20, USB_ETIMEOUT = -21 };

void usb_start(void);           /* clocks, PHY, core reset, then connect */
void usb_start_class(const struct usb_class *c);    /* the same for another class */
void usb_iso_out_open(uint32_t mps);    /* endpoint 1 OUT, isochronous, every frame */
void usb_iso_out_close(void);
void usb_iso_in_open(uint32_t mps);     /* endpoint 1 IN, isochronous, periodic TX FIFO 1 */
void usb_iso_in_close(void);
void usb_iso_in_send(const void *data, uint32_t n);    /* the next packet, n <= mps */
void usb_stop(void);            /* soft disconnect */
void usb_service(void);         /* handles pending core events; call often */
/* Interrupt mode (the USB sound card): usb_service() runs from the controller's interrupt, not
 * from the caller; what the caller shares with it is guarded by usb_irq_block() */
void usb_irq(int on);
void usb_irq_block(int block);

/* Bulk OUT (endpoint 1): arm for up to len bytes, then poll; done when a short packet or len
 * bytes arrived. */
void usb_out_arm(void *buf, uint32_t len);
int usb_out_done(uint32_t *got);        /* 1 = done */
int usb_out(void *buf, uint32_t len, uint32_t *got);    /* blocking, with timeout */
/* Bulk IN (endpoint 1), blocking: len bytes, a zero-length packet when len is 0 */
int usb_in(const void *buf, uint32_t len);

#endif
