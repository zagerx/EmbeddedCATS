/*
 * 减速停：通过 6040 bit8 停车并保持使能。
 * 停车期间忽略 MOVE/STOP，完成后转 HOLDING。
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
