/*
 *  Copyright (c) 2013 Andrew Duggan <aduggan@synaptics.com>
 *  Copyright (c) 2013 Synaptics Incorporated
 *  Copyright (c) 2014 Benjamin Tissoires <benjamin.tissoires@gmail.com>
 *  Copyright (c) 2014 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <linux/kernel.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/bitops.h>
#include <linux/sched.h>
#include "hid-ids.h"

#include "compat.h"

#define RMI_MOUSE_REPORT_ID		0x01 /* Mouse emulation Report */
#define RMI_WRITE_REPORT_ID		0x09 /* Output Report */
#define RMI_READ_ADDR_REPORT_ID		0x0a /* Output Report */
#define RMI_READ_DATA_REPORT_ID		0x0b /* Input Report */
#define RMI_ATTN_REPORT_ID		0x0c /* Input Report */
#define RMI_SET_RMI_MODE_REPORT_ID	0x0f /* Feature Report */

/* flags */
#define RMI_READ_REQUEST_PENDING	BIT(0)
#define RMI_READ_DATA_PENDING		BIT(1)
#define RMI_STARTED			BIT(2)

enum rmi_mode_type {
	RMI_MODE_OFF 			= 0,
	RMI_MODE_ATTN_REPORTS		= 1,
	RMI_MODE_NO_PACKED_ATTN_REPORTS	= 2,
};

struct rmi_function {
	unsigned page;			/* page of the function */
	u16 query_base_addr;		/* base address for queries */
	u16 command_base_addr;		/* base address for commands */
	u16 control_base_addr;		/* base address for controls */
	u16 data_base_addr;		/* base address for datas */
	unsigned int interrupt_base;	/* cross-function interrupt number
					 * (uniq in the device)*/
	unsigned int interrupt_count;	/* number of interrupts */
	unsigned int report_size;	/* size of a report */
	unsigned long irq_mask;		/* mask of the interrupts
					 * (to be applied against ATTN IRQ) */
};

/**
 * struct rmi_data - stores information for hid communication
 *
 * @page_mutex: Locks current page to avoid changing pages in unexpected ways.
 * @page: Keeps track of the current virtual page
 *
 * @wait: Used for waiting for read data
 *
 * @writeReport: output buffer when writing RMI registers
 * @readReport: input buffer when reading RMI registers
 *
 * @input_report_size: size of an input report (advertised by HID)
 * @output_report_size: size of an output report (advertised by HID)
 *
 * @flags: flags for the current device (started, reading, etc...)
 *
 * @f11: placeholder of internal RMI function F11 description
 * @f30: placeholder of internal RMI function F30 description
 *
 * @max_fingers: maximum finger count reported by the device
 * @max_x: maximum x value reported by the device
 * @max_y: maximum y value reported by the device
 *
 * @gpio_led_count: count of GPIOs + LEDs reported by F30
 * @button_count: actual physical buttons count
 * @button_mask: button mask used to decode GPIO ATTN reports
 * @button_state_mask: pull state of the buttons
 *
 * @input: pointer to the kernel input device
 *
 * @reset_work: worker which will be called in case of a mouse report
 * @hdev: pointer to the struct hid_device
 */
struct rmi_data {
	struct mutex page_mutex;
	int page;

	wait_queue_head_t wait;

	u8 *writeReport;
	u8 *readReport;

	int input_report_size;
	int output_report_size;

	unsigned long flags;

	struct rmi_function f11;
	struct rmi_function f30;

	unsigned int max_fingers;
	unsigned int max_x;
	unsigned int max_y;
	unsigned int x_size_mm;
	unsigned int y_size_mm;

	unsigned int gpio_led_count;
	unsigned int button_count;
	unsigned long button_mask;
	unsigned long button_state_mask;

	struct input_dev *input;

	struct work_struct reset_work;
	struct hid_device *hdev;
};

#define RMI_PAGE(addr) (((addr) >> 8) & 0xff)

static int rmi_write_report(struct hid_device *hdev, u8 *report, int len);

/**
 * rmi_set_page - Set RMI page
 * @hdev: The pointer to the hid_device struct
 * @page: The new page address.
 *
 * RMI devices have 16-bit addressing, but some of the physical
 * implementations (like SMBus) only have 8-bit addressing. So RMI implements
 * a page address at 0xff of every page so we can reliable page addresses
 * every 256 registers.
 *
 * The page_mutex lock must be held when this function is entered.
 *
 * Returns zero on success, non-zero on failure.
 */
static int rmi_set_page(struct hid_device *hdev, u8 page)
{
	struct rmi_data *data = hid_get_drvdata(hdev);
	int retval;

	data->writeReport[0] = RMI_WRITE_REPORT_ID;
	data->writeReport[1] = 1;
	data->writeReport[2] = 0xFF;
	data->writeReport[4] = page;

	retval = rmi_write_report(hdev, data->writeReport,
			data->output_report_size);
	if (retval != data->output_report_size) {
		dev_err(&hdev->dev,
			"%s: set page failed: %d.", __func__, retval);
		return retval;
	}

	data->page = page;
	return 0;
}

static int rmi_set_mode(struct hid_device *hdev, u8 mode)
{
	int ret;
	u8 txbuf[2] = {RMI_SET_RMI_MODE_REPORT_ID, mode};

	ret = hid_hw_raw_request(hdev, RMI_SET_RMI_MODE_REPORT_ID, txbuf,
			sizeof(txbuf), HID_FEATURE_REPORT, HID_REQ_SET_REPORT);
	if (ret < 0) {
		dev_err(&hdev->dev, "unable to set rmi mode to %d (%d)\n", mode,
			ret);
		return ret;
	}

	return 0;
}

static int rmi_write_report(struct hid_device *hdev, u8 * report, int len)
{
	int ret;

	ret = hid_hw_output_report(hdev, (void *)report, len);
	if (ret < 0) {
		dev_err(&hdev->dev, "failed to write hid report (%d)\n", ret);
		return ret;
	}

	return ret;
}

static int rmi_read_block(struct hid_device *hdev, u16 addr, void *buf,
		const int len)
{
	struct rmi_data *data = hid_get_drvdata(hdev);
	int ret;
	int bytes_read;
	int bytes_needed;
	int retries;
	int read_input_count;

	mutex_lock(&data->page_mutex);

	if (RMI_PAGE(addr) != data->page) {
		ret = rmi_set_page(hdev, RMI_PAGE(addr));
		if (ret < 0)
			goto exit;
	}

	for (retries = 5; retries > 0; retries--) {
		data->writeReport[0] = RMI_READ_ADDR_REPORT_ID;
		data->writeReport[1] = 0; /* old 1 byte read count */
		data->writeReport[2] = addr & 0xFF;
		data->writeReport[3] = (addr >> 8) & 0xFF;
		data->writeReport[4] = len  & 0xFF;
		data->writeReport[5] = (len >> 8) & 0xFF;

		set_bit(RMI_READ_REQUEST_PENDING, &data->flags);

		ret = rmi_write_report(hdev, data->writeReport,
						data->output_report_size);
		if (ret != data->output_report_size) {
			clear_bit(RMI_READ_REQUEST_PENDING, &data->flags);
			dev_err(&hdev->dev,
				"failed to write request output report (%d)\n",
				ret);
			goto exit;
		}

		bytes_read = 0;
		bytes_needed = len;
		while (bytes_read < len) {
			if (!wait_event_timeout(data->wait, 
				test_bit(RMI_READ_DATA_PENDING, &data->flags),
					msecs_to_jiffies(1000))) {
				hid_warn(hdev, "%s: timeout elapsed\n",
					 __func__);
				ret = -EAGAIN;
				break;
			}

			read_input_count = data->readReport[1];
			memcpy(buf + bytes_read, &data->readReport[2],
				read_input_count < bytes_needed ?
					read_input_count : bytes_needed);

			bytes_read += read_input_count;
			bytes_needed -= read_input_count;
			clear_bit(RMI_READ_DATA_PENDING, &data->flags);
		}

		if (ret >= 0) {
			ret = 0;
			break;
		}
	}

exit:
	clear_bit(RMI_READ_REQUEST_PENDING, &data->flags);
	mutex_unlock(&data->page_mutex);
	return ret;
}

static inline int rmi_read(struct hid_device *hdev, u16 addr, void *buf)
{
	return rmi_read_block(hdev, addr, buf, 1);
}

static void rmi_f11_process_touch(struct rmi_data *hdata, int slot,
		u8 finger_state, u8 *touch_data)
{
	int x, y, wx, wy;
	int wide, major, minor;
	int z;

	input_mt_slot(hdata->input, slot);
	input_mt_report_slot_state(hdata->input, MT_TOOL_FINGER,
			finger_state == 0x01);
	if (finger_state == 0x01) {
		x = (touch_data[0] << 4) | (touch_data[2] & 0x07);
		y = (touch_data[1] << 4) | (touch_data[2] >> 4);
		wx = touch_data[3] & 0x07;
		wy = touch_data[3] >> 4;
		wide = (wx > wy);
		major = max(wx, wy);
		minor = min(wx, wy);
		z = touch_data[4];

		/* y is inverted */
		y = hdata->max_y - y;

		input_event(hdata->input, EV_ABS, ABS_MT_POSITION_X, x);
		input_event(hdata->input, EV_ABS, ABS_MT_POSITION_Y, y);
		input_event(hdata->input, EV_ABS, ABS_MT_ORIENTATION, wide);
		input_event(hdata->input, EV_ABS, ABS_MT_PRESSURE, z);
		input_event(hdata->input, EV_ABS, ABS_MT_TOUCH_MAJOR, major);
		input_event(hdata->input, EV_ABS, ABS_MT_TOUCH_MINOR, minor);
	}
}

static void rmi_reset_work(struct work_struct *work)
{
	struct rmi_data *hdata = container_of(work, struct rmi_data,
						reset_work);

	/* switch the device to RMI if we receive a generic mouse report */
	rmi_set_mode(hdata->hdev, RMI_MODE_ATTN_REPORTS);
}

static inline int rmi_schedule_reset(struct hid_device *hdev)
{
	struct rmi_data *hdata = hid_get_drvdata(hdev);
	return schedule_work(&hdata->reset_work);
}

static int rmi_f11_input_event(struct hid_device *hdev, u8 irq, u8 *data,
		int size)
{
	struct rmi_data *hdata = hid_get_drvdata(hdev);
	int offset;
	int i;

	if (size < hdata->f11.report_size)
		return 0;

	if (!(irq & hdata->f11.irq_mask))
		return 0;

	offset = (hdata->max_fingers >> 2) + 1;
	for (i = 0; i < hdata->max_fingers; i++) {
		int fs_byte_position = i >> 2;
		int fs_bit_position = (i & 0x3) << 1;
		int finger_state = (data[fs_byte_position] >> fs_bit_position) &
					0x03;

		rmi_f11_process_touch(hdata, i, finger_state,
				&data[offset + 5 * i]);
	}
	input_mt_sync_frame(hdata->input);
	input_sync(hdata->input);
	return hdata->f11.report_size;
}

static int rmi_f30_input_event(struct hid_device *hdev, u8 irq, u8 *data,
		int size)
{
	struct rmi_data *hdata = hid_get_drvdata(hdev);
	int i;
	int button = 0;
	bool value;

	if (!(irq & hdata->f30.irq_mask))
		return 0;

	for (i = 0; i < hdata->gpio_led_count; i++) {
		if (test_bit(i, &hdata->button_mask)){
			value = (data[i / 8] >> (i & 0x07)) & BIT(0);
			if (test_bit(i, &hdata->button_state_mask))
				value = !value;
			input_event(hdata->input, EV_KEY, BTN_LEFT + button++,
					value);
		}
	}
	return hdata->f30.report_size;
}

static int rmi_input_event(struct hid_device *hdev, u8 *data, int size)
{
	struct rmi_data *hdata = hid_get_drvdata(hdev);
	unsigned long irq_mask = 0;
	unsigned index = 2;

	if (!(test_bit(RMI_STARTED, &hdata->flags)))
		return 0;

	irq_mask |= hdata->f11.irq_mask;
	irq_mask |= hdata->f30.irq_mask;

	if (data[1] & ~irq_mask)
		hid_warn(hdev, "unknown intr source:%02lx %s:%d\n",
			data[1] & ~irq_mask, __FILE__, __LINE__);

	if (hdata->f11.interrupt_base < hdata->f30.interrupt_base) {
		index += rmi_f11_input_event(hdev, data[1], &data[index],
				size - index);
		index += rmi_f30_input_event(hdev, data[1], &data[index],
				size - index);
	} else {
		index += rmi_f30_input_event(hdev, data[1], &data[index],
				size - index);
		index += rmi_f11_input_event(hdev, data[1], &data[index],
				size - index);
	}

	return 1;
}

static int rmi_read_data_event(struct hid_device *hdev, u8 *data, int size)
{
	struct rmi_data *hdata = hid_get_drvdata(hdev);

	if (!test_bit(RMI_READ_REQUEST_PENDING, &hdata->flags)) {
		hid_err(hdev, "no read request pending\n");
		return 0;
	}

	memcpy(hdata->readReport, data, size < hdata->input_report_size ?
			size : hdata->input_report_size);
	set_bit(RMI_READ_DATA_PENDING, &hdata->flags);
	wake_up(&hdata->wait);

	return 1;
}

static int rmi_raw_event(struct hid_device *hdev,
		struct hid_report *report, u8 *data, int size)
{
	switch (data[0]) {
	case RMI_READ_DATA_REPORT_ID:
		return rmi_read_data_event(hdev, data, size);
	case RMI_ATTN_REPORT_ID:
		return rmi_input_event(hdev, data, size);
	case RMI_MOUSE_REPORT_ID:
		if (!data[1])
			rmi_schedule_reset(hdev);
		break;
	}

	return 0;
}

static int rmi_post_reset(struct hid_device *hdev)
{
	return rmi_set_mode(hdev, RMI_MODE_ATTN_REPORTS);
}

static int rmi_post_resume(struct hid_device *hdev)
{
	return rmi_set_mode(hdev, RMI_MODE_ATTN_REPORTS);
}

#define RMI4_MAX_PAGE 0xff
#define RMI4_PAGE_SIZE 0x0100

#define PDT_START_SCAN_LOCATION 0x00e9
#define PDT_END_SCAN_LOCATION	0x0005
#define RMI4_END_OF_PDT(id) ((id) == 0x00 || (id) == 0xff)

struct pdt_entry {
	u8 query_base_addr:8;
	u8 command_base_addr:8;
	u8 control_base_addr:8;
	u8 data_base_addr:8;
	u8 interrupt_source_count:3;
	u8 bits3and4:2;
	u8 function_version:2;
	u8 bit7:1;
	u8 function_number:8;
} __attribute__((__packed__));

static inline unsigned long rmi_gen_mask(unsigned irq_base, unsigned irq_count)
{
	return GENMASK(irq_count + irq_base - 1, irq_base);
}

static void rmi_register_function(struct rmi_data *data,
	struct pdt_entry *pdt_entry, int page, unsigned interrupt_count)
{
	struct rmi_function *f = NULL;
	u16 page_base = page << 8;

	switch (pdt_entry->function_number) {
	case 0x11:
		f = &data->f11;
		break;
	case 0x30:
		f = &data->f30;
		break;
	}

	if (f) {
		f->page = page;
		f->query_base_addr = page_base | pdt_entry->query_base_addr;
		f->command_base_addr = page_base | pdt_entry->command_base_addr;
		f->control_base_addr = page_base | pdt_entry->control_base_addr;
		f->data_base_addr = page_base | pdt_entry->data_base_addr;
		f->interrupt_base = interrupt_count;
		f->interrupt_count = pdt_entry->interrupt_source_count;
		f->irq_mask = rmi_gen_mask(f->interrupt_base,
						f->interrupt_count);
	}
}

static int rmi_scan_pdt(struct hid_device *hdev)
{
	struct rmi_data *data = hid_get_drvdata(hdev);
	struct pdt_entry entry;
	int page;
	bool page_has_function;
	int i;
	int retval;
	int interrupt = 0;
	u16 page_start, pdt_start , pdt_end;

	hid_info(hdev, "Scanning PDT...\n");

	for (page = 0; (page <= RMI4_MAX_PAGE); page++) {
		page_start = RMI4_PAGE_SIZE * page;
		pdt_start = page_start + PDT_START_SCAN_LOCATION;
		pdt_end = page_start + PDT_END_SCAN_LOCATION;

		page_has_function = false;
		for (i = pdt_start; i >= pdt_end; i -= sizeof(entry)) {
			retval = rmi_read_block(hdev, i, &entry, sizeof(entry));
			if (retval) {
				hid_err(hdev,
					"Read of PDT entry at %#06x failed.\n",
					i);
				goto error_exit;
			}

			if (RMI4_END_OF_PDT(entry.function_number))
				break;

			page_has_function = true;

			hid_info(hdev, "Found F%02X on page %#04x\n",
					entry.function_number, page);

			rmi_register_function(data, &entry, page, interrupt);
			interrupt += entry.interrupt_source_count;
		}

		if (!page_has_function)
			break;
	}

	hid_info(hdev, "%s: Done with PDT scan.\n", __func__);
	retval = 0;

error_exit:
	return retval;
}

static int rmi_populate_f11(struct hid_device *hdev)
{
	struct rmi_data *data = hid_get_drvdata(hdev);
	u8 buf[20];
	int ret;
	bool has_query9;
	bool has_query10;
	bool has_query11;
	bool has_query12;
	bool has_physical_props;
	unsigned x_size, y_size;
	u16 query12_offset;

	if (!data->f11.query_base_addr) {
		hid_err(hdev, "No 2D sensor found, giving up.\n");
		return -ENODEV;
	}

	/* query 0 contains some useful information */
	ret = rmi_read(hdev, data->f11.query_base_addr, buf);
	if (ret) {
		hid_err(hdev, "can not get query 0: %d.\n", ret);
		return ret;
	}
	has_query9 = !!(buf[0] & BIT(3));
	has_query11 = !!(buf[0] & BIT(4));
	has_query12 = !!(buf[0] & BIT(5));

	/* query 1 to get the max number of fingers */
	ret = rmi_read(hdev, data->f11.query_base_addr + 1, buf);
	if (ret) {
		hid_err(hdev, "can not get NumberOfFingers: %d.\n", ret);
		return ret;
	}
	data->max_fingers = (buf[0] & 0x07) + 1;
	if (data->max_fingers > 5)
		data->max_fingers = 10;

	data->f11.report_size = data->max_fingers * 5 +
				DIV_ROUND_UP(data->max_fingers, 4);

	if (!(buf[0] & BIT(4))) {
		hid_err(hdev, "No absolute events, giving up.\n");
		return -ENODEV;
	}

	/* query 8 to find out if query 10 exists */
	ret = rmi_read(hdev, data->f11.query_base_addr + 8, buf);
	if (ret) {
		hid_err(hdev, "can not read gesture information: %d.\n", ret);
		return ret;
	}
	has_query10 = !!(buf[0] & BIT(2));

	/*
	 * At least 8 queries are guaranteed to be present in F11
	 * +1 for query12.
	 */
	query12_offset = 9;

	if (has_query9)
		++query12_offset;

	if (has_query10)
		++query12_offset;

	if (has_query11)
		++query12_offset;

	/* query 12 to know if the physical properties are reported */
	if (has_query12) {
		ret = rmi_read(hdev, data->f11.query_base_addr
				+ query12_offset, buf);
		if (ret) {
			hid_err(hdev, "can not get query 12: %d.\n", ret);
			return ret;
		}
		has_physical_props = !!(buf[0] & BIT(5));

		if (has_physical_props) {
			ret = rmi_read_block(hdev,
					data->f11.query_base_addr
						+ query12_offset + 1, buf, 4);
			if (ret) {
				hid_err(hdev, "can not read query 15-18: %d.\n",
					ret);
				return ret;
			}

			x_size = buf[0] | (buf[1] << 8);
			y_size = buf[2] | (buf[3] << 8);

			data->x_size_mm = DIV_ROUND_CLOSEST(x_size, 10);
			data->y_size_mm = DIV_ROUND_CLOSEST(y_size, 10);

			hid_info(hdev, "%s: size in mm: %d x %d\n",
				 __func__, data->x_size_mm, data->y_size_mm);
		}
	}

	/* retrieve the ctrl registers */
	ret = rmi_read_block(hdev, data->f11.control_base_addr, buf, 20);
	if (ret) {
		hid_err(hdev, "can not read ctrl block of size 20: %d.\n", ret);
		return ret;
	}

	data->max_x = buf[6] | (buf[7] << 8);
	data->max_y = buf[8] | (buf[9] << 8);

	return 0;
}

static int rmi_populate_f30(struct hid_device *hdev)
{
	struct rmi_data *data = hid_get_drvdata(hdev);
	u8 buf[20];
	int ret;
	bool has_gpio, has_led;
	unsigned bytes_per_ctrl;
	u8 ctrl2_addr;
	int ctrl2_3_length;
	int i;

	/* function F30 is for physical buttons */
	if (!data->f30.query_base_addr) {
		hid_err(hdev, "No GPIO/LEDs found, giving up.\n");
		return -ENODEV;
	}

	ret = rmi_read_block(hdev, data->f30.query_base_addr, buf, 2);
	if (ret) {
		hid_err(hdev, "can not get F30 query registers: %d.\n", ret);
		return ret;
	}

	has_gpio = !!(buf[0] & BIT(3));
	has_led = !!(buf[0] & BIT(2));
	data->gpio_led_count = buf[1] & 0x1f;

	/* retrieve ctrl 2 & 3 registers */
	bytes_per_ctrl = (data->gpio_led_count + 7) / 8;
	/* Ctrl0 is present only if both has_gpio and has_led are set*/
	ctrl2_addr = (has_gpio && has_led) ? bytes_per_ctrl : 0;
	/* Ctrl1 is always be present */
	ctrl2_addr += bytes_per_ctrl;
	ctrl2_3_length = 2 * bytes_per_ctrl;

	data->f30.report_size = bytes_per_ctrl;

	ret = rmi_read_block(hdev, data->f30.control_base_addr + ctrl2_addr,
				buf, ctrl2_3_length);
	if (ret) {
		hid_err(hdev, "can not read ctrl 2&3 block of size %d: %d.\n",
			ctrl2_3_length, ret);
		return ret;
	}

	for (i = 0; i < data->gpio_led_count; i++) {
		int byte_position = i >> 3;
		int bit_position = i & 0x07;
		u8 dir_byte = buf[byte_position];
		u8 data_byte = buf[byte_position + bytes_per_ctrl];
		bool dir = (dir_byte >> bit_position) & BIT(0);
		bool dat = (data_byte >> bit_position) & BIT(0);

		if (dir == 0) {
			/* input mode */
			if (dat) {
				/* actual buttons have pull up resistor */
				data->button_count++;
				set_bit(i, &data->button_mask);
				set_bit(i, &data->button_state_mask);
			}
		}

	}

	return 0;
}

static int rmi_populate(struct hid_device *hdev)
{
	int ret;

	ret = rmi_scan_pdt(hdev);
	if (ret) {
		hid_err(hdev, "PDT scan failed with code %d.\n", ret);
		return ret;
	}

	ret = rmi_populate_f11(hdev);
	if (ret) {
		hid_err(hdev, "Error while initializing F11 (%d).\n", ret);
		return ret;
	}

	ret = rmi_populate_f30(hdev);
	if (ret)
		hid_warn(hdev, "Error while initializing F30 (%d).\n", ret);

	return 0;
}

static void rmi_input_configured(struct hid_device *hdev, struct hid_input *hi)
{
	struct rmi_data *data = hid_get_drvdata(hdev);
	struct input_dev *input = hi->input;
	int ret;
	int res_x, res_y, i;

	data->input = input;

	hid_dbg(hdev, "Opening low level driver\n");
	ret = hid_hw_open(hdev);
	if (ret)
		return;

	/* Allow incoming hid reports */
	hid_device_io_start(hdev);

	ret = rmi_set_mode(hdev, RMI_MODE_ATTN_REPORTS);
	if (ret < 0) {
		dev_err(&hdev->dev, "failed to set rmi mode\n");
		goto exit;
	}

	ret = rmi_set_page(hdev, 0);
	if (ret < 0) {
		dev_err(&hdev->dev, "failed to set page select to 0.\n");
		goto exit;
	}

	ret = rmi_populate(hdev);
	if (ret)
		goto exit;

	__set_bit(EV_ABS, input->evbit);
	input_set_abs_params(input, ABS_MT_POSITION_X, 1, data->max_x, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 1, data->max_y, 0, 0);

	if (data->x_size_mm && data->x_size_mm) {
		res_x = (data->max_x - 1) / data->x_size_mm;
		res_y = (data->max_y - 1) / data->x_size_mm;

		input_abs_set_res(input, ABS_MT_POSITION_X, res_x);
		input_abs_set_res(input, ABS_MT_POSITION_Y, res_y);
	}

	input_set_abs_params(input, ABS_MT_ORIENTATION, 0, 1, 0, 0);
	input_set_abs_params(input, ABS_MT_PRESSURE, 0, 0xff, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MAJOR, 0, 0x0f, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MINOR, 0, 0x0f, 0, 0);

	input_mt_init_slots(input, data->max_fingers, INPUT_MT_POINTER);

	if (data->button_count) {
		__set_bit(EV_KEY, input->evbit);
		for (i = 0; i < data->button_count; i++)
			__set_bit(BTN_LEFT + i, input->keybit);

		if (data->button_count == 1)
			__set_bit(INPUT_PROP_BUTTONPAD, input->propbit);
	}

	set_bit(RMI_STARTED, &data->flags);

exit:
	hid_device_io_stop(hdev);
	hid_hw_close(hdev);
}

static int rmi_input_mapping(struct hid_device *hdev,
		struct hid_input *hi, struct hid_field *field,
		struct hid_usage *usage, unsigned long **bit, int *max)
{
	/* we want to make HID ignore the advertised HID collection */
	return -1;
}

static int rmi_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct rmi_data *data = NULL;
	int ret;
	size_t alloc_size;

	data = devm_kzalloc(&hdev->dev, sizeof(struct rmi_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	INIT_WORK(&data->reset_work, rmi_reset_work);
	data->hdev = hdev;

	hid_set_drvdata(hdev, data);

	hdev->quirks |= HID_QUIRK_NO_INIT_REPORTS;

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "parse failed\n");
		return ret;
	}

	data->input_report_size = (hdev->report_enum[HID_INPUT_REPORT]
		.report_id_hash[RMI_ATTN_REPORT_ID]->size >> 3)
		+ 1 /* report id */;
	data->output_report_size = (hdev->report_enum[HID_OUTPUT_REPORT]
		.report_id_hash[RMI_WRITE_REPORT_ID]->size >> 3)
		+ 1 /* report id */;

	alloc_size = data->output_report_size + data->input_report_size;

	data->writeReport = devm_kzalloc(&hdev->dev, alloc_size, GFP_KERNEL);
	if (!data->writeReport) {
		ret = -ENOMEM;
		return ret;
	}

	data->readReport = data->writeReport + data->output_report_size;

	init_waitqueue_head(&data->wait);

	mutex_init(&data->page_mutex);

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret) {
		hid_err(hdev, "hw start failed\n");
		return ret;
	}

	if (!test_bit(RMI_STARTED, &data->flags)) {
		hid_hw_stop(hdev);
		return -EIO;
	}

	return 0;
}

static void rmi_remove(struct hid_device *hdev)
{
	struct rmi_data *hdata = hid_get_drvdata(hdev);

	clear_bit(RMI_STARTED, &hdata->flags);

	hid_hw_stop(hdev);
}

static const struct hid_device_id rmi_id[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_SYNAPTICS, HID_ANY_ID) },
	{ HID_USB_DEVICE(0x1532, 0x011D) },
	{ }
};
MODULE_DEVICE_TABLE(hid, rmi_id);

static struct hid_driver rmi_driver = {
	.name = "hid-rmi",
	.id_table		= rmi_id,
	.probe			= rmi_probe,
	.remove			= rmi_remove,
	.raw_event		= rmi_raw_event,
	.input_mapping		= rmi_input_mapping,
	.input_configured	= rmi_input_configured,
#ifdef CONFIG_PM
	.resume			= rmi_post_resume,
	.reset_resume		= rmi_post_reset,
#endif
};

module_hid_driver(rmi_driver);

MODULE_AUTHOR("Andrew Duggan <aduggan@synaptics.com>");
MODULE_DESCRIPTION("RMI HID driver");
MODULE_LICENSE("GPL");
