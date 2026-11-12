/*
 * state_holding.c — 保持已使能状态并受理新的位置指令
 *
 * MOVE 迁移到 MOVING；没有运动在途时 STOP 直接忽略。
 *
 * Author: zager
 */

#include "states.h"

/**
 * st_holding - 处理保持状态下的位置、停止和节拍事件
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: MOVE 迁移 MOVING；STOP 消费；其他事件按状态层次处理或上抛。
 */
sm_ret_t st_holding(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct controller *ctl = container_of(me, struct controller, hsm);

	switch (e->sig) {
	case SM_SIG_ENTRY: {
		uint8_t i;

		ctl->fault_code = CONTROLLER_FAULT_NONE;
		for (i = 0; i < ctl->axis_count; i++) {
			(void)axis_take_motion(&ctl->axes[i]); /* 丢弃旧结论 */
		}
		break;
	}

	case CONTROLLER_SIG_MOVE:
		/* 事件自带参数：拷贝到 ctl->move，供 MOVING 的 ENTRY 下发 */
		ctl->move = SM_EVT_CAST(struct controller_evt, e)->u.move;
		return SM_TRAN(&st_moving);

	case CONTROLLER_SIG_STOP:
		return SM_HANDLED(); /* 已经在保持位置，没有可做的事 */

	case CONTROLLER_SIG_TICK:
		return SM_SUPER(&st_online); /* 没有命令：让 ONLINE 补做心跳判定 */

	case SM_SIG_EXIT:
		break;

	default:
		return SM_SUPER(&st_online);
	}

	return SM_HANDLED();
}
