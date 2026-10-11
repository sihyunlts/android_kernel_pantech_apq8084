/* SPDX-License-Identifier: GPL-2.0-only */
/* Navigation recovered from EF67S S0226114 fpc_btp. */
#define FPC_NAV_FRAME_SIZE (160 * 8)

enum ef67_fpc_input_mode {
	FPC_INPUT_TOUCH = 1,
	FPC_INPUT_TRACKPAD = 2,
	FPC_INPUT_MOUSE = 3,
};

enum ef67_fpc_contact {
	FPC_CONTACT_DOWN = 1,
	FPC_CONTACT_UP = 2,
	FPC_CONTACT_HOLD = 4,
	FPC_CONTACT_MOVE = 5,
};

/* Byte order also defines the stock nav_settings attribute offsets. */
struct ef67_fpc_nav_settings {
	u8 p_multiplier_x;
	u8 p_multiplier_y;
	u8 p_sensitivity_key;
	u8 p_sensitivity_ptr;
	u8 multiplier_key_accel;
	u8 multiplier_ptr_accel;
	u8 threshold_key_accel;
	u8 threshold_ptr_accel;
	u8 threshold_ptr_start;
	u8 duration_ptr_clear;
	u8 nav_finger_up_threshold;
	u8 btp_input_mode;
};

struct ef67_fpc_nav_state {
	unsigned long down_ms;
	unsigned long pointer_ms;
	int deviation_x;
	int deviation_y;
	int sum_x;
	int sum_y;
	int touch_x;
	int touch_y;
	bool pointer_active;
	bool touch_down;
};

static void ef67_fpc_nav_defaults(struct ef67_fpc_nav_settings *settings,
				 unsigned int mode)
{
	static const u8 defaults[][10] = {
		{ 220, 220, 200, 180, 1, 2, 40, 20, 5, 100 },
		{ 70, 50, 35, 180, 2, 2, 55, 10, 5, 100 },
		{ 150, 100, 180, 170, 1, 2, 40, 30, 2, 100 },
	};

	memcpy(settings, defaults[mode - 1], sizeof(defaults[0]));
	settings->nav_finger_up_threshold = 3;
	settings->btp_input_mode = mode;
}

static unsigned int ef67_fpc_nav_difference(const u8 *a, const u8 *b)
{
	unsigned int row, column, difference = 0;

	for (row = 0; row < 2; row++)
		for (column = 0; column < 112; column++)
			difference += abs(a[row * 160 + column] -
					  b[row * 160 + column]);
	return difference;
}

/* Stock compares both image orders, searching X -8..8 and Y 0..5. */
static void ef67_fpc_nav_movement(const u8 *previous_image, const u8 *current_image,
				 int *dx, int *dy)
{
	unsigned int best = 10200, forward = 0, reverse = 0, zero = 0;
	int x, y, best_x = 0, best_y = 0;

	for (y = 0; y < 6; y++) {
		for (x = -8; x <= 8; x++) {
			forward = ef67_fpc_nav_difference(previous_image + 24,
					current_image + y * 160 + 24 + x);
			if (forward < best) {
				best = forward;
				best_x = x;
				best_y = y;
			}
			reverse = ef67_fpc_nav_difference(current_image + 24,
					previous_image + y * 160 + 24 + x);
			if (reverse < best) {
				best = reverse;
				best_x = -x;
				best_y = -y;
			}
			if (!x && !y)
				zero = reverse;
		}
	}
	/* Reject a shift too similar to the unshifted image difference. */
	if ((best_x || best_y) && best &&
	    zero * 1000 / best <= 1399) {
		best_x = 0;
		best_y = 0;
	}
	*dx = best_x;
	*dy = best_y;
}

static int ef67_fpc_nav_accelerate(int value, unsigned int threshold,
				  unsigned int multiplier)
{
	if (abs(value) > threshold) {
		int accelerated = threshold + (abs(value) - threshold) * multiplier;

		return value > 0 ? accelerated : -accelerated;
	}
	return value;
}

static int ef67_fpc_nav_pointer_step(int value)
{
	int magnitude = abs(value);
	int divisor = magnitude > 40 ? 5 : magnitude > 20 ? 4 :
		      magnitude > 10 ? 3 : 2;
	int step = value / divisor;

	/* Preserve stock's integer remainder and minimum-step convention. */
	return step ? step : value > 0 ? 1 : -1;
}

static void ef67_fpc_nav_event(struct ef67_fpc_nav_state *state,
		const struct ef67_fpc_nav_settings *settings,
		struct input_dev *input, int dx, int dy, unsigned int contact,
		unsigned long now_ms)
{
	int x = dy, y = dx;
	unsigned int mode = settings->btp_input_mode;

	if (contact == FPC_CONTACT_DOWN)
		state->down_ms = now_ms;
	if (state->down_ms) {
		state->deviation_x += dx;
		state->deviation_y += dy;
		if (state->deviation_x * state->deviation_x +
		    state->deviation_y * state->deviation_y > 100) {
			state->down_ms = 0;
			state->deviation_x = 0;
			state->deviation_y = 0;
		} else if (now_ms - state->down_ms > 900) {
			contact = FPC_CONTACT_HOLD;
			state->down_ms = 0;
			state->deviation_x = 0;
			state->deviation_y = 0;
		}
	}
	if (mode == FPC_INPUT_MOUSE) {
		if (contact == FPC_CONTACT_HOLD) {
			input_report_key(input, BTN_RIGHT, 1);
			input_sync(input);
			input_report_key(input, BTN_RIGHT, 0);
			input_sync(input);
			return;
		}
		if (contact == FPC_CONTACT_DOWN || contact == FPC_CONTACT_UP) {
			state->sum_x = 0;
			state->sum_y = 0;
			state->pointer_active = false;
		}
		if (state->pointer_ms && now_ms - state->pointer_ms >
		    settings->duration_ptr_clear) {
			state->sum_x = 0;
			state->sum_y = 0;
			state->pointer_active = false;
		}
		state->pointer_ms = now_ms;
		if (!state->pointer_active &&
		    abs(x) < settings->threshold_ptr_start &&
		    abs(y) < settings->threshold_ptr_start)
			return;
		state->pointer_active = true;
		x = ef67_fpc_nav_accelerate(x, settings->threshold_ptr_accel,
					 settings->multiplier_ptr_accel);
		y = ef67_fpc_nav_accelerate(y, settings->threshold_ptr_accel,
					 settings->multiplier_ptr_accel);
		x = x * settings->p_multiplier_x / 100 *
			settings->p_sensitivity_ptr / 100;
		y = y * settings->p_multiplier_y / 100 *
			settings->p_sensitivity_ptr / 100;
		if (state->sum_x * x < 0)
			state->sum_x = 0;
		if (state->sum_y * y < 0)
			state->sum_y = 0;
		x += state->sum_x;
		y += state->sum_y;
		dx = ef67_fpc_nav_pointer_step(x);
		dy = ef67_fpc_nav_pointer_step(y);
		state->sum_x = x - dx;
		state->sum_y = y - dy;
		input_report_rel(input, REL_X, dx);
		input_report_rel(input, REL_Y, dy);
		input_sync(input);
		return;
	}
	if (mode == FPC_INPUT_TOUCH) {
		if (contact == FPC_CONTACT_DOWN) {
			state->touch_x = 720;
			state->touch_y = 1280;
		} else if (contact != FPC_CONTACT_UP &&
			   contact != FPC_CONTACT_MOVE) {
			return;
		}
		if (contact == FPC_CONTACT_MOVE)
			y = -y;
	}
	x = ef67_fpc_nav_accelerate(x, settings->threshold_key_accel,
					settings->multiplier_key_accel);
	y = ef67_fpc_nav_accelerate(y, settings->threshold_key_accel,
					settings->multiplier_key_accel);
	x = x * settings->p_multiplier_x / 100 *
		settings->p_sensitivity_key / 100;
	y = y * settings->p_multiplier_y / 100 *
		settings->p_sensitivity_key / 100;
	if (mode == FPC_INPUT_TRACKPAD) {
		input_report_rel(input, REL_X, x);
		input_report_rel(input, REL_Y, y);
	} else {
		if (contact == FPC_CONTACT_MOVE) {
			if (!x && !y)
				return;
			if (abs(x) > abs(y)) {
				x = clamp(state->touch_x + x, 0, 1439);
				if (x == state->touch_x)
					return;
				state->touch_x = x;
			} else {
				y = clamp(state->touch_y - y, 0, 2559);
				if (y == state->touch_y)
					return;
				state->touch_y = y;
			}
		}
		input_report_abs(input, ABS_X, state->touch_x);
		input_report_abs(input, ABS_Y, state->touch_y);
		input_report_abs(input, ABS_Z, 255);
		state->touch_down = contact != FPC_CONTACT_UP;
		input_report_key(input, BTN_TOUCH, state->touch_down);
	}
	input_sync(input);
}
