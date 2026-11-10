/*
 * state_moving.c — 下发一条 PP 运动并等待运动结论
 *
 * 运动期间的新位置指令被丢弃，STOP 迁移到 HALTING。
 *
 * Author: zager
 */

#include "states.h"

/* 将当前运动事件参数下发到目标轴 */
static int issue_move(struct stm *ctl);

/**
 * st_moving - 处理运动发起、停止请求和运动结论
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: 到位迁移 HOLDING；停止迁移 HALTING；失败交由 ROOT 处理。
 */
sm_ret_t st_moving(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct stm *ctl = container_of(me, struct stm, hsm);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		/* <0 = 真失败（没使能/参数非法）；>0 = 忙，不会发生在这里 */
		if (issue_move(ctl) < 0) {
			ctl->fault_code = CATS_FAULT_MOTION_FAILED;
		}
		break;

	case CATS_SIG_MOVE:
		/* 运动中的新目标：驱动层不接受改目标，本段跑完再说 ⇒ 丢弃 */
		return SM_HANDLED();

	case CATS_SIG_STOP:
		return SM_TRAN(&st_halting); /* Halt 动作在 HALTING 的 ENTRY */

	case CATS_SIG_TICK: {
		enum axis_motion m = axis_take_motion(&ctl->axes[ctl->move.axis]);

		if (m == AXIS_MOTION_DONE) {
			return SM_TRAN(&st_holding);
		}
		if (m == AXIS_MOTION_FAILED) {
			ctl->fault_code = CATS_FAULT_MOTION_FAILED;
		}
		return SM_SUPER(&st_online);
	}

	case SM_SIG_EXIT:
		break;

	default:
		return SM_SUPER(&st_online);
	}

	return SM_HANDLED();
}

static int issue_move(struct stm *ctl)
{
	const struct cats_move_cmd *m = &ctl->move;

	if (m->axis >= ctl->axis_count) {
		return -1;
	}
	return axis_move_to(&ctl->axes[m->axis], m->mm, m->mm_s, m->mm_s2);
}
