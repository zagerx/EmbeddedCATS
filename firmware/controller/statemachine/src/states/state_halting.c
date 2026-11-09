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

sm_ret_t st_halting(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct stm *ctl = container_of(me, struct stm, hsm);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		if (axis_halt(&ctl->axes[ctl->move.axis]) != 0) {
			ctl->fault_code = CATS_FAULT_STOP_FAILED; /* ENTRY 里不迁移，交 ROOT */
		}
		break;

	case CATS_SIG_MOVE:
	case CATS_SIG_STOP:
		/* 停车期间的新指令一律丢弃（否则会留在队列里，停稳后被意外执行） */
		return SM_HANDLED();

	case CATS_SIG_TICK: {
		enum axis_motion m;

		m = axis_take_motion(&ctl->axes[ctl->move.axis]);
		if (m == AXIS_MOTION_DONE) {
			return SM_TRAN(&st_holding);
		}
		if (m == AXIS_MOTION_FAILED) {
			ctl->fault_code = CATS_FAULT_STOP_FAILED;
		}
		return SM_SUPER(&st_online); /* 公共判定交 ONLINE */
	}

	case SM_SIG_EXIT:
		break;

	default:
		return SM_SUPER(&st_online);
	}

	return SM_HANDLED();
}
