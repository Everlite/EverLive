/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_hw.h — Userspace FPGA bridge, GPIO and HDMI link initialization
 */

#ifndef GC573_USER_HW_H
#define GC573_USER_HW_H

#include "gc573-user-compat.h"

#define GC573_REG_VIDEO_SOURCE		0x01000
#define GC573_REG_VIDEO_STATUS		0x01004
#define GC573_REG_VIDEO_WIDTH		0x01008
#define GC573_REG_VIDEO_HEIGHT		0x0100c
#define GC573_REG_DDR_STATUS		0x0107c
#define GC573_REG_GPIO			0x00040

/* GPIO Pin Definitions */
#define GC573_GPIO_SPLITTER_ENABLE	1
#define GC573_GPIO_INPUT_HPD		2
#define GC573_GPIO_SPLITTER_SCDT	3
#define GC573_GPIO_SPLITTER_READY0	4
#define GC573_GPIO_SPLITTER_RESET	5
#define GC573_GPIO_SPLITTER_READY1	6
#define GC573_GPIO_TX1_HPD_GATE		7
#define GC573_GPIO_RECEIVER_RESET	8

/**
 * gc573_user_set_gpio - Set an FPGA GPIO pin state
 */
int gc573_user_set_gpio(void *bar0, unsigned int pin, bool high, unsigned int delay_ms);

/**
 * gc573_user_get_gpio - Get an FPGA GPIO pin state
 */
int gc573_user_get_gpio(void *bar0, unsigned int pin, bool *high);

/**
 * gc573_user_hw_init - Perform full FPGA bridge and link reset sequence
 *
 * Releases IT6805 / IT6664 from hardware reset, asserts HDMI HPD to console,
 * and sets up I2C bus controllers.
 */
int gc573_user_hw_init(void *bar0);

/**
 * gc573_user_bridge_reset - Perform bridge unit resets on register 0x0000c
 */
int gc573_user_bridge_reset(void *bar0);

/**
 * gc573_user_bridge_wait_for_ddr - Wait for FPGA DDR sampler gate (0x0107c)
 */
int gc573_user_bridge_wait_for_ddr(void *bar0);

/**
 * gc573_user_bridge_restore_for_led - DDR gate and VIP release after a core reset
 *
 * gc573_user_fpga_reset_dma() pulses 0x0000c bit 0 and returns the LED block
 * to the bitstream default. Profile 241 only sticks once this has finished.
 */
int gc573_user_bridge_restore_for_led(void *bar0);

/**
 * gc573_user_set_led_rgb - Set onboard RGB LED color via Userspace I2C MCU
 */
int gc573_user_set_led_rgb(void *bar0, u8 r, u8 g, u8 b);

/**
 * gc573_user_led_refresh - Re-program the same LED profile without logging
 *
 * Used by the teardown diagnostic and the background daemon to test whether
 * the LED block needs periodic re-arming after the video DMA stops.
 */
void gc573_user_led_refresh(void *bar0, u8 r, u8 g, u8 b);

#endif /* GC573_USER_HW_H */
