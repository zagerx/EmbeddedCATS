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
		for (i = 0u; i < ctl->axis_count; i++) {
			if ((ctl->home_mask & (uint8_t)(1u << i)) == 0u) {
				continue;
			}
			if (axis_home(&ctl->axes[i]) != 0) {
				/*
				 * 设备忙（上一笔流程还没收尾）：这轴没起来，把它从
				 * 本次委托里摘掉。不锁故障——没下发就没动作，按未受理
				 * 处理比按故障处理更安全，重新点一次即可。
				 */
				ctl->home_mask &= (uint8_t) ~(1u << i);
				ctl->axis_done |= (uint8_t)(1u << i);
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
		/*
		 * 回零期间的暂停与位置指令一律丢弃：回零不能半途停（原点没标定，上层
		 * 却以为标了）。丢在状态里而不是留在队列里，避免回零结束后被意外执行。
		 */
		return SM_HANDLED();

	case CONTROLLER_SIG_TICK: {
		struct controller_axes_failure failure;
		enum controller_axes_result result = controller_axes_collect(
			ctl->axes, ctl->axis_count, &ctl->axis_done, &failure);

		if (result == CONTROLLER_AXES_FAILED) {
			controller_set_fault(ctl, CONTROLLER_STATE_HOMING,
					     controller_fault_reason_of(failure.first.fail),
					     failure.first_axis, failure.first.abort_code);
			return SM_SUPER(&st_root);
		}
		/*
		 * 判据是"参与的轴都置位"，不能用相等：controller_axes_collect()
		 * 遍历的是全部已装配轴，非参与轴的 DONE 也会进 axis_done（超集），
		 * 相等判据遇到超集永远不成立，只能干等到 402 层的 30 s 超时。
		 * home_mask 为 0（一个都没起来）时两边都是 0，同样成立——
		 * 没下发就没动作，等于没发生。
		 */
		if ((ctl->axis_done & ctl->home_mask) == ctl->home_mask) {
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
