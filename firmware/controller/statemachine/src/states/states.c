/*
 * states.c — 注册业务状态并提供状态与故障名称映射
 *
 * 状态表下标与 enum cats_state 一一对应，是状态函数的唯一注册点。
 *
 * Author: zager
 */

#include "states.h"

const sm_state_t states[CATS_STATE_COUNT] = {
	[CATS_STATE_PREPARING] = st_preparing, [CATS_STATE_CONFIGURING] = st_configuring,
	[CATS_STATE_ENABLING] = st_enabling,   [CATS_STATE_HOLDING] = st_holding,
	[CATS_STATE_MOVING] = st_moving,       [CATS_STATE_HALTING] = st_halting,
	[CATS_STATE_FAULT] = st_fault,         [CATS_STATE_ESTOP] = st_estop,
};

static const char *const NAMES[CATS_STATE_COUNT];
static const char *const FAULT_NAMES[CATS_FAULT_STOP_FAILED + 1];

/**
 * initial - 定义状态机的顶层初始转移
 * @me: 状态机内核对象
 * @e: SM_SIG_INIT 保留事件
 *
 * Return: 迁移到 st_preparing，由内核执行 ROOT 和 PREPARING 的 ENTRY 链。
 */
sm_ret_t initial(struct sm_hsm *const me, const struct sm_event *const e)
{
	(void)e;
	return SM_TRAN(&st_preparing);
}

/**
 * stm_state_name - 获取业务状态名称
 * @s: 业务状态枚举
 *
 * Return: 静态名称字符串；未知状态返回 "?"。
 */
const char *stm_state_name(enum cats_state s)
{
	if ((int)s < 0 || s >= CATS_STATE_COUNT) {
		return "?";
	}
	return NAMES[s];
}

/**
 * stm_fault_name - 获取业务故障名称
 * @f: 业务故障枚举
 *
 * Return: 静态名称字符串；未知故障返回 "?"。
 */
const char *stm_fault_name(enum cats_fault f)
{
	if ((int)f < 0 || f >= (enum cats_fault)(sizeof(FAULT_NAMES) / sizeof(FAULT_NAMES[0]))) {
		return "?";
	}
	return FAULT_NAMES[f];
}

static const char *const NAMES[CATS_STATE_COUNT] = {
	[CATS_STATE_PREPARING] = "PREPARING", [CATS_STATE_CONFIGURING] = "CONFIGURING",
	[CATS_STATE_ENABLING] = "ENABLING",   [CATS_STATE_HOLDING] = "HOLDING",
	[CATS_STATE_MOVING] = "MOVING",       [CATS_STATE_HALTING] = "HALTING",
	[CATS_STATE_FAULT] = "FAULT",         [CATS_STATE_ESTOP] = "ESTOP",
};

static const char *const FAULT_NAMES[] = {
	[CATS_FAULT_NONE] = "NONE",
	[CATS_FAULT_BOOTUP_TIMEOUT] = "BOOTUP_TIMEOUT",
	[CATS_FAULT_HEARTBEAT_LOST] = "HEARTBEAT_LOST",
	[CATS_FAULT_CONFIG_TIMEOUT] = "CONFIG_TIMEOUT",
	[CATS_FAULT_CONFIG_FAILED] = "CONFIG_FAILED",
	[CATS_FAULT_ENABLE_TIMEOUT] = "ENABLE_TIMEOUT",
	[CATS_FAULT_ENABLE_FAILED] = "ENABLE_FAILED",
	[CATS_FAULT_MOTION_FAILED] = "MOTION_FAILED",
	[CATS_FAULT_STOP_FAILED] = "STOP_FAILED",
};
