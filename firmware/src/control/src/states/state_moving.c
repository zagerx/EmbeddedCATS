/*
 * state_moving.c — 运动中：一条 PP 运动在途
 *
 * 进入：把目标下发到该轴的 402 推进器（受理失败留到下一拍判故障——
 *       ENTRY 里不能迁移，状态机内核不允许）。
 * 每拍：先看有没有新指令：
 *         位置指令 → 覆盖目标（PP 允许运动中改目标，重新触发 bit4）；
 *         停止指令 → 发 Halt，迁 STOPPING；
 *       再看该轴的运动结论：DONE → READY；FAILED → FAULT。
 *
 * 插补在驱动器内部，本状态**不每拍发帧**，只等结论。
 */

#include "states.h"

static int st_moving_h(struct hsm *me, const struct hsm_event *e);
const struct hsm_state st_moving = {
	.handler = st_moving_h,
	.super = &st_operational,
	.name = "MOVING",
};

/* 把 app->move 下发给对应的轴 */
static int issue_move(struct cats_control *app)
{
	const struct cats_move_cmd *m = &app->move;

	if (m->axis >= app->axis_count) {
		return -1;
	}
	return axis_move_to(&app->axes[m->axis], m->um, m->um_s, m->um_s2);
}

static int st_moving_h(struct hsm *me, const struct hsm_event *e)
{
	struct cats_control *app = container_of(me, struct cats_control, hsm);

	switch (e->sig) {
	case HSM_SIG_ENTRY:
		app->move_err = issue_move(app);
		break;

	case CATS_SIG_TICK: {
		enum cats_cmd_kind cmd;
		enum axis_motion m;

		if (app->move_err != 0) {
			app->fault_code = CATS_FAULT_MOTION_FAILED;
			HSM_TRAN(&st_fault);
		}

		cmd = cats_take_cmd(app, &app->move);
		if (cmd == CATS_CMD_STOP) {
			/* 发 Halt 的动作在 STOPPING 的 ENTRY 里，这里只负责迁过去 */
			HSM_TRAN(&st_stopping);
		}
		if (cmd == CATS_CMD_MOVE) {
			app->move_err = issue_move(app); /* 覆盖目标：PP 允许运动中被改 */
			break;                           /* 下一拍再开始等新结论 */
		}

		m = axis_take_motion(&app->axes[app->move.axis]);
		if (m == AXIS_MOTION_DONE) {
			HSM_TRAN(&st_ready);
		}
		if (m == AXIS_MOTION_FAILED) {
			app->fault_code = CATS_FAULT_MOTION_FAILED;
			HSM_TRAN(&st_fault);
		}
		break;
	}

	case HSM_SIG_EXIT:
		break;

	default:
		return HSM_RET_SUPER;
	}

	return HSM_RET_HANDLED;
}
