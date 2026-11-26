/*
 * state_halting.c — 通过 6040 bit8 暂停并保持使能
 *
 * 暂停期间忽略 MOVE 和 HALT，完成后迁移到 HOLDING。
 *
 * Author: zager
 */

#include "states.h"
#include "controller_log.h"

/**
 * st_halting - 处理暂停发起与暂停结论
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: 停稳迁移 HOLDING；暂停期间命令被消费；失败交由 ROOT 处理。
 */
sm_ret_t st_halting(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct controller *ctl = container_of(me, struct controller, hsm);

	switch (e->sig) {
	case SM_SIG_ENTRY: {
		uint8_t i;

		ctl->move_done = 0u;
		ctl->axis_active = 0u;
		for (i = 0u; i < ctl->move.count; i++) {
			const uint8_t axis = ctl->move.axis[i].axis;

			/* 未受理就没有暂停动作，不参与停稳等待 */
			if (axis_halt(&ctl->axes[axis]) != 0) {
				controller_log_not_executed(ctl, CONTROLLER_SIG_HALT, axis,
							    CONTROLLER_NOT_EXECUTED_DEVICE_BUSY);
				ctl->move_done |= (uint8_t)(1u << i);
			} else {
				ctl->axis_active |= (uint8_t)(1u << i);
			}
		}
		break;
	}

	case CONTROLLER_SIG_MOVE:
	case CONTROLLER_SIG_HALT:
		controller_log_not_executed(ctl, (enum controller_sig)e->sig,
					    CONTROLLER_FAULT_NO_AXIS,
					    CONTROLLER_NOT_EXECUTED_INVALID_STATE);
		return SM_HANDLED();

	case CONTROLLER_SIG_TICK: {
		const uint8_t all = (uint8_t)((1u << ctl->move.count) - 1u);
		uint8_t i;

		for (i = 0u; i < ctl->move.count; i++) {
			const uint8_t axis = ctl->move.axis[i].axis;
			struct axis_failure failure;
			enum axis_motion m;

			if ((ctl->move_done & (uint8_t)(1u << i)) != 0u) {
				continue; /* 这一轴已停稳 */
			}
			m = axis_take_motion(&ctl->axes[axis], &failure);
			if (m == AXIS_MOTION_FAILED) {
				/* 停不下来属安全事件：不区分归类，一律锁存并进 FAULT */
				controller_set_fault(ctl, CONTROLLER_STATE_HALTING,
						     controller_fault_reason_of(failure.fail),
						     axis);
				ctl->move_done |= (uint8_t)(1u << i);
			} else if (m == AXIS_MOTION_DONE || !axis_is_running(&ctl->axes[axis])) {
				ctl->move_done |= (uint8_t)(1u << i);
			}
		}
		if (ctl->move_done == all) {
			return SM_TRAN(&st_holding);
		}
		return SM_SUPER(&st_root);
	}

	case SM_SIG_EXIT:
		break;

	default:
		return SM_SUPER(&st_root);
	}

	return SM_HANDLED();
}
