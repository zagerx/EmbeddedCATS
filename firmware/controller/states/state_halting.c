/*
 * state_halting.c — 通过 6040 bit8 减速停车并保持使能
 *
 * 停车期间忽略 MOVE 和 STOP，完成后迁移到 HOLDING。
 *
 * Author: zager
 */

#include "states.h"

/**
 * st_halting - 处理减速停发起与停车结论
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: 停稳迁移 HOLDING；停车期间命令被消费；失败交由 ROOT 处理。
 */
sm_ret_t st_halting(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct controller *ctl = container_of(me, struct controller, hsm);

	switch (e->sig) {
	case SM_SIG_ENTRY: {
		uint8_t i;

		ctl->move_done = 0u;
		for (i = 0u; i < ctl->move.count; i++) {
			/* ENTRY 里不迁移，失败交给 ROOT */
			if (axis_halt(&ctl->axes[ctl->move.axis[i].axis]) != 0) {
				ctl->fault_code = CONTROLLER_FAULT_STOP_FAILED;
			}
		}
		break;
	}

	case CONTROLLER_SIG_MOVE:
	case CONTROLLER_SIG_STOP:
		/* 停车期间的新指令一律丢弃（否则会留在队列里，停稳后被意外执行） */
		return SM_HANDLED();

	case CONTROLLER_SIG_TICK: {
		const uint8_t all = (uint8_t)((1u << ctl->move.count) - 1u);
		uint8_t i;

		for (i = 0u; i < ctl->move.count; i++) {
			enum axis_motion m;

			if ((ctl->move_done & (uint8_t)(1u << i)) != 0u) {
				continue; /* 这一轴已停稳 */
			}
			m = axis_take_motion(&ctl->axes[ctl->move.axis[i].axis]);
			if (m == AXIS_MOTION_FAILED) {
				ctl->fault_code = CONTROLLER_FAULT_STOP_FAILED;
			} else if (m == AXIS_MOTION_DONE) {
				ctl->move_done |= (uint8_t)(1u << i);
			}
		}
		if (ctl->move_done == all) {
			return SM_TRAN(&st_holding);
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
