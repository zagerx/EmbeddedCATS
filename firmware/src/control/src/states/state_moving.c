/*
 * state_moving.c — 运动中：一条 PP 运动在途
 *
 * 进入：把目标下发给该轴的 402 推进器；受理失败只置故障码（ENTRY 里不能迁移，
 *       由 ROOT 在下一拍统一迁 FAULT）。
 * 每拍：先看有没有新指令——
 *         位置指令 → 覆盖目标（PP 允许运动中改目标，重新触发 bit4）；
 *         停止指令 → 迁 HALTING（发 Halt 的动作在 HALTING 的 ENTRY 里）；
 *       再看该轴的运动结论：DONE → HOLDING；FAILED → 置 MOTION_FAILED。
 *
 * 插补在驱动器内部，本状态**不每拍发帧**，只等结论。
 */

#include "states.h"

/* 把 ctl->move 下发给对应的轴 */
static int issue_move(struct cats_control *ctl)
{
	const struct cats_move_cmd *m = &ctl->move;

	if (m->axis >= ctl->axis_count) {
		return -1;
	}
	return axis_move_to(&ctl->axes[m->axis], m->mm, m->mm_s, m->mm_s2);
}

sm_ret_t st_moving(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cats_control *ctl = container_of(me, struct cats_control, hsm);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		/* <0 = 真失败（没使能/参数非法）；>0 = 忙，不会发生在这里 */
		if (issue_move(ctl) < 0) {
			ctl->fault_code = CATS_FAULT_MOTION_FAILED;
		}
		break;

	case CATS_SIG_MOVE:
		/* 运动中的新目标：cia402 层不接受改目标，本段跑完再说 ⇒ 丢弃 */
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
