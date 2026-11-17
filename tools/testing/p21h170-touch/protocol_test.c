// SPDX-License-Identifier: GPL-2.0
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "nt36532.h"

static void encode_point(u8 *data, unsigned int index, u8 id, u8 state,
			 u16 x, u16 y, u8 width, u16 pressure)
{
	u8 *p = data + 6 * index;

	p[0] = (id << 3) | state;
	p[1] = x >> 4;
	p[2] = y >> 4;
	p[3] = ((x & 15) << 4) | (y & 15);
	p[4] = width;
	p[5] = pressure;
	if (index < 2)
		data[62 + index] = pressure >> 8;
}

int main(void)
{
	/* Firmware payload fixture: max X=1280, max Y=1752, PID=0x1234. */
	u8 fw[NVT_FW_INFO_LEN] = {
		0x27, 0xd8, 32, 44, 0x05, 0, 0x06, 0xd8,
		[34] = 0x34, [35] = 0x12,
	};
	u8 data[NVT_TOUCH_DATA_LEN] = { 0 };
	u8 id[] = { 0x5a, 0x19, 0x00, 0x32, 0x65, 0x03 };
	struct nvt_fw_info info = { 0 }, before;
	struct nvt_point point;
	unsigned int index;

	assert(nvt_is_nt36532(id));
	id[3] = 0x25;
	assert(!nvt_is_nt36532(id));
	assert(!nvt_decode_fw_info(fw, &info));
	assert(info.version == 0x27 && info.pid == 0x1234);
	assert(info.x_max == 1280 && info.y_max == 1752);
	before = info;
	fw[1] = 0;
	assert(nvt_decode_fw_info(fw, &info) == -EINVAL);
	assert(!memcmp(&before, &info, sizeof(info)));
	fw[1] = 0xd8;
	fw[2] = 0;
	assert(nvt_decode_fw_info(fw, &info) == -EINVAL);
	fw[2] = 32;
	fw[4] = 0x10;
	assert(nvt_decode_fw_info(fw, &info) == -EINVAL);
	fw[4] = 0;
	assert(nvt_decode_fw_info(fw, &info) == -EINVAL);
	fw[4] = 0x05;
	assert(!nvt_decode_fw_info(fw, &info));

	/* Slot IDs differ from record indices; use mixed coordinate nibbles. */
	encode_point(data, 0, 10, 1, 0x4af, 0x6b2, 7, 700);
	assert(nvt_decode_point(data, 0, &info, &point));
	assert(point.slot == 9 && point.x == 0x4af && point.y == 0x6b2);
	assert(point.width == 7 && point.pressure == 700);
	encode_point(data, 1, 1, 2, 1280, 1752, 0, 0xffff);
	assert(nvt_decode_point(data, 1, &info, &point));
	assert(point.slot == 0 && point.width == 1 && point.pressure == 1000);
	encode_point(data, 2, 2, 1, 0, 0, 1, 0);
	assert(nvt_decode_point(data, 2, &info, &point));
	assert(point.pressure == 1);

	encode_point(data, 0, 0, 1, 1, 1, 1, 1);
	assert(!nvt_decode_point(data, 0, &info, &point));
	encode_point(data, 0, 11, 1, 1, 1, 1, 1);
	assert(!nvt_decode_point(data, 0, &info, &point));
	encode_point(data, 0, 1, 3, 1, 1, 1, 1);
	assert(!nvt_decode_point(data, 0, &info, &point));
	encode_point(data, 0, 1, 1, 1281, 1, 1, 1);
	assert(!nvt_decode_point(data, 0, &info, &point));
	encode_point(data, 0, 1, 1, 1, 1753, 1, 1);
	assert(!nvt_decode_point(data, 0, &info, &point));
	assert(!nvt_decode_point(data, 10, &info, &point));

	/* All ten packet records must map cleanly to their own slots. */
	for (index = 0; index < NVT_MAX_TOUCHES; index++) {
		encode_point(data, index, index + 1, 2,
			     11 * index, 17 * index, 1, 20);
		assert(nvt_decode_point(data, index, &info, &point));
		assert(point.slot == index && point.x == 11 * index);
	}
	memset(data, 0x77, 6);
	assert(nvt_is_recovery_packet(data));
	data[5] = 0;
	assert(!nvt_is_recovery_packet(data));
	puts("PASS: IDs, firmware, 10 slots, pressure and malformed packets");
	return 0;
}
