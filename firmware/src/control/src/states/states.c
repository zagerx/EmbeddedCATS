/*
 * states.c — 状态表与状态名映射（**唯一的注册点**）
 *
 * 新增状态时：写好 state_xxx.c → 在 states.h 加声明 → 在本表加一行。
 * 下标与 enum cats_state 一一对应，靠 static_assert 保证不会错位。
 */

#include "states.h"

/* ---- 状态表：下标 = enum cats_state，值 = 状态描述符 ---- */

const struct hsm_state *const states[CATS_STATE_COUNT] = {
	[CATS_STATE_PREPARING] = &st_preparing, [CATS_STATE_ENABLING] = &st_enabling,
	[CATS_STATE_HOLDING] = &st_holding,     [CATS_STATE_MOVING] = &st_moving,
	[CATS_STATE_HALTING] = &st_halting,     [CATS_STATE_FAULT] = &st_fault,
	[CATS_STATE_ESTOP] = &st_estop,
};

static const char *const NAMES[CATS_STATE_COUNT] = {
	[CATS_STATE_PREPARING] = "PREPARING", [CATS_STATE_ENABLING] = "ENABLING",
	[CATS_STATE_HOLDING] = "HOLDING",     [CATS_STATE_MOVING] = "MOVING",
	[CATS_STATE_HALTING] = "HALTING",     [CATS_STATE_FAULT] = "FAULT",
	[CATS_STATE_ESTOP] = "ESTOP",
};

static const char *const FAULT_NAMES[] = {
	[CATS_FAULT_NONE] = "NONE",
	[CATS_FAULT_BOOTUP_TIMEOUT] = "BOOTUP_TIMEOUT",
	[CATS_FAULT_HEARTBEAT_LOST] = "HEARTBEAT_LOST",
	[CATS_FAULT_ENABLE_TIMEOUT] = "ENABLE_TIMEOUT",
	[CATS_FAULT_ENABLE_FAILED] = "ENABLE_FAILED",
	[CATS_FAULT_MOTION_FAILED] = "MOTION_FAILED",
	[CATS_FAULT_STOP_FAILED] = "STOP_FAILED",
};

const char *cats_control_state_name(enum cats_state s)
{
	if ((int)s < 0 || s >= CATS_STATE_COUNT) {
		return "?";
	}
	return NAMES[s];
}

const char *cats_control_fault_name(enum cats_fault f)
{
	if ((int)f < 0 || f >= (enum cats_fault)(sizeof(FAULT_NAMES) / sizeof(FAULT_NAMES[0]))) {
		return "?";
	}
	return FAULT_NAMES[f];
}

uint32_t now_ms(struct cats_control *ctl)
{
	return ctl->master != NULL ? can_io_now_ms(ctl->master->bus) : 0;
}
