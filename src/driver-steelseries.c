/*
 * Copyright © 2017 Thomas Hindoe Paaboel Andersen.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "config.h"
#include <assert.h>
#include <errno.h>
#include <libevdev/libevdev.h>
#include <linux/input.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "libratbag-private.h"
#include "libratbag-hidraw.h"
#include "libratbag-data.h"
#include "hidpp-generic.h"

#define STEELSERIES_NUM_PROFILES	1
#define STEELSERIES_NUM_DPI		2
#define STEELSERIES_INPUT_ENDPOINT	0
#define STEELSERIES_INPUT_HIDRAW	1

/* not sure these two are used for */
#define STEELSERIES_REPORT_ID_1			0x01
#define STEELSERIES_REPORT_ID_2			0x02

#define STEELSERIES_REPORT_SIZE_SHORT		32
#define STEELSERIES_REPORT_SIZE			64
#define STEELSERIES_REPORT_LONG_SIZE		262

#define STEELSERIES_ID_DPI_SHORT		0x03
#define STEELSERIES_ID_REPORT_RATE_SHORT	0x04
#define STEELSERIES_ID_LED_INTENSITY_SHORT	0x05
#define STEELSERIES_ID_LED_EFFECT_SHORT		0x07
#define STEELSERIES_ID_LED_COLOR_SHORT		0x08
#define STEELSERIES_ID_SAVE_SHORT		0x09

#define STEELSERIES_ID_BUTTONS			0x31
#define STEELSERIES_ID_DPI			0x53
#define STEELSERIES_ID_REPORT_RATE		0x54
#define STEELSERIES_ID_LED			0x5b
#define STEELSERIES_ID_SAVE			0x59
#define STEELSERIES_ID_FIRMWARE			0x90
#define STEELSERIES_ID_SETTTINGS		0x92

#define STEELSERIES_BUTTON_OFF			0x00
#define STEELSERIES_BUTTON_RES_CYCLE		0x30
#define STEELSERIES_BUTTON_WHEEL_UP		0x31
#define STEELSERIES_BUTTON_WHEEL_DOWN		0x32
#define STEELSERIES_BUTTON_KEY			0x10
#define STEELSERIES_BUTTON_KBD			0x51
#define STEELSERIES_BUTTON_CONSUMER		0x61

struct steelseries_data {
	int firmware_major;
	int firmware_minor;
};

struct steelseries_point {
	struct list link;

	struct ratbag_color color;	/* point color */
	uint8_t pos;			/* relative position in the cycle */
};

struct steelseries_led_cycle {
	uint8_t led_id;			/* led id */
	uint16_t duration;		/* cycle duration */
	bool repeat;			/* if the cycle restarts automatically */
	uint8_t trigger_buttons;	/* trigger button combination */
	struct list points;		/* colors in the cycle */
};

static int
steelseries_test_hidraw(struct ratbag_device *device)
{
	int device_version = ratbag_device_data_steelseries_get_device_version(device->data);

	if (device_version > 1)
		return ratbag_hidraw_has_report(device, STEELSERIES_REPORT_ID_1);
	else
		return true;
}

static void
button_defaults_for_layout(struct ratbag_button *button, int button_count)
{
	/* The default button mapping vary depending on the number of buttons
	 * on the device. */
	enum ratbag_button_type button_types[8] = {RATBAG_BUTTON_TYPE_UNKNOWN};
	struct ratbag_button_action button_actions[8] = {
		BUTTON_ACTION_BUTTON(1),
		BUTTON_ACTION_BUTTON(2),
		BUTTON_ACTION_BUTTON(3),
		BUTTON_ACTION_BUTTON(4),
		BUTTON_ACTION_BUTTON(5),
		BUTTON_ACTION_BUTTON(6),
		BUTTON_ACTION_BUTTON(7),
		BUTTON_ACTION_BUTTON(8),
	};

	if (button_count <= 6) {
		button_actions[5].type = RATBAG_BUTTON_ACTION_TYPE_SPECIAL;
		button_actions[5].action.special = RATBAG_BUTTON_ACTION_SPECIAL_RESOLUTION_CYCLE_UP;
		button_actions[6].type = RATBAG_BUTTON_ACTION_TYPE_NONE;
		button_actions[7].type = RATBAG_BUTTON_ACTION_TYPE_NONE;
	} else {
		button_actions[7].type = RATBAG_BUTTON_ACTION_TYPE_SPECIAL;
		button_actions[7].action.special = RATBAG_BUTTON_ACTION_SPECIAL_RESOLUTION_CYCLE_UP;
	}

	button_types[0] = RATBAG_BUTTON_TYPE_LEFT;
	button_types[1] = RATBAG_BUTTON_TYPE_RIGHT;
	button_types[2] = RATBAG_BUTTON_TYPE_MIDDLE;
	button_types[3] = RATBAG_BUTTON_TYPE_THUMB;
	button_types[4] = RATBAG_BUTTON_TYPE_THUMB2;
	if (button_count <= 6) {
		button_types[5] = RATBAG_BUTTON_TYPE_RESOLUTION_CYCLE_UP;
	} else {
		button_types[5] = RATBAG_BUTTON_TYPE_PINKIE;
		button_types[6] = RATBAG_BUTTON_TYPE_PINKIE2;
		button_types[7] = RATBAG_BUTTON_TYPE_RESOLUTION_CYCLE_UP;
	}

	button->type = button_types[button->index];
	ratbag_button_set_action(button, &button_actions[button->index]);
}

static int
steelseries_get_firmware_version(struct ratbag_device *device)
{
	struct steelseries_data *drv_data = device->drv_data;
	int device_version = ratbag_device_data_steelseries_get_device_version(device->data);
	size_t buf_len = STEELSERIES_REPORT_SIZE;
	uint8_t buf[STEELSERIES_REPORT_SIZE] = {0};
	int ret;

	if (device_version == 1)
		return 0;

	buf[0] = STEELSERIES_ID_FIRMWARE;
	msleep(10);
	ret = ratbag_hidraw_output_report(device, buf, buf_len);
	if (ret < 0)
		return ret;

	ret = ratbag_hidraw_read_input_report_index(device, buf, buf_len, STEELSERIES_INPUT_HIDRAW);
	if (ret < 0)
		return ret;

	drv_data->firmware_major = buf[1];
	drv_data->firmware_minor = buf[0];

	return 0;
}

static int
steelseries_read_settings(struct ratbag_device *device)
{
	int device_version = ratbag_device_data_steelseries_get_device_version(device->data);
	struct ratbag_profile *profile = NULL;
	struct ratbag_resolution *resolution;
	struct ratbag_led *led;

	size_t buf_len = STEELSERIES_REPORT_SIZE;
	uint8_t buf[STEELSERIES_REPORT_SIZE] = {0};
	int ret;
	unsigned int active_resolution;

	if (device_version == 1)
		return 0;

	buf[0] = STEELSERIES_ID_SETTTINGS;
	msleep(10);
	ret = ratbag_hidraw_output_report(device, buf, buf_len);
	if (ret < 0)
		return ret;

	ret = ratbag_hidraw_read_input_report_index(device, buf, buf_len, STEELSERIES_INPUT_HIDRAW);
	if (ret < 0)
		return ret;

	active_resolution = buf[1] - 1;
	ratbag_device_for_each_profile(device, profile) {
		ratbag_profile_for_each_resolution(profile, resolution) {
			resolution->is_active = resolution->index == active_resolution;

			resolution->dpi_x = 100 * (1 +  buf[2 + resolution->index*2]);
			resolution->dpi_y = resolution->dpi_x;
		}

		ratbag_profile_for_each_led(profile, led) {
			led->color.red = buf[6 + led->index * 3];
			led->color.green = buf[7 + led->index * 3];
			led->color.blue = buf[8 + led->index * 3];
		}
	}

	return 0;
}

static int
steelseries_probe(struct ratbag_device *device)
{
	struct steelseries_data *drv_data = NULL;
	struct ratbag_profile *profile = NULL;
	struct ratbag_resolution *resolution;
	struct ratbag_button *button;
	struct ratbag_led *led;
	int rc, button_count, led_count, device_version, mono_led, short_button;
	_cleanup_(dpi_list_freep) struct dpi_list *dpilist = NULL;
	_cleanup_(freep) struct dpi_range *dpirange = NULL;

	unsigned int report_rates[] = { 125, 250, 500, 1000 };

	rc = ratbag_find_hidraw(device, steelseries_test_hidraw);
	if (rc)
		return rc;

	rc = ratbag_open_hidraw_index(device,
				      STEELSERIES_INPUT_ENDPOINT,
				      STEELSERIES_INPUT_HIDRAW);
	if (rc)
		return rc;

	device_version = ratbag_device_data_steelseries_get_device_version(device->data);
	button_count = ratbag_device_data_steelseries_get_button_count(device->data);
	led_count = ratbag_device_data_steelseries_get_led_count(device->data);
	dpirange = ratbag_device_data_steelseries_get_dpi_range(device->data);
	dpilist = ratbag_device_data_steelseries_get_dpi_list(device->data);
	mono_led = ratbag_device_data_steelseries_get_mono_led(device->data);
	short_button = ratbag_device_data_steelseries_get_short_button(device->data);

	ratbag_device_init_profiles(device,
				    STEELSERIES_NUM_PROFILES,
				    STEELSERIES_NUM_DPI,
				    button_count,
				    led_count);

	/* only later models allow setting buttons on the device */
	if (ratbag_device_data_steelseries_get_macro_length(device->data) > 0) {
		/* set these caps manually as they are not assumed with only 1 profile */
		ratbag_device_set_capability(device, RATBAG_DEVICE_CAP_BUTTON);
		ratbag_device_set_capability(device, RATBAG_DEVICE_CAP_BUTTON_KEY);
		if (short_button == 0) {
			ratbag_device_set_capability(device, RATBAG_DEVICE_CAP_BUTTON_MACROS);
		}
	}

	ratbag_device_unset_capability(device, RATBAG_DEVICE_CAP_QUERY_CONFIGURATION);

	/* The device does not support reading the current settings. Fall back
	   to some sensible defaults */
	ratbag_device_for_each_profile(device, profile) {
		profile->is_active = true;

		ratbag_profile_for_each_resolution(profile, resolution) {
			if (resolution->index == 0) {
				resolution->is_active = true;
				resolution->is_default = true;
			}

			if (dpirange)
				ratbag_resolution_set_dpi_list_from_range(resolution,
									  dpirange->min,
									  dpirange->max);
			if (dpilist)
				ratbag_resolution_set_dpi_list(resolution,
							       (unsigned int *)dpilist->entries,
							       dpilist->nentries);

			/* 800 and 1600 seem as reasonable defaults supported
			 * by all known devices. */
			resolution->dpi_x = 800 * (resolution->index + 1);
			resolution->dpi_y = 800 * (resolution->index + 1);

			ratbag_resolution_set_report_rate_list(resolution, report_rates,
							       ARRAY_LENGTH(report_rates));
			resolution->hz = 1000;
		}

		ratbag_profile_for_each_button(profile, button) {
			ratbag_button_enable_action_type(button, RATBAG_BUTTON_ACTION_TYPE_BUTTON);
			ratbag_button_enable_action_type(button, RATBAG_BUTTON_ACTION_TYPE_SPECIAL);
			ratbag_button_enable_action_type(button, RATBAG_BUTTON_ACTION_TYPE_KEY);
			if (short_button == 0) {
				ratbag_button_enable_action_type(button, RATBAG_BUTTON_ACTION_TYPE_MACRO);
			}

			button_defaults_for_layout(button, button_count);
		}

		ratbag_profile_for_each_led(profile, led) {
			led->type = led->index == 0 ? RATBAG_LED_TYPE_LOGO : RATBAG_LED_TYPE_WHEEL;
			led->mode = RATBAG_LED_ON;
			if (mono_led) {
				led->colordepth = RATBAG_LED_COLORDEPTH_MONOCHROME;
				led->brightness = 255;
			} else {
				led->colordepth = RATBAG_LED_COLORDEPTH_RGB_888;
				led->color.red = 0;
				led->color.green = 0;
				led->color.blue = 255;
			}
			ratbag_led_set_mode_capability(led, RATBAG_LED_OFF);
			ratbag_led_set_mode_capability(led, RATBAG_LED_ON);
			ratbag_led_set_mode_capability(led, RATBAG_LED_BREATHING);
			if (device_version >= 2)
				ratbag_led_set_mode_capability(led, RATBAG_LED_CYCLE);
		}
	}

	drv_data = zalloc(sizeof(*drv_data));
	ratbag_set_drv_data(device, drv_data);

	steelseries_get_firmware_version(device);
	log_debug(device->ratbag, "SteelSeries firmware version %d.%d\n",
		  drv_data->firmware_major,
		  drv_data->firmware_minor);

	steelseries_read_settings(device);

	return 0;
}

static int
steelseries_write_dpi(struct ratbag_resolution *resolution)
{
	struct ratbag_device *device = resolution->profile->device;
	int device_version = ratbag_device_data_steelseries_get_device_version(device->data);
	struct dpi_list *dpilist = NULL;
	struct dpi_range *dpirange = NULL;
	int ret;
	size_t buf_len;
	uint8_t buf[STEELSERIES_REPORT_SIZE] = {0};

	dpirange = ratbag_device_data_steelseries_get_dpi_range(device->data);
	dpilist = ratbag_device_data_steelseries_get_dpi_list(device->data);

	if (device_version == 1) {
		int i = 0;

		/* when using lists the entries are enumerated in reverse */
		if (dpilist) {
			for (i = 0; i < (int)resolution->ndpis; i++) {
				if (resolution->dpis[i] == resolution->dpi_x)
					break;
			}
			i = resolution->ndpis - i;
		} else {
			i = resolution->dpi_x / dpirange->step - 1;
		}

		buf_len = STEELSERIES_REPORT_SIZE_SHORT;
		buf[0] = STEELSERIES_ID_DPI_SHORT;
		buf[1] = resolution->index + 1;
		buf[2] = i;
	} else {
		buf_len = STEELSERIES_REPORT_SIZE;
		buf[0] = STEELSERIES_ID_DPI;
		buf[2] = resolution->index + 1;
		buf[3] = resolution->dpi_x / dpirange->step - 1;
		buf[6] = 0x42; /* not sure if needed */
	}

	msleep(10);
	ret = ratbag_hidraw_output_report(device, buf, buf_len);
	if (ret < 0)
		return ret;

	return 0;
}

static int
steelseries_write_report_rate(struct ratbag_resolution *resolution)
{
	struct ratbag_device *device = resolution->profile->device;
	int device_version = ratbag_device_data_steelseries_get_device_version(device->data);
	int ret;
	size_t buf_len;
	uint8_t buf[STEELSERIES_REPORT_SIZE] = {0};

	if (device_version == 1) {
		buf_len = STEELSERIES_REPORT_SIZE_SHORT;
		buf[0] = STEELSERIES_ID_REPORT_RATE_SHORT;
		buf[2] = 1000 / resolution->hz;
	} else {
		buf_len = STEELSERIES_REPORT_SIZE;
		buf[0] = STEELSERIES_ID_REPORT_RATE;
		buf[2] = 1000 / resolution->hz;
	}

	msleep(10);
	ret = ratbag_hidraw_output_report(device, buf, buf_len);
	if (ret < 0)
		return ret;

	return 0;
}

static int
steelseries_write_buttons(struct ratbag_profile *profile)
{
	struct ratbag_device *device = profile->device;
	struct ratbag_button *button;
	int ret;

	if (ratbag_device_data_steelseries_get_macro_length(device->data) == 0)
		return 0;

	int short_button = ratbag_device_data_steelseries_get_short_button(device->data);
	int button_size = (short_button == 0) ? 5 : 3;
	int report_size = (short_button == 0) ? STEELSERIES_REPORT_LONG_SIZE : STEELSERIES_REPORT_SIZE_SHORT;
	int max_modifiers = (short_button == 0) ? 3 : 0;

	uint8_t buf[report_size];
	memset(buf, 0, report_size);

	buf[0] = STEELSERIES_ID_BUTTONS;

	ratbag_profile_for_each_button(profile, button) {
		struct ratbag_button_action *action = &button->action;
		uint16_t code;
		unsigned int key, modifiers;
		int idx;

		/* Each button takes up 3 or 5 bytes starting from index 2 */
		idx = 2 + button->index * button_size;

		switch (action->type) {
		case RATBAG_BUTTON_ACTION_TYPE_BUTTON:
			buf[idx] = action->action.button;
			break;

		case RATBAG_BUTTON_ACTION_TYPE_MACRO:
			ratbag_action_keycode_from_macro(action, &key, &modifiers);

			/* There is only space for 3 modifiers */
			if (__builtin_popcount(modifiers) > max_modifiers) {
				log_error(device->ratbag,
					"Too many modifiers in macro for button %d (maximum %d)\n",
					button->index, max_modifiers);
				break;
			}

			code = ratbag_hidraw_get_keyboard_usage_from_keycode(
						device, key);
			if (code) {
				if (short_button) {
					buf[idx] = STEELSERIES_BUTTON_KEY;
				} else {
					buf[idx] = STEELSERIES_BUTTON_KBD;

					if (modifiers & MODIFIER_LEFTCTRL)
						buf[++idx] = 0xE0;
					if (modifiers & MODIFIER_LEFTSHIFT)
						buf[++idx] = 0xE1;
					if (modifiers & MODIFIER_LEFTALT)
						buf[++idx] = 0xE2;
					if (modifiers & MODIFIER_LEFTMETA)
						buf[++idx] = 0xE3;
					if (modifiers & MODIFIER_RIGHTCTRL)
						buf[++idx] = 0xE4;
					if (modifiers & MODIFIER_RIGHTSHIFT)
						buf[++idx] = 0xE5;
					if (modifiers & MODIFIER_RIGHTALT)
						buf[++idx] = 0xE6;
					if (modifiers & MODIFIER_RIGHTMETA)
						buf[++idx] = 0xE7;
				}

				buf[idx + 1] = code;
			} else {
				code = ratbag_hidraw_get_consumer_usage_from_keycode(
						device, action->action.key.key);
				buf[idx] = STEELSERIES_BUTTON_CONSUMER;
				buf[idx + 1] = code;
			}
			break;

		case RATBAG_BUTTON_ACTION_TYPE_SPECIAL:
			switch (action->action.special) {
			case RATBAG_BUTTON_ACTION_SPECIAL_RESOLUTION_CYCLE_UP:
				buf[idx] = STEELSERIES_BUTTON_RES_CYCLE;
				break;
			case RATBAG_BUTTON_ACTION_SPECIAL_WHEEL_UP:
				buf[idx] = STEELSERIES_BUTTON_WHEEL_UP;
				break;
			case RATBAG_BUTTON_ACTION_SPECIAL_WHEEL_DOWN:
				buf[idx] = STEELSERIES_BUTTON_WHEEL_DOWN;
				break;
			default:
				break;
			}
			break;

		case RATBAG_BUTTON_ACTION_TYPE_KEY:
		case RATBAG_BUTTON_ACTION_TYPE_NONE:
		default:
			buf[idx] = STEELSERIES_BUTTON_OFF;
			break;
		}
	}

	msleep(10);
	ret = ratbag_hidraw_output_report(device, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	return 0;
}

static int
steelseries_write_led_v1(struct ratbag_led *led)
{
	struct ratbag_device *device = led->profile->device;
	uint8_t buf[STEELSERIES_REPORT_SIZE_SHORT] = {0};
	int ret;

	buf[0] = STEELSERIES_ID_LED_EFFECT_SHORT;
	buf[1] = led->index + 1;

	switch(led->mode) {
	case RATBAG_LED_OFF:
	case RATBAG_LED_ON:
		buf[2] = 0x01;
		break;
	case RATBAG_LED_BREATHING:
		buf[2] = 0x03; /* 0x02: slow, 0x03: medium, 0x04: fast */
		break;
	case RATBAG_LED_CYCLE:
		/* Cycle mode is not supported on this version */
	default:
		return -EINVAL;
	}

	msleep(10);
	ret = ratbag_hidraw_output_report(device, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	int mono_led = ratbag_device_data_steelseries_get_mono_led(device->data);

	if (mono_led) {
		buf[0] = STEELSERIES_ID_LED_INTENSITY_SHORT;
		buf[1] = led->index + 1;
		if (led->mode == RATBAG_LED_OFF || led->brightness == 0) {
			buf[2] = 1;
		} else {
			//split the brightness into roughly 3 equal intensities
			buf[2] = (led->brightness / 86) + 2;
		}
	} else {
		buf[0] = STEELSERIES_ID_LED_COLOR_SHORT;
		buf[1] = led->index + 1;
		buf[2] = led->color.red;
		buf[3] = led->color.green;
		buf[4] = led->color.blue;
	}

	msleep(10);
	ret = ratbag_hidraw_output_report(device, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	return 0;
}

static void
create_cycle(struct steelseries_led_cycle *cycle)
{
	cycle->led_id = 0x00;
	cycle->duration = 5000;
	cycle->repeat = true;
	cycle->trigger_buttons = 0x00;

	list_init(&cycle->points);
}

static void
construct_cycle_buffer(struct steelseries_led_cycle *cycle, uint8_t *buf, uint8_t buf_size)
{
	struct steelseries_point *point;
	uint16_t duration;
	uint8_t npoints = 0;
	uint16_t cycle_size = 0;

	buf[0] = STEELSERIES_ID_LED;
	buf[2] = cycle->led_id;

	if (!cycle->repeat)
		buf[19] = 0x01;

	buf[23] = cycle->trigger_buttons;

	list_for_each(point, &cycle->points, link) {
		if(npoints == 0){
			/* this is not a mistake,
				we need to write the first
				point to 28,29,30 */
			buf[28] = point->color.red;
			buf[29] = point->color.green;
			buf[30] = point->color.blue;
		}

		cycle_size += point->pos;
		assert(cycle_size < 256);
		assert(34 + npoints*4 <= buf_size);

		buf[31 + npoints*4] = point->color.red;
		buf[32 + npoints*4] = point->color.green;
		buf[33 + npoints*4] = point->color.blue;
		buf[34 + npoints*4] = point->pos;

		npoints++;
	}

	buf[27] = npoints;

	/* this seems to be the minimum allowed */
	duration = max(buf[27] * 330, cycle->duration);
	
	hidpp_set_unaligned_le_u16(&buf[3], duration);
}

static int
steelseries_write_led_v2(struct ratbag_led *led)
{
	struct ratbag_device *device = led->profile->device;
	uint8_t buf[STEELSERIES_REPORT_SIZE] = {0};
	int ret;

	struct steelseries_led_cycle cycle;
	struct steelseries_point point[4];

	const struct ratbag_color black  = { 0x00 };
	const struct ratbag_color red = { 0xFF, 0x00, 0x00 };
	const struct ratbag_color green = { 0x00, 0xFF, 0x00 };
	const struct ratbag_color blue = { 0x00, 0x00, 0xFF };

	create_cycle(&cycle);
	cycle.led_id = led->index;

	switch(led->mode) {
	case RATBAG_LED_OFF:
		point[0].color = black;
		point[0].pos = 0x00;

		list_append(&cycle.points, &point[0].link);
		break;
	case RATBAG_LED_ON:
		point[0].color = led->color;

		list_append(&cycle.points, &point[0].link);
		break;
	case RATBAG_LED_CYCLE:		
		point[0].color = red;
		point[0].pos = 0x00;

		point[1].color = green;
		point[1].pos = 0x55;

		point[2].color = blue;
		point[2].pos = 0x55;

		point[3].color = red;
		point[3].pos = 0x55;

		list_append(&cycle.points, &point[0].link);
		list_append(&cycle.points, &point[1].link);
		list_append(&cycle.points, &point[2].link);
		list_append(&cycle.points, &point[3].link);

		cycle.duration = led->ms;
		break;
	case RATBAG_LED_BREATHING:
		point[0].color = black;
		point[0].pos = 0x00;

		point[1].color = led->color;
		point[1].pos = 0x7F;

		point[2].color = black;
		point[2].pos = 0x7F;
		
		list_append(&cycle.points, &point[0].link);
		list_append(&cycle.points, &point[1].link);
		list_append(&cycle.points, &point[2].link);

		cycle.duration = led->ms;
		break;
	default:
		return -EINVAL;
	}

	construct_cycle_buffer(&cycle, buf, sizeof(buf));

	msleep(10);
	ret = ratbag_hidraw_output_report(device, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	return 0;
}

static int
steelseries_write_led(struct ratbag_led *led)
{
	struct ratbag_device *device = led->profile->device;
	int device_version = ratbag_device_data_steelseries_get_device_version(device->data);

	if (device_version == 1)
		return steelseries_write_led_v1(led);
	else if (device_version == 2)
		return steelseries_write_led_v2(led);

	return -ENOTSUP;
}

static int
steelseries_write_save(struct ratbag_device *device)
{
	int device_version = ratbag_device_data_steelseries_get_device_version(device->data);
	int ret;
	size_t buf_len;
	uint8_t buf[STEELSERIES_REPORT_SIZE] = {0};

	if (device_version == 1) {
		buf_len = STEELSERIES_REPORT_SIZE_SHORT;
		buf[0] = STEELSERIES_ID_SAVE_SHORT;
	} else {
		buf_len = STEELSERIES_REPORT_SIZE;
		buf[0] = STEELSERIES_ID_SAVE;
	}

	msleep(20);
	ret = ratbag_hidraw_output_report(device, buf, buf_len);
	if (ret < 0)
		return ret;

	return 0;
}

static int
steelseries_write_profile(struct ratbag_profile *profile)
{
	struct ratbag_resolution *resolution;
	struct ratbag_button *button;
	struct ratbag_led *led;
	int rc;
	bool buttons_dirty = false;

	ratbag_profile_for_each_resolution(profile, resolution) {
		if (!resolution->dirty)
			continue;

		rc = steelseries_write_dpi(resolution);
		if (rc != 0)
			return rc;

		/* The same hz is used for all resolutions. Only write once. */
		if (resolution->index > 0)
			continue;

		rc = steelseries_write_report_rate(resolution);
		if (rc != 0)
			return rc;
	}

	ratbag_profile_for_each_button(profile, button) {
		if (button->dirty)
			buttons_dirty = true;
	}

	if (buttons_dirty) {
		rc = steelseries_write_buttons(profile);
		if (rc != 0)
			return rc;
	}

	ratbag_profile_for_each_led(profile, led) {
		if (!led->dirty)
			continue;

		rc = steelseries_write_led(led);
		if (rc != 0)
			return rc;
	}

	return 0;
}

static int
steelseries_commit(struct ratbag_device *device)
{
	struct ratbag_profile *profile;
	int rc = 0;

	list_for_each(profile, &device->profiles, link) {
		if (!profile->dirty)
			continue;

		log_debug(device->ratbag,
			  "Profile %d changed, rewriting\n", profile->index);

		rc = steelseries_write_profile(profile);
		if (rc)
			return rc;

		/* persist the current settings on the device */

		rc = steelseries_write_save(device);
		if (rc)
			return rc;
	}

	return 0;
}

static void
steelseries_remove(struct ratbag_device *device)
{
	ratbag_close_hidraw_index(device, 0);
	ratbag_close_hidraw_index(device, 1);
	free(ratbag_get_drv_data(device));
}

struct ratbag_driver steelseries_driver = {
	.name = "SteelSeries",
	.id = "steelseries",
	.probe = steelseries_probe,
	.remove = steelseries_remove,
	.commit = steelseries_commit,
};
