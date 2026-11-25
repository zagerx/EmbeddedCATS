/*
 * state_root.c — 处理业务状态机的全局急停与故障出口
 *
 * 急停在任何状态下都迁移到 ESTOP；子状态锁存的故障在节拍中统一进入 FAULT。
 * 全局规则属于业务层，不属于通用 HSM 内核。
 *
 * Author: zager
 */

#include "states.h"
#include "controller_log.h"

/**
 * st_root - 处理全局急停、故障迁移和事件上抛
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: 急停或故障迁移结果；其他事件交给 sm_top。
 */
sm_ret_t st_root(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct controller *ctl = container_of(me, struct controller, hsm);

	if (e->sig == CONTROLLER_SIG_ESTOP) {
		return SM_TRAN(&st_estop); /* 急停优先于一切，连故障出口规则也让位 */
	}

	if (e->sig == CONTROLLER_SIG_TICK &&
	    ctl->info.fault.reason != CONTROLLER_FAULT_REASON_NONE) {
		return SM_TRAN(&st_fault); /* 任何状态下锁存了故障，都在这里统一出去 */
	}

	switch (e->sig) {
	case CONTROLLER_SIG_MOVE:
	case CONTROLLER_SIG_HALT:
	case CONTROLLER_SIG_ENABLE:
	case CONTROLLER_SIG_HOME:
	case CONTROLLER_SIG_ESTOP_CLEAR:
		controller_log_not_executed(ctl, (enum controller_sig)e->sig,
					    CONTROLLER_FAULT_NO_AXIS,
					    CONTROLLER_NOT_EXECUTED_INVALID_STATE);
		return SM_HANDLED();
	default:
		break;
	}

	return SM_SUPER(&sm_top); /* 交给框架顶层：冒泡结束，事件被忽略 */
}
