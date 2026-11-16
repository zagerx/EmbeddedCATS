/*
 * states.c — 注册业务状态并提供状态与故障名称映射
 *
 * 状态表下标与 enum controller_state 一一对应，是状态函数的唯一注册点。
 *
 * Author: zager
 */

#include "states.h"

const sm_state_t states[CONTROLLER_STATE_COUNT] = {
	[CONTROLLER_STATE_PREPARING] = st_preparing,
	[CONTROLLER_STATE_CONFIGURING] = st_configuring,
	[CONTROLLER_STATE_IDLE] = st_idle_ctrl,
	[CONTROLLER_STATE_ENABLING] = st_enabling,
	[CONTROLLER_STATE_HOLDING] = st_holding,
	[CONTROLLER_STATE_MOVING] = st_moving,
	[CONTROLLER_STATE_HALTING] = st_halting,
	[CONTROLLER_STATE_FAULT] = st_fault,
	[CONTROLLER_STATE_ESTOP] = st_estop,
};

static const char *const NAMES[CONTROLLER_STATE_COUNT];
static const char *const FAULT_NAMES[CONTROLLER_FAULT_STOP_FAILED + 1];

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
 * controller_state_name - 获取业务状态名称
 * @s: 业务状态枚举
 *
 * Return: 静态名称字符串；未知状态返回 "?"。
 */
const char *controller_state_name(enum controller_state s)
{
	if ((int)s < 0 || s >= CONTROLLER_STATE_COUNT) {
		return "?";
	}
	return NAMES[s];
}

/**
 * controller_fault_name - 获取业务故障名称
 * @f: 业务故障枚举
 *
 * Return: 静态名称字符串；未知故障返回 "?"。
 */
const char *controller_fault_name(enum controller_fault f)
{
	if ((int)f < 0 ||
	    f >= (enum controller_fault)(sizeof(FAULT_NAMES) / sizeof(FAULT_NAMES[0]))) {
		return "?";
	}
	return FAULT_NAMES[f];
}

static const char *const NAMES[CONTROLLER_STATE_COUNT] = {
	[CONTROLLER_STATE_PREPARING] = "PREPARING", [CONTROLLER_STATE_CONFIGURING] = "CONFIGURING",
	[CONTROLLER_STATE_IDLE] = "IDLE",           [CONTROLLER_STATE_ENABLING] = "ENABLING",
	[CONTROLLER_STATE_HOLDING] = "HOLDING",     [CONTROLLER_STATE_MOVING] = "MOVING",
	[CONTROLLER_STATE_HALTING] = "HALTING",     [CONTROLLER_STATE_FAULT] = "FAULT",
	[CONTROLLER_STATE_ESTOP] = "ESTOP",
};

static const char *const FAULT_NAMES[] = {
	[CONTROLLER_FAULT_NONE] = "NONE",
	[CONTROLLER_FAULT_BOOTUP_TIMEOUT] = "BOOTUP_TIMEOUT",
	[CONTROLLER_FAULT_HEARTBEAT_LOST] = "HEARTBEAT_LOST",
	[CONTROLLER_FAULT_CONFIG_TIMEOUT] = "CONFIG_TIMEOUT",
	[CONTROLLER_FAULT_CONFIG_FAILED] = "CONFIG_FAILED",
	[CONTROLLER_FAULT_ENABLE_TIMEOUT] = "ENABLE_TIMEOUT",
	[CONTROLLER_FAULT_ENABLE_FAILED] = "ENABLE_FAILED",
	[CONTROLLER_FAULT_MOTION_FAILED] = "MOTION_FAILED",
	[CONTROLLER_FAULT_STOP_FAILED] = "STOP_FAILED",
};
