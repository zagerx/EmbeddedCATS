/*
 * state_stopping.c — 停止中：减速停在当前位置（保持使能）
 *
 * 进入：给该轴发 Halt（6040 bit8）——不停使能，轴还在保持力矩。
 * 每拍：等该轴报"停稳"：DONE → READY（可继续接指令）；FAILED → FAULT。
 * 退出：无动作（bit8 由 402 层在停稳后自己清掉）。
 *
 * 说明：本状态是"停一下"，不是"关机"。下使能 + NMT pre-op 属于另一条流程，
 * 将来按需加成独立状态 SHUTDOWN（出口是终态，和这里不同）。
 */

#include "states.h"

static int st_stopping_h(struct hsm *me, const struct hsm_event *e);
const struct hsm_state st_stopping = {
	.handler = st_stopping_h,
	.super = &st_operational,
	.name = "STOPPING",
};

static int st_stopping_h(struct hsm *me, const struct hsm_event *e)
{
	struct cats_app *app = container_of(me, struct cats_app, hsm);

	switch (e->sig) {
	case HSM_SIG_ENTRY:
		app->move_err = (axis_halt(&app->axes[app->move.axis]) == 0) ? 0 : -1;
		break;

	case CATS_SIG_TICK: {
		enum axis_motion m;

		if (app->move_err != 0) {
			app->fault_code = CATS_FAULT_STOP_FAILED;
			HSM_TRAN(&st_fault);
		}

		m = axis_take_motion(&app->axes[app->move.axis]);
		if (m == AXIS_MOTION_DONE) {
			HSM_TRAN(&st_ready);
		}
		if (m == AXIS_MOTION_FAILED) {
			app->fault_code = CATS_FAULT_STOP_FAILED;
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
