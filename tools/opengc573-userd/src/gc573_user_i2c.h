/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_i2c.h — Userspace I2C master driver over FPGA BAR0 MMIO
 */

#ifndef GC573_USER_I2C_H
#define GC573_USER_I2C_H

#include "gc573-user-compat.h"

#define GC573_I2C_BUS_PRIMARY		0
#define GC573_I2C_BUS_SECONDARY		1

/* Known I2C addresses on GC573 */
#define GC573_I2C_ADDR_IT6805		0x48	/* IT6805 HDMI 2.0 Receiver */
#define GC573_I2C_ADDR_IT6664		0x5b	/* IT6664 HDMI 2.0 Transmitter / Passthrough */
#define GC573_I2C_ADDR_MCU_LED		0x20	/* RGB LED MCU */

/**
 * gc573_user_i2c_init - Configure I2C bus clock divisors on FPGA
 * @bar0: Pointer to mapped BAR0 memory
 */
void gc573_user_i2c_init(void *bar0);

/**
 * gc573_user_i2c_read - Read bytes from an I2C slave register
 * @bar0: Pointer to mapped BAR0 memory
 * @bus: I2C bus index (0 = primary, 1 = secondary)
 * @addr: 7-bit I2C slave address
 * @subaddr: Internal register subaddress
 * @subaddr_len: Length of subaddress in bytes (1 to 4)
 * @data: Buffer to receive data
 * @data_len: Number of bytes to read
 *
 * Returns 0 on success, negative errno on failure.
 */
int gc573_user_i2c_read(void *bar0, unsigned int bus, u16 addr,
			u32 subaddr, unsigned int subaddr_len,
			u8 *data, unsigned int data_len);

/**
 * gc573_user_i2c_write - Write bytes to an I2C slave register
 * @bar0: Pointer to mapped BAR0 memory
 * @bus: I2C bus index (0 = primary, 1 = secondary)
 * @addr: 7-bit I2C slave address
 * @subaddr: Internal register subaddress
 * @subaddr_len: Length of subaddress in bytes (1 to 4)
 * @data: Buffer containing data to write
 * @data_len: Number of bytes to write
 *
 * Returns 0 on success, negative errno on failure.
 */
int gc573_user_i2c_write(void *bar0, unsigned int bus, u16 addr,
			 u32 subaddr, unsigned int subaddr_len,
			 const u8 *data, unsigned int data_len);

#endif /* GC573_USER_I2C_H */
