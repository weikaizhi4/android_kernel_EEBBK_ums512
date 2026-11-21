/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * NT36532 protocol definitions adapted from Novatek's GPL drivers.
 * Copyright (C) 2010 - 2022 Novatek, Inc.
 * A7 memory maps checked against A7_boot_touchfix_KSU_SUSFS.img.
 */
#ifndef _NVT_A7_NT36532_H
#define _NVT_A7_NT36532_H

#include <linux/types.h>
#include <linux/errno.h>

#define NVT_MAX_TOUCHES		10
#define NVT_TOUCH_DATA_LEN	65
#define NVT_FW_INFO_LEN		38
#define NVT_MAX_PRESSURE		1000
#define NVT_FW_ADDR		0x01
#define NVT_HW_ADDR		0x62
#define NVT_CASCADE_ADDR		0x1fb12c
#define NVT_RESET_STATE		0x60
#define NVT_FW_INFO		0x78
#define NVT_HOST_CMD		0x50
#define NVT_RESET_INIT		0xa0
#define NVT_RESET_REK		0xa1
#define NVT_RESET_MAX		0xaf

struct nvt_hw_reg {
	u32 trim;
	u32 reset;
	u32 crc;
};

static const struct nvt_hw_reg nvt_hw_regs[] = {
	{ 0x1fb104, 0x1fb43e, 0x1fb535 },
	{ 0x03f004, 0, 0x03f135 },
	{ 0x01f64e, 0, 0 },
};

struct nvt_ts_mem_map {
	u32 EVENT_BUF_ADDR;
	u32 RAW_PIPE0_ADDR;
	u32 RAW_PIPE1_ADDR;
	u32 BASELINE_ADDR;
	u32 BASELINE_BTN_ADDR;
	u32 DIFF_PIPE0_ADDR;
	u32 DIFF_PIPE1_ADDR;
	u32 RAW_BTN_PIPE0_ADDR;
	u32 RAW_BTN_PIPE1_ADDR;
	u32 DIFF_BTN_PIPE0_ADDR;
	u32 DIFF_BTN_PIPE1_ADDR;
	u32 READ_FLASH_CHECKSUM_ADDR;
	u32 RW_FLASH_DATA_ADDR;
};

static const struct nvt_ts_mem_map NT36532_single_memory_map = {
	.EVENT_BUF_ADDR = 0x125800,
	.RAW_PIPE0_ADDR = 0x10c1ec,
	.RAW_PIPE1_ADDR = 0x10c1ec,
	.BASELINE_ADDR = 0x10b1ec,
	.DIFF_PIPE0_ADDR = 0x11bfc4,
	.DIFF_PIPE1_ADDR = 0x11cfc4,
	.READ_FLASH_CHECKSUM_ADDR = 0x100000,
	.RW_FLASH_DATA_ADDR = 0x100002,
};

static const struct nvt_ts_mem_map NT36532_cascade_memory_map = {
	.EVENT_BUF_ADDR = 0x125800,
	.RAW_PIPE0_ADDR = 0x10b200,
	.RAW_PIPE1_ADDR = 0x10b200,
	.BASELINE_ADDR = 0x109e00,
	.DIFF_PIPE0_ADDR = 0x128140,
	.DIFF_PIPE1_ADDR = 0x129540,
	.READ_FLASH_CHECKSUM_ADDR = 0x100000,
	.RW_FLASH_DATA_ADDR = 0x100002,
};

struct nvt_fw_info {
	u8 version;
	u8 x_num;
	u8 y_num;
	u16 x_max;
	u16 y_max;
	u16 pid;
};

struct nvt_point {
	u8 slot;
	u8 width;
	u16 x;
	u16 y;
	u16 pressure;
};

static inline bool nvt_is_nt36532(const u8 *id)
{
	/* The first three trim bytes are not part of the product ID. */
	return id[3] == 0x32 && id[4] == 0x65 && id[5] == 0x03;
}

static inline bool nvt_is_recovery_packet(const u8 *data)
{
	unsigned int i;

	for (i = 0; i < 6; i++)
		if (data[i] != 0x77)
			return false;
	return true;
}

/* Buffers below contain payload only, without the I2C register byte. */
static inline int nvt_decode_fw_info(const u8 *data, struct nvt_fw_info *info)
{
	u16 x = ((u16)data[4] << 8) | data[5];
	u16 y = ((u16)data[6] << 8) | data[7];

	if (data[0] + data[1] != 0xff || !data[2] || !data[3] ||
	    !x || !y || x > 0xfff || y > 0xfff)
		return -EINVAL;

	info->version = data[0];
	info->x_num = data[2];
	info->y_num = data[3];
	info->x_max = x;
	info->y_max = y;
	info->pid = data[34] | ((u16)data[35] << 8);
	return 0;
}

static inline bool nvt_decode_point(const u8 *data, unsigned int index,
				    const struct nvt_fw_info *info,
				    struct nvt_point *point)
{
	const u8 *p;
	u8 id, state;
	u16 x, y, pressure;

	if (index >= NVT_MAX_TOUCHES)
		return false;
	p = data + index * 6;
	id = p[0] >> 3;
	state = p[0] & 7;
	if (!id || id > NVT_MAX_TOUCHES || (state != 1 && state != 2))
		return false;

	x = ((u16)p[1] << 4) | (p[3] >> 4);
	y = ((u16)p[2] << 4) | (p[3] & 0xf);
	if (x > info->x_max || y > info->y_max)
		return false;

	pressure = p[5];
	if (index < 2)
		pressure |= (u16)data[62 + index] << 8;
	if (pressure > NVT_MAX_PRESSURE)
		pressure = NVT_MAX_PRESSURE;

	point->slot = id - 1;
	point->x = x;
	point->y = y;
	point->width = p[4] ? p[4] : 1;
	point->pressure = pressure ? pressure : 1;
	return true;
}

#endif
