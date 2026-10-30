/* I2C0 master (Rockchip I2C controller, TRM chapter 22), polled, for the two CS43131 DACs */
#ifndef I2C_H
#define I2C_H

#include <stdint.h>

enum { I2C_ETIMEOUT = -30, I2C_ENAK = -31 };

void i2c_init(void);
/* CS43131 register access: 24-bit register address, 8-bit value; dev = 8-bit write address */
int i2c_reg_write(uint8_t dev, uint32_t reg, uint8_t val);
int i2c_reg_read(uint8_t dev, uint32_t reg, uint8_t *val);

extern uint32_t i2c_errors;

/* I2C1 (pins C), the real-time clock: 8-bit register addresses; dev = 8-bit write address */
void i2c1_init(void);
int i2c1_read(uint8_t dev, uint8_t reg, uint8_t *buf, uint32_t n);
int i2c1_write(uint8_t dev, uint8_t reg, const uint8_t *buf, uint32_t n);

/* Called while waiting for the bus: what must not wait for a whole transfer (the USB sound
 * card's packets, uac.c); 0: none */
extern void (*i2c_hook)(void);

#endif
