/*
 * state_moving.c — 运动中：一条 PP 运动在途
 *
 * 进入：把目标下发给该轴的 402 推进器；受理失败只置故障码（ENTRY 里不能迁移，
 *       由 ONLINE 在下一拍统一迁 FAULT）。
 * 每拍：先看有没有新指令——
 *         位置指令 → 覆盖目标（PP 允许运动中改目标，重新触发 bit4）；
 *         停止指令 → 迁 HALTING（发 Halt 的动作在 HALTING 的 ENTRY 里）；
 *       再看该轴的运动结论：DONE → HOLDING；FAILED → 置 MOTION_FAILED。
 *
 * 插补在驱动器内部，本状态**不每拍发帧**，只等结论。
 */

#include "states.h"

static int st_moving_h(struct hsm *me, const struct hsm_event *e);
const struct hsm_state st_moving = {
	.handler = st_moving_h,
	.super = &st_online,
	.name = "MOVING",
};

/* 把 ctl->move 下发给对应的轴 */
static int issue_move(struct cats_control *ctl)
{
	const struct cats_move_cmd *m = &ctl->move;

	if (m->axis >= ctl->axis_count) {
		return -1;
	}
	return axis_move_to(&ctl->axes[m->axis], m->um, m->um_s, m->um_s2);
}

static int st_moving_h(struct hsm *me, const struct hsm_event *e)
{
	struct cats_control *ctl = container_of(me, struct cats_control, hsm);

	switch (e->sig) {
	case HSM_SIG_ENTRY:
		if (issue_move(ctl) != 0) {
			ctl->fault_code = CATS_FAULT_MOTION_FAILED;
		}
		break;

	case CATS_SIG_TICK: {
		enum cats_cmd_kind cmd;
		enum axis_motion m;

		cmd = cats_take_cmd(ctl, &ctl->move);
		if (cmd == CATS_CMD_STOP) {
			HSM_TRAN(&st_halting); /* Halt 动作在 HALTING 的 ENTRY */
		}
		if (cmd == CATS_CMD_MOVE) {
			if (issue_move(ctl) != 0) { /* 覆盖目标：PP 允许运动中被改 */
				ctl->fault_code = CATS_FAULT_MOTION_FAILED;
			}
			return HSM_RET_SUPER; /* 下一拍再开始等新结论 */
		}

		m = axis_take_motion(&ctl->axes[ctl->move.axis]);
		if (m == AXIS_MOTION_DONE) {
			HSM_TRAN(&st_holding);
		}
		if (m == AXIS_MOTION_FAILED) {
			ctl->fault_code = CATS_FAULT_MOTION_FAILED;
		}
		return HSM_RET_SUPER;
	}

	case HSM_SIG_EXIT:
		break;

	default:
		return HSM_RET_SUPER;
	}

	return HSM_RET_HANDLED;
}
