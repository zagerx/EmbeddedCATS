/*
 * state_halting.c — 减速停在途（Halt，保持使能）
 *
 * 进入：给该轴发 Halt（6040 bit8）——不停使能，轴还在保持力矩。
 * 每拍：取指令槽并**显式丢弃**（停止过程中不接受新目标；不丢弃的话它会留在
 *       槽里，等停稳进 HOLDING 后被意外执行）；
 *       再等该轴报"停稳"：DONE → HOLDING；FAILED → 置 STOP_FAILED。
 *
 * 名字不叫 STOPPING：这里做的是 6040 bit8 = Halt（停在当前位置、保持力矩），
 * 不是"停止程序/停机"。
 */

#include "states.h"

static int st_halting_h(struct hsm *me, const struct hsm_event *e);
const struct hsm_state st_halting = {
	.handler = st_halting_h,
	.super = &st_online,
	.name = "HALTING",
};

static int st_halting_h(struct hsm *me, const struct hsm_event *e)
{
	struct cats_control *ctl = container_of(me, struct cats_control, hsm);

	switch (e->sig) {
	case HSM_SIG_ENTRY:
		if (axis_halt(&ctl->axes[ctl->move.axis]) != 0) {
			ctl->fault_code = CATS_FAULT_STOP_FAILED; /* ENTRY 里不迁移，交 ONLINE */
		}
		break;

	case CATS_SIG_TICK: {
		enum axis_motion m;

		(void)cats_take_cmd(ctl, NULL); /* 停车期间的新指令一律丢弃 */

		m = axis_take_motion(&ctl->axes[ctl->move.axis]);
		if (m == AXIS_MOTION_DONE) {
			HSM_TRAN(&st_holding);
		}
		if (m == AXIS_MOTION_FAILED) {
			ctl->fault_code = CATS_FAULT_STOP_FAILED;
		}
		return HSM_RET_SUPER; /* 公共判定交 ONLINE */
	}

	case HSM_SIG_EXIT:
		break;

	default:
		return HSM_RET_SUPER;
	}

	return HSM_RET_HANDLED;
}
