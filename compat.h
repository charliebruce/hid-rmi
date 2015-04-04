/*
 *  Copyright (c) 2014 Benjamin Tissoires <benjamin.tissoires@gmail.com>
 *  Copyright (c) 2014 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <linux/version.h>

#ifndef HID_I2C_DEVICE
#define HID_I2C_DEVICE(ven, prod)				\
	.bus = BUS_I2C, .vendor = (ven), .product = (prod)
#endif

#ifndef GENMASK
#define GENMASK(h, l)           (((U32_C(1) << ((h) - (l) + 1)) - 1) << (l))
#endif

/* It seems that fairly recently, Ubuntu added these functions to their headers in include/linux/hid.h 
or something else happened? */
#if 0

static inline int hid_hw_raw_request(struct hid_device *hdev,
				  unsigned char reportnum, __u8 *buf,
				  size_t len, unsigned char rtype, int reqtype)
{
	if (len < 1 || len > HID_MAX_BUFFER_SIZE || !buf)
		return -EINVAL;

	switch (reqtype) {
	case HID_REQ_SET_REPORT:
		if (reportnum != buf[0])
			return -EINVAL;
		return hdev->hid_output_raw_report(hdev, buf, len, rtype);
	case HID_REQ_GET_REPORT:
		return hdev->hid_get_raw_report(hdev, reportnum, buf, len, rtype);
	}

	return -ENOSYS;
}

static inline int hid_hw_output_report(struct hid_device *hdev, __u8 *buf,
					size_t len)
{
	if (len < 1 || len > HID_MAX_BUFFER_SIZE || !buf)
		return -EINVAL;

	return hdev->hid_output_raw_report(hdev, buf, len, HID_OUTPUT_REPORT);
}

#endif
