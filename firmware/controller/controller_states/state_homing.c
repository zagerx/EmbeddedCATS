/*
 * state_homing.c — 回原点：下发回零请求并等参与轴给结论
 *
 * 回零由驱动器自己跑（搜原点开关、减速反向、开关释放处标零），主站只负责
 * 下发参数、等待、汇总回执。
 *
 * 回零期间是**原子的**：只响应急停（由 ROOT 迁 ESTOP）和故障（由 ROOT 迁
 * FAULT），暂停与位置指令一律丢弃。半个回零流程比走完更危险——原点没标定，
 * 上层却以为标了。要中途停只能按急停。
 *
 * Author: zager
 */

#include "states.h"
#include "controller_log.h"

/**
 * st_homing - 处理回零期间的节拍与结论
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: 参与轴全部给出成功回执 → HOLDING；失败 → 锁故障交给 ROOT。
 */
sm_ret_t st_homing(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct controller *ctl = container_of(me, struct controller, hsm);

	switch (e->sig) {
	case SM_SIG_ENTRY: {
		uint8_t i;

		ctl->axis_done = 0u;
		ctl->axis_active = 0u;
		for (i = 0u; i < ctl->axis_count; i++) {
			if ((ctl->home_mask & (uint8_t)(1u << i)) == 0u) {
				continue;
			}
			if (axis_home(&ctl->axes[i]) != 0) {
				/*
				 * 设备忙：这轴没执行，从本次参与集合中移除并记日志。
				 * 不写完成位，未执行不是任何意义上的执行结果。
				 */
				ctl->home_mask &= (uint8_t) ~(1u << i);
				controller_log_not_executed(ctl, CONTROLLER_SIG_HOME, i,
							    CONTROLLER_NOT_EXECUTED_DEVICE_BUSY);
			} else {
				ctl->axis_active |= (uint8_t)(1u << i);
			}
		}
		/*
		 * ENTRY 里不迁移（内核丢弃 ENTRY 的返回值）：一个都没起来时
		 * home_mask 为 0，下一拍 TICK 的完成判据自然成立。
		 */
		break;
	}

	case CONTROLLER_SIG_MOVE:
	case CONTROLLER_SIG_HALT:
		/* 回零是原子流程；新请求不执行，只说明原因，不生成故障 */
		controller_log_not_executed(ctl, (enum controller_sig)e->sig,
					    CONTROLLER_FAULT_NO_AXIS,
					    CONTROLLER_NOT_EXECUTED_INVALID_STATE);
		return SM_HANDLED();

	case CONTROLLER_SIG_TICK: {
		struct controller_axes_failure failure;
		enum controller_axes_result result = controller_axes_collect(
			ctl->axes, ctl->axis_count, &ctl->axis_active, &failure);

		if (result == CONTROLLER_AXES_FAILED) {
			controller_set_axes_fault(ctl, CONTROLLER_STATE_HOMING, &failure);
			return SM_SUPER(&st_root);
		}
		if (ctl->axis_active == 0u) {
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
