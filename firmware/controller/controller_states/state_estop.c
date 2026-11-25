/*
 * state_estop.c — 抢占在途流程并保持急停终态
 *
 * 入口只向每个轴写一次 quick stop 控制字；驱动器按 605A = 2 自行减速、
 * 断使能并抱闸。本层不等减速过程、不补断使能。写失败只锁存故障，不重试。
 *
 * 解除急停也不碰驱动器：断电在急停那一帧就完成了，这里只收齐回执并回 IDLE。
 * 回执不交给 IDLE 收——那条回执没有流程身份，空闲态拿到会把“解除”误当成
 * “使能成功”。
 *
 * Author: zager
 */

#include "states.h"
#include "controller_log.h"

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
	case SM_SIG_ENTRY: {
		uint8_t i;

		ctl->axis_done = 0u;
		ctl->estop_clearing = 0u;
		for (i = 0u; i < ctl->axis_count; i++) {
			if (axis_quick_stop(&ctl->axes[i]) == 0) {
				ctl->axis_done |= (uint8_t)(1u << i);
			} else {
				controller_log_not_executed(ctl, CONTROLLER_SIG_ESTOP, i,
							    CONTROLLER_NOT_EXECUTED_DEVICE_BUSY);
			}
		}
		/* 急停覆盖旧的普通命令；日志只记未执行，不新增故障 */
		controller_log_queue_cleared(ctl);
		break;
	}

	case SM_SIG_EXIT:
		break;

	case CONTROLLER_SIG_MOVE:
	case CONTROLLER_SIG_HALT:
	case CONTROLLER_SIG_ENABLE:
	case CONTROLLER_SIG_HOME:
		controller_log_not_executed(ctl, (enum controller_sig)e->sig,
					    CONTROLLER_FAULT_NO_AXIS,
					    CONTROLLER_NOT_EXECUTED_TERMINAL);
		return SM_HANDLED();

	case CONTROLLER_SIG_ESTOP_CLEAR: {
		uint8_t i;

		/*
		 * 解除急停：断使能与抱闸已在急停那一帧由驱动器完成，这里不写任何控制字，
		 * 只释放流程。故障快照不碰：急停只解除覆盖层，底下压着的故障仍然锁存。
		 * 本状态自己等回执，收齐后才迁 IDLE（有故障时 ROOT 随后把它带进 FAULT）。
		 */
		ctl->move = (struct controller_move_cmd){0};
		ctl->move_done = 0u;
		ctl->axis_done = 0u;
		ctl->estop_clearing = 1u;
		for (i = 0u; i < ctl->axis_count; i++) {
			if (axis_clear_estop(&ctl->axes[i]) == 0) {
				ctl->axis_done |= (uint8_t)(1u << i);
			} else {
				controller_log_not_executed(ctl, CONTROLLER_SIG_ESTOP_CLEAR, i,
							    CONTROLLER_NOT_EXECUTED_DEVICE_BUSY);
			}
		}
		return SM_HANDLED();
	}

	case CONTROLLER_SIG_TICK:
		/*
		 * 终态吃掉节拍：不能让 ROOT 的“有故障 → FAULT”规则把 ESTOP 又拉回 FAULT
		 * （进入急停前可能已经锁存过故障）。
		 * 回执在这里收：急停那一笔只用于上报，解除那一笔收齐后才迁 IDLE。
		 */
		if (ctl->axis_done != 0u) {
			struct controller_axes_failure failure;

			if (controller_axes_collect(ctl->axes, ctl->axis_count, &ctl->axis_done,
						    &failure) == CONTROLLER_AXES_FAILED) {
				controller_set_axes_fault(ctl, CONTROLLER_STATE_ESTOP, &failure);
				ctl->axis_done = controller_axis_mask(ctl->axis_count);
			}
		}
		if (ctl->estop_clearing != 0u && ctl->axis_done == 0u) {
			ctl->estop_clearing = 0u;
			return SM_TRAN(&st_idle_ctrl);
		}
		return SM_HANDLED();

	default:
		return SM_SUPER(&st_root);
	}

	return SM_HANDLED();
}
