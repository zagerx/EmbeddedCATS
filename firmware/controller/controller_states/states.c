/*
 * states.c — 注册业务状态函数
 *
 * 状态表下标与 enum controller_state 一一对应，是状态函数的唯一注册点。
 * 本文件不产出任何字符串：状态与故障只以枚举值对外发布。
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
