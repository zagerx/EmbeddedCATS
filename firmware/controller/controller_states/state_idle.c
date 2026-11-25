/*
 * state_idle.c — 已配置、未使能的空闲态，等待外部使能指令
 *
 * 进入本态时各轴已配置完成但未上使能，不自行推进任何流程。
 * 收到使能事件才进入 ENABLING；位置与暂停指令在此态没有意义。
 *
 * Author: zager
 */

#include "states.h"
#include "controller_log.h"

/**
 * st_idle_ctrl - 在空闲态等待使能指令
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: ENABLE 迁移 ENABLING；MOVE / HALT 被丢弃；其余按层次上抛。
 */
sm_ret_t st_idle_ctrl(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct controller *ctl = container_of(me, struct controller, hsm);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		break; /* 配置已在下游完成，本态不发任何帧 */

	case SM_SIG_EXIT:
		break; /* 无出口动作 */

	case CONTROLLER_SIG_ENABLE:
		return SM_TRAN(&st_enabling);

	case CONTROLLER_SIG_MOVE:
	case CONTROLLER_SIG_HALT:
		/* 本事件不会迁移：日志取到的仍是实际拒绝它的 IDLE */
		controller_log_not_executed(ctl, (enum controller_sig)e->sig,
					    CONTROLLER_FAULT_NO_AXIS,
					    CONTROLLER_NOT_EXECUTED_INVALID_STATE);
		return SM_HANDLED();

	default:
		return SM_SUPER(&st_root);
	}

	return SM_HANDLED();
}
