// SPDX-License-Identifier: GPL-2.0-only
/*
 * NT36532 I2C finger touch support for EEBBK A7 (P21H170).
 *
 * Protocol adapted from Novatek GPL drivers published by LineageOS
 * (I2C) and MiCode (NT36532 registers). Copyright (C) 2010 - 2022
 * Novatek, Inc. See Documentation/input/p21h170-novatek-touch.rst.
 *
 * Uses the panel's existing flash firmware. Display power and touch
 * initialization are serialized independently of the IRQ's I2C lock.
 */
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <linux/sprd_drm_notifier.h>
#include <linux/workqueue.h>

#include "nt36532.h"

#define NVT_NAME "NVT-ts"
#define NVT_PANEL "lcd_nt36532_tianma_mipi_fhd_bbk_H170"
#define NVT_RETRY_LIMIT 5

extern const char *lcd_str;

struct nvt_ts_data {
	struct i2c_client *client;
	struct input_dev *input;
	struct notifier_block disp_notifier;
	struct delayed_work retry_work;
	/* state_lock -> io_lock; the IRQ takes io_lock only. */
	struct mutex state_lock;
	struct mutex io_lock;
	const struct nvt_ts_mem_map *mmap;
	const struct nvt_hw_reg *hw_reg;
	struct nvt_fw_info info;
	int reset_gpio;
	int irq_gpio;
	int irq;
	int last_error;
	unsigned int attempts;
	unsigned int retry_count;
	bool ready;
	bool awake;
	bool irq_requested;
	bool irq_enabled;
	bool display_on;
	bool stopping;
	bool notifier_registered;
	bool cascade;
	u8 chip_id[6];
	/* Heap buffers also work with I2C adapters that use DMA. */
	u8 xfer_buf[NVT_TOUCH_DATA_LEN + 1];
	char phys[64];
};

/* All bus operations below require io_lock. Normalize short transfers. */
static int nvt_i2c_write(struct nvt_ts_data *ts, u16 addr, u8 reg,
			 const u8 *data, size_t len)
{
	struct i2c_msg msg = {
		.addr = addr,
		.buf = ts->xfer_buf,
		.len = len + 1,
	};
	int ret = -EIO, retry;

	if (len > NVT_TOUCH_DATA_LEN)
		return -EINVAL;
	ts->xfer_buf[0] = reg;
	memcpy(ts->xfer_buf + 1, data, len);
	for (retry = 0; retry < 5; retry++) {
		ret = i2c_transfer(ts->client->adapter, &msg, 1);
		if (ret == 1)
			return 0;
		usleep_range(1000, 2000);
	}
	return ret < 0 ? ret : -EIO;
}

static int nvt_i2c_read(struct nvt_ts_data *ts, u8 reg, u8 *data, size_t len)
{
	struct i2c_msg msgs[] = {
		{ .addr = NVT_FW_ADDR, .len = 1, .buf = ts->xfer_buf },
		{ .addr = NVT_FW_ADDR, .flags = I2C_M_RD,
		  .len = len, .buf = ts->xfer_buf + 1 },
	};
	int ret = -EIO, retry;

	if (!len || len > NVT_TOUCH_DATA_LEN)
		return -EINVAL;
	ts->xfer_buf[0] = reg;
	for (retry = 0; retry < 5; retry++) {
		ret = i2c_transfer(ts->client->adapter, msgs, ARRAY_SIZE(msgs));
		if (ret == ARRAY_SIZE(msgs)) {
			memcpy(data, ts->xfer_buf + 1, len);
			return 0;
		}
		usleep_range(1000, 2000);
	}
	return ret < 0 ? ret : -EIO;
}

static int nvt_set_page(struct nvt_ts_data *ts, u32 addr)
{
	u8 page[] = { addr >> 16, addr >> 8 };

	return nvt_i2c_write(ts, NVT_FW_ADDR, 0xff, page, sizeof(page));
}

static int nvt_read_addr(struct nvt_ts_data *ts, u32 addr, u8 *data, size_t len)
{
	int ret = nvt_set_page(ts, addr);

	return ret ? ret : nvt_i2c_read(ts, addr & 0xff, data, len);
}

static int nvt_write_addr(struct nvt_ts_data *ts, u32 addr, u8 value)
{
	int ret = nvt_set_page(ts, addr);

	return ret ? ret : nvt_i2c_write(ts, NVT_FW_ADDR, addr & 0xff,
					&value, 1);
}

static int nvt_reset(struct nvt_ts_data *ts, bool idle)
{
	u8 cmd = idle ? 0xaa : 0x69;
	int ret;

	if (ts->hw_reg->reset) {
		ret = nvt_write_addr(ts, ts->hw_reg->reset, cmd);
	} else {
		if (idle)
			cmd = 0xa5;
		ret = nvt_i2c_write(ts, NVT_HW_ADDR, 0, &cmd, 1);
	}
	msleep(idle ? 15 : 35);
	return ret;
}

static int nvt_stop_crc_reboot(struct nvt_ts_data *ts)
{
	u8 value;
	int ret, retry;

	if (!ts->hw_reg->crc)
		return -ENODEV;
	for (retry = 0; retry < 5; retry++) {
		ret = nvt_reset(ts, true);
		if (ret)
			continue;
		ret = nvt_reset(ts, true);
		if (ret)
			continue;
		ret = nvt_write_addr(ts, ts->hw_reg->crc, 0xa5);
		if (ret)
			continue;
		ret = nvt_read_addr(ts, ts->hw_reg->crc, &value, 1);
		if (!ret && value == 0xa5)
			return 0;
	}
	return -EIO;
}

static int nvt_identify(struct nvt_ts_data *ts)
{
	u8 id[6], cascade, zeroes[6] = { 0 }, unlock = 0x35;
	int ret = -ENODEV, map, retry;

	for (map = 0; map < ARRAY_SIZE(nvt_hw_regs); map++) {
		ts->hw_reg = &nvt_hw_regs[map];
		ret = nvt_reset(ts, false);
		if (ret)
			continue;
		for (retry = 0; retry < 5; retry++) {
			ret = nvt_reset(ts, true);
			if (ret)
				continue;
			if (!ts->hw_reg->reset) {
				ret = nvt_i2c_write(ts, NVT_HW_ADDR, 0,
						    &unlock, 1);
				if (ret)
					continue;
				usleep_range(10000, 11000);
			}
			ret = nvt_set_page(ts, ts->hw_reg->trim);
			if (ret)
				continue;
			/* Trim read preparation used in the working image. */
			ret = nvt_i2c_write(ts, NVT_FW_ADDR,
					    ts->hw_reg->trim & 0xff,
					    zeroes, sizeof(zeroes));
			if (ret)
				continue;
			ret = nvt_i2c_read(ts, ts->hw_reg->trim & 0xff,
					   id, sizeof(id));
			if (ret)
				continue;
			if (id[0] == 0xfc ||
			    (id[0] == 0xff && id[1] == 0xff &&
			     id[2] == 0xff)) {
				nvt_stop_crc_reboot(ts);
				ret = -EIO;
				continue;
			}
			if (!nvt_is_nt36532(id)) {
				ret = -ENODEV;
				usleep_range(10000, 11000);
				continue;
			}
			/* NT36532 uses the new reset/CRC register mapping. */
			ts->hw_reg = &nvt_hw_regs[0];
			ret = nvt_read_addr(ts, NVT_CASCADE_ADDR, &cascade, 1);
			if (ret)
				return ret;
			ts->cascade = !(cascade & 1);
			ts->mmap = ts->cascade ? &NT36532_cascade_memory_map :
						 &NT36532_single_memory_map;
			memcpy(ts->chip_id, id, sizeof(id));
			dev_info(&ts->client->dev, "NT36532 %s, trim %*ph\n",
				 ts->cascade ? "cascade" : "single", 6, id);
			return 0;
		}
	}
	return ret;
}

static int nvt_wait_ready(struct nvt_ts_data *ts, u8 state)
{
	u8 status[5];
	int ret, retry;

	for (retry = 0; retry < 101; retry++) {
		usleep_range(10000, 11000);
		ret = nvt_read_addr(ts,
				    ts->mmap->EVENT_BUF_ADDR + NVT_RESET_STATE,
				    status, sizeof(status));
		if (ret)
			return ret;
		if (status[0] >= state && status[0] <= NVT_RESET_MAX)
			return 0;
	}
	return -ETIMEDOUT;
}

static int nvt_get_fw_info(struct nvt_ts_data *ts)
{
	u8 info[NVT_FW_INFO_LEN];
	int ret = -EINVAL, retry;

	for (retry = 0; retry < 4; retry++) {
		ret = nvt_read_addr(ts, ts->mmap->EVENT_BUF_ADDR + NVT_FW_INFO,
				    info, sizeof(info));
		if (!ret)
			ret = nvt_decode_fw_info(info, &ts->info);
		if (!ret)
			return 0;
	}
	return ret;
}

static void nvt_release_touches(struct nvt_ts_data *ts)
{
	int i;

	for (i = 0; i < NVT_MAX_TOUCHES; i++) {
		input_mt_slot(ts->input, i);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, false);
	}
	input_mt_sync_frame(ts->input);
	input_report_key(ts->input, BTN_TOUCH, 0);
	input_sync(ts->input);
}

static irqreturn_t nvt_ts_irq(int irq, void *data)
{
	struct nvt_ts_data *ts = data;
	struct nvt_point point;
	u8 points[NVT_TOUCH_DATA_LEN];
	bool pressed[NVT_MAX_TOUCHES] = { false };
	bool any = false;
	int ret, i;

	mutex_lock(&ts->io_lock);
	if (!ts->ready || !ts->awake)
		goto out;
	/* Restore the event page after reset/status transactions. */
	ret = nvt_read_addr(ts, ts->mmap->EVENT_BUF_ADDR,
			    points, sizeof(points));
	if (ret) {
		dev_err_ratelimited(&ts->client->dev,
				    "touch read failed: %d\n", ret);
		/* Do not leave contacts held after a bus failure. */
		nvt_release_touches(ts);
		goto out;
	}
	if (nvt_is_recovery_packet(points)) {
		nvt_release_touches(ts);
		goto out;
	}
	for (i = 0; i < NVT_MAX_TOUCHES; i++) {
		if (!nvt_decode_point(points, i, &ts->info, &point))
			continue;
		if (pressed[point.slot])
			continue;
		pressed[point.slot] = true;
		any = true;
		input_mt_slot(ts->input, point.slot);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, true);
		input_report_abs(ts->input, ABS_MT_POSITION_X, point.x);
		input_report_abs(ts->input, ABS_MT_POSITION_Y, point.y);
		input_report_abs(ts->input, ABS_MT_TOUCH_MAJOR, point.width);
		input_report_abs(ts->input, ABS_MT_PRESSURE, point.pressure);
	}
	for (i = 0; i < NVT_MAX_TOUCHES; i++) {
		if (pressed[i])
			continue;
		input_mt_slot(ts->input, i);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, false);
	}
	input_mt_sync_frame(ts->input);
	input_report_key(ts->input, BTN_TOUCH, any);
	input_sync(ts->input);
out:
	mutex_unlock(&ts->io_lock);
	return IRQ_HANDLED;
}

/* Called with both locks held; no IRQ exists on entry. */
static int nvt_ts_late_probe(struct nvt_ts_data *ts)
{
	struct input_dev *input;
	unsigned long flags;
	int ret;

	ret = nvt_identify(ts);
	if (ret)
		return ret;
	ret = nvt_reset(ts, false);
	if (ret)
		return ret;
	ret = nvt_wait_ready(ts, NVT_RESET_INIT);
	if (ret)
		return ret;
	ret = nvt_get_fw_info(ts);
	if (ret)
		return ret;

	input = input_allocate_device();
	if (!input)
		return -ENOMEM;
	input->name = "NVTCapacitiveTouchScreen";
	input->phys = ts->phys;
	input->id.bustype = BUS_I2C;
	input->dev.parent = &ts->client->dev;
	input_set_capability(input, EV_KEY, BTN_TOUCH);
	__set_bit(INPUT_PROP_DIRECT, input->propbit);
	input_set_abs_params(input, ABS_MT_POSITION_X, 0, ts->info.x_max, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, ts->info.y_max, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(input, ABS_MT_PRESSURE, 0, NVT_MAX_PRESSURE, 0, 0);
	ret = input_mt_init_slots(input, NVT_MAX_TOUCHES, INPUT_MT_DIRECT);
	if (ret)
		goto free_input;
	ret = input_register_device(input);
	if (ret)
		goto free_input;

	ts->input = input;
	ts->ready = true;
	ts->awake = true;
	flags = irq_get_trigger_type(ts->irq);
	if (!flags)
		flags = IRQF_TRIGGER_RISING;
	ret = request_threaded_irq(ts->irq, NULL, nvt_ts_irq,
				   flags | IRQF_ONESHOT, NVT_NAME, ts);
	if (ret) {
		ts->ready = false;
		ts->awake = false;
		ts->input = NULL;
		input_unregister_device(input);
		return ret;
	}
	ts->irq_requested = true;
	ts->irq_enabled = true;
	dev_info(&ts->client->dev,
		 "touch ready: FW=%02x PID=%04x range=%ux%u IRQ=%d\n",
		 ts->info.version, ts->info.pid,
		 ts->info.x_max, ts->info.y_max, ts->irq);
	return 0;

free_input:
	input_free_device(input);
	return ret;
}

/* state_lock held. Never disable/synchronize the IRQ while holding io_lock. */
static int nvt_enable(struct nvt_ts_data *ts)
{
	int ret = 0;

	if (ts->stopping || !ts->display_on)
		return -ENODEV;
	if (ts->ready && ts->awake)
		return 0;
	ts->attempts++;
	mutex_lock(&ts->io_lock);
	if (!ts->ready) {
		ret = nvt_ts_late_probe(ts);
	} else {
		gpio_set_value_cansleep(ts->reset_gpio, 1);
		ret = nvt_wait_ready(ts, NVT_RESET_REK);
		if (ret) {
			ret = nvt_reset(ts, false);
			if (!ret)
				ret = nvt_wait_ready(ts, NVT_RESET_REK);
		}
		if (!ret)
			ts->awake = true;
	}
	mutex_unlock(&ts->io_lock);
	if (!ret && ts->irq_requested && !ts->irq_enabled) {
		enable_irq(ts->irq);
		ts->irq_enabled = true;
	}
	ts->last_error = ret;
	return ret;
}

/* state_lock held. The display notifier runs before panel power removal. */
static void nvt_suspend(struct nvt_ts_data *ts)
{
	int ret;

	if (ts->irq_requested && ts->irq_enabled) {
		disable_irq(ts->irq);
		ts->irq_enabled = false;
	}
	mutex_lock(&ts->io_lock);
	if (ts->ready && ts->awake) {
		ts->awake = false;
		ret = nvt_write_addr(ts, ts->mmap->EVENT_BUF_ADDR +
				     NVT_HOST_CMD, 0x11);
		if (ret)
			dev_warn(&ts->client->dev,
				 "sleep command failed: %d\n", ret);
		nvt_release_touches(ts);
		msleep(50);
	}
	mutex_unlock(&ts->io_lock);
}

static void nvt_enable_or_retry(struct nvt_ts_data *ts)
{
	int ret = nvt_enable(ts);

	if (!ret)
		return;
	dev_warn(&ts->client->dev,
		 "touch initialization/resume failed: %d\n", ret);
	if (!ts->stopping && ts->display_on &&
	    ts->retry_count++ < NVT_RETRY_LIMIT)
		mod_delayed_work(system_wq, &ts->retry_work,
				 msecs_to_jiffies(500));
}

static void nvt_retry_work(struct work_struct *work)
{
	struct nvt_ts_data *ts = container_of(to_delayed_work(work),
					    struct nvt_ts_data, retry_work);

	mutex_lock(&ts->state_lock);
	if (!ts->stopping && ts->display_on)
		nvt_enable_or_retry(ts);
	mutex_unlock(&ts->state_lock);
}

static int nvt_disp_notifier_callback(struct notifier_block *nb,
				      unsigned long event, void *data)
{
	struct nvt_ts_data *ts = container_of(nb, struct nvt_ts_data,
					    disp_notifier);

	if (event != DISPC_POWER_ON && event != DISPC_POWER_OFF)
		return NOTIFY_DONE;
	mutex_lock(&ts->state_lock);
	if (ts->stopping)
		goto out;
	ts->display_on = event == DISPC_POWER_ON;
	if (ts->display_on) {
		ts->retry_count = 0;
		nvt_enable_or_retry(ts);
	} else {
		/* The worker uses state_lock; do not wait while holding it. */
		cancel_delayed_work(&ts->retry_work);
		nvt_suspend(ts);
	}
out:
	mutex_unlock(&ts->state_lock);
	/* An I2C failure must not stop other display notifier clients. */
	return NOTIFY_OK;
}

static ssize_t nvt_status_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct nvt_ts_data *ts = dev_get_drvdata(dev);
	ssize_t len;

	mutex_lock(&ts->state_lock);
	len = scnprintf(buf, PAGE_SIZE,
		"ready=%u awake=%u display_on=%u irq_enabled=%u "
		"attempts=%u last_error=%d fw=%02x pid=%04x "
		"x_max=%u y_max=%u cascade=%u trim=%*ph\n",
		ts->ready, ts->awake, ts->display_on, ts->irq_enabled,
		ts->attempts, ts->last_error, ts->info.version, ts->info.pid,
		ts->info.x_max, ts->info.y_max, ts->cascade, 6, ts->chip_id);
	mutex_unlock(&ts->state_lock);
	return len;
}
static DEVICE_ATTR_RO(nvt_status);

static int nvt_ts_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct nvt_ts_data *ts;
	int ret;

	/* Alternate FTS panels share GPIOs and must keep their driver. */
	if (lcd_str && *lcd_str && strcmp(lcd_str, NVT_PANEL))
		return -ENODEV;
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -ENODEV;
	ts = devm_kzalloc(&client->dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;
	ts->client = client;
	mutex_init(&ts->state_lock);
	mutex_init(&ts->io_lock);
	INIT_DELAYED_WORK(&ts->retry_work, nvt_retry_work);
	ts->reset_gpio = of_get_named_gpio(client->dev.of_node,
					  "novatek,reset-gpio", 0);
	if (!gpio_is_valid(ts->reset_gpio))
		return ts->reset_gpio < 0 ? ts->reset_gpio : -EINVAL;
	ts->irq_gpio = of_get_named_gpio(client->dev.of_node,
					"novatek,irq-gpio", 0);
	if (!gpio_is_valid(ts->irq_gpio))
		return ts->irq_gpio < 0 ? ts->irq_gpio : -EINVAL;
	/* Keep touch reset high; display owns the panel reset sequence. */
	ret = devm_gpio_request_one(&client->dev, ts->reset_gpio,
				    GPIOF_OUT_INIT_HIGH, "NVT-tp-rst");
	if (ret)
		return ret;
	ret = devm_gpio_request_one(&client->dev, ts->irq_gpio,
				    GPIOF_IN, "NVT-int");
	if (ret)
		return ret;
	ts->irq = client->irq > 0 ? client->irq : gpio_to_irq(ts->irq_gpio);
	if (ts->irq <= 0)
		return ts->irq ? ts->irq : -EINVAL;
	client->irq = ts->irq;
	snprintf(ts->phys, sizeof(ts->phys), "%s/input0",
		 dev_name(&client->dev));
	i2c_set_clientdata(client, ts);
	ret = device_create_file(&client->dev, &dev_attr_nvt_status);
	if (ret)
		return ret;
	ts->disp_notifier.notifier_call = nvt_disp_notifier_callback;
	/* Register before the initial attempt to catch later power-on. */
	mutex_lock(&ts->state_lock);
	ts->display_on = true;
	mutex_unlock(&ts->state_lock);
	ret = disp_notifier_register(&ts->disp_notifier);
	if (ret) {
		device_remove_file(&client->dev, &dev_attr_nvt_status);
		return ret;
	}
	ts->notifier_registered = true;
	mutex_lock(&ts->state_lock);
	/* Cover a display power-on that happened before registration. */
	if (ts->display_on)
		nvt_enable_or_retry(ts);
	mutex_unlock(&ts->state_lock);
	/* Keep the client bound after a transient panel/bus power failure. */
	return 0;
}

static void nvt_quiesce(struct nvt_ts_data *ts)
{
	if (ts->notifier_registered) {
		disp_notifier_unregister(&ts->disp_notifier);
		ts->notifier_registered = false;
	}
	mutex_lock(&ts->state_lock);
	ts->stopping = true;
	ts->display_on = false;
	mutex_unlock(&ts->state_lock);
	cancel_delayed_work_sync(&ts->retry_work);
	mutex_lock(&ts->state_lock);
	nvt_suspend(ts);
	mutex_unlock(&ts->state_lock);
}

static int nvt_ts_remove(struct i2c_client *client)
{
	struct nvt_ts_data *ts = i2c_get_clientdata(client);

	nvt_quiesce(ts);
	device_remove_file(&client->dev, &dev_attr_nvt_status);
	if (ts->irq_requested)
		free_irq(ts->irq, ts);
	if (ts->input)
		input_unregister_device(ts->input);
	return 0;
}

static void nvt_ts_shutdown(struct i2c_client *client)
{
	nvt_quiesce(i2c_get_clientdata(client));
}

static const struct i2c_device_id nvt_ts_id[] = {
	{ NVT_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, nvt_ts_id);

static const struct of_device_id nvt_match_table[] = {
	{ .compatible = "novatek,nt36532" },
	{ .compatible = "novatek,NVT-ts" },
	{ }
};
MODULE_DEVICE_TABLE(of, nvt_match_table);

static struct i2c_driver nvt_i2c_driver = {
	.probe = nvt_ts_probe,
	.remove = nvt_ts_remove,
	.shutdown = nvt_ts_shutdown,
	.id_table = nvt_ts_id,
	.driver = {
		.name = NVT_NAME,
		.of_match_table = nvt_match_table,
	},
};

static int __init nvt_driver_init(void)
{
	return i2c_add_driver(&nvt_i2c_driver);
}

static void __exit nvt_driver_exit(void)
{
	i2c_del_driver(&nvt_i2c_driver);
}

late_initcall(nvt_driver_init);
module_exit(nvt_driver_exit);
MODULE_DESCRIPTION("EEBBK A7 NT36532 I2C touchscreen");
MODULE_LICENSE("GPL");
