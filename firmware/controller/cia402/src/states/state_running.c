/*
 * state_running.c — 所有流程状态的父状态
 *
 * 这一层只做三件事，全部流程状态共同继承：
 *   1. 被抢占 / 超时 / 手动取消时统一退出（结论由 st_idle 处理）；
 *   2. 流程进行中不刷反馈，避免与当前事务争用同一条对象通道；
 *   3. 兜底处理漏派发的完成事件，避免状态机卡死。
 *
 * Author: zager
 */

#include "states.h"

/**
 * st_running - 流程中的公共规则
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: 取消迁回 st_idle；其余上抛。
 */
sm_ret_t st_running(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case CIA402_SIG_CANCEL:
		/* 打断在途事务由 device.c 负责，这里只退出当前流程 */
		return SM_TRAN(&st_idle);

	case CIA402_SIG_TICK:
		return SM_HANDLED();

	case CIA402_SIG_SDO_DONE:
		/* 子状态都应自行处理完成事件，走到这里说明漏了分支 */
		dev_fail(d, DEV_SDO_RESULT(e));
		return SM_TRAN(&st_idle);

	default:
		return SM_SUPER(&sm_top);
	}
}
