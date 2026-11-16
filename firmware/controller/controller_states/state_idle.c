/*
 * state_idle.c — 已配置、未使能的空闲态，等待外部使能指令
 *
 * 进入本态时各轴已配置完成但未上使能，不自行推进任何流程。
 * 收到使能事件才进入 ENABLING；位置与停止指令在此态没有意义。
 *
 * Author: zager
 */

#include "states.h"

/**
 * st_idle_ctrl - 在空闲态等待使能指令
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: ENABLE 迁移 ENABLING；MOVE / STOP 被丢弃；其余按层次上抛。
 */
sm_ret_t st_idle_ctrl(struct sm_hsm *const me, const struct sm_event *const e)
{
	switch (e->sig) {
	case SM_SIG_ENTRY:
		break; /* 配置已在下游完成，本态不发任何帧 */

	case SM_SIG_EXIT:
		break; /* 无出口动作 */

	case CONTROLLER_SIG_ENABLE:
		return SM_TRAN(&st_enabling);

	case CONTROLLER_SIG_MOVE:
	case CONTROLLER_SIG_STOP:
		/* 未使能：位置指令无处执行，停止也没有在途运动 */
		return SM_HANDLED();

	default:
		/* 心跳检查仍由 st_online 负责：空闲不等于可以掉线 */
		return SM_SUPER(&st_online);
	}

	return SM_HANDLED();
}
