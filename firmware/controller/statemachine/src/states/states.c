/* 状态表与名称映射；数组下标与 enum cats_state 一一对应。 */

#include "states.h"

/* ---- 状态表：下标 = enum cats_state，值 = 状态函数 ---- */

const sm_state_t states[CATS_STATE_COUNT] = {
	[CATS_STATE_PREPARING] = st_preparing, [CATS_STATE_CONFIGURING] = st_configuring,
	[CATS_STATE_ENABLING] = st_enabling,   [CATS_STATE_HOLDING] = st_holding,
	[CATS_STATE_MOVING] = st_moving,       [CATS_STATE_HALTING] = st_halting,
	[CATS_STATE_FAULT] = st_fault,         [CATS_STATE_ESTOP] = st_estop,
};

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

/* ---- 顶层初始转移 ----
 * sm_hsm_init() 带着 SM_SIG_INIT 调这里；从 sm_top 出发进 PREPARING，
 * 内核会把 sm_top -> st_root -> st_preparing 的 ENTRY 链走一遍。
 */
sm_ret_t initial(struct sm_hsm *const me, const struct sm_event *const e)
{
	(void)e;
	return SM_TRAN(&st_preparing);
}

const char *stm_state_name(enum cats_state s)
{
	if ((int)s < 0 || s >= CATS_STATE_COUNT) {
		return "?";
	}
	return NAMES[s];
}

const char *stm_fault_name(enum cats_fault f)
{
	if ((int)f < 0 || f >= (enum cats_fault)(sizeof(FAULT_NAMES) / sizeof(FAULT_NAMES[0]))) {
		return "?";
	}
	return FAULT_NAMES[f];
}

