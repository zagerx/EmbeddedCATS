/*
 * state_estop.c — 抢占在途流程并保持急停终态
 *
 * 入口只向每个轴写一次 quick stop 控制字；驱动器自己按 605A 减速停机，
 * 本层不等减速过程、不断使能。写失败只上报故障码，不重试。
 * 终态只接受一条命令：解除急停（驱动器回可操作，控制器回 IDLE）。
 *
 * Author: zager
 */

#include "states.h"

/**
 * st_estop - 处理急停入口、运行命令和节拍
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: MOVE、HALT 和 TICK 被消费；解除急停迁 IDLE；其他事件上抛 ROOT。
 */
sm_ret_t st_estop(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct controller *ctl = container_of(me, struct controller, hsm);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		ctl->axis_done = 0u;
		(void)controller_quick_stop_all(ctl->axes, ctl->axis_count);
		/* 进终态：丢掉还没处理的命令 */
		controller_evtq_clear(&ctl->evtq);
		break;

	case SM_SIG_EXIT:
		break;

	case CONTROLLER_SIG_MOVE:
	case CONTROLLER_SIG_HALT:
		return SM_HANDLED(); /* 终态：命令直接丢弃 */

	case CONTROLLER_SIG_ESTOP_CLEAR:
		/*
		 * 解除急停：驱动器被写 0x0F 回到可操作，控制器回 IDLE 等新的使能。
		 * 故障码与旧运动批次必须清掉，否则 IDLE 的节拍会立刻把它拉回 FAULT。
		 */
		ctl->fault_code = CONTROLLER_FAULT_NONE;
		ctl->move = (struct controller_move_cmd){0};
		ctl->move_done = 0u;
		ctl->axis_done = 0u;
		(void)controller_clear_estop_all(ctl->axes, ctl->axis_count);
		return SM_TRAN(&st_idle_ctrl);

	case CONTROLLER_SIG_TICK:
		/*
		 * 终态吃掉节拍：不能让 ROOT 的“故障码 → FAULT”规则把 ESTOP 又拉回 FAULT
		 * （进入急停前可能已经置过故障码）。
		 * 回执只收一次用途：把“那一笔 quick stop 没送到”报成故障码。
		 */
		if (ctl->axis_done != controller_axis_mask(ctl->axis_count)) {
			enum controller_axes_result r = controller_axes_collect(
				ctl->axes, ctl->axis_count, &ctl->axis_done);

			if (r == CONTROLLER_AXES_FAILED) {
				ctl->fault_code = CONTROLLER_FAULT_ESTOP_FAILED;
				ctl->axis_done = controller_axis_mask(ctl->axis_count);
			}
		}
		return SM_HANDLED();

	default:
		return SM_SUPER(&st_root);
	}

	return SM_HANDLED();
}
