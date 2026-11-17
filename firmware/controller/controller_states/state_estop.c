/*
 * state_estop.c — 抢占在途流程并保持急停终态
 *
 * 入口只向每个轴写一次 quick stop 控制字；驱动器自己按 605A 减速停机，
 * 本层不等减速过程、不断使能。写失败只锁存故障，不重试。
 *
 * 解除急停也由本状态自己完成：发一次 0x00 断使能，等回执齐了再迁 IDLE。
 * 回执不交给 IDLE 收——那条回执没有流程身份，空闲态拿到会把“解除”误当成
 * “使能成功”。
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
		ctl->estop_clearing = 0u;
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
		 * 解除急停：驱动器断使能回 Switch on disabled。故障快照不碰：
		 * 急停只解除覆盖层，底下压着的故障仍然锁存。
		 * 本状态自己等回执，收齐后才迁 IDLE（有故障时 ROOT 随后把它带进 FAULT）。
		 */
		ctl->move = (struct controller_move_cmd){0};
		ctl->move_done = 0u;
		ctl->axis_done = 0u; /* 重新开始收：“解除那一笔”的回执 */
		ctl->estop_clearing = 1u;
		(void)controller_clear_estop_all(ctl->axes, ctl->axis_count);
		return SM_HANDLED();

	case CONTROLLER_SIG_TICK:
		/*
		 * 终态吃掉节拍：不能让 ROOT 的“有故障 → FAULT”规则把 ESTOP 又拉回 FAULT
		 * （进入急停前可能已经锁存过故障）。
		 * 回执在这里收：急停那一笔只用于上报，解除那一笔收齐后才迁 IDLE。
		 */
		if (ctl->axis_done != controller_axis_mask(ctl->axis_count)) {
			struct controller_axes_failure failure;

			if (controller_axes_collect(ctl->axes, ctl->axis_count, &ctl->axis_done,
						    &failure) == CONTROLLER_AXES_FAILED) {
				controller_set_fault(ctl, CONTROLLER_STATE_ESTOP,
						     controller_fault_reason_of(failure.first.fail),
						     failure.first_axis, failure.first.abort_code);
				ctl->axis_done = controller_axis_mask(ctl->axis_count);
			}
		}
		if (ctl->estop_clearing != 0u &&
		    ctl->axis_done == controller_axis_mask(ctl->axis_count)) {
			ctl->estop_clearing = 0u;
			return SM_TRAN(&st_idle_ctrl);
		}
		return SM_HANDLED();

	default:
		return SM_SUPER(&st_root);
	}

	return SM_HANDLED();
}
