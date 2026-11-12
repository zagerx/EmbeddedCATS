/*
 * state_moving.c — 下发一批 PP 运动（可多轴）并等待全部到位
 *
 * 同一批里的轴一起启动，任何一轴失败即整体失败；全部到位才回 HOLDING。
 * 运动期间的新位置指令被丢弃，STOP 迁移到 HALTING。
 *
 * Author: zager
 */

#include "states.h"

/* 把本批请求逐轴下发；有轴被拒就返回 -1（由 ENTRY 置故障） */
static int issue_move(struct controller *ctl);

/**
 * st_moving - 处理运动发起、停止请求和运动结论
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: 全部到位迁 HOLDING；停止迁 HALTING；失败交由 ROOT 处理。
 */
sm_ret_t st_moving(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct controller *ctl = container_of(me, struct controller, hsm);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		ctl->move_done = 0u;
		if (issue_move(ctl) < 0) {
			ctl->fault_code = CATS_FAULT_MOTION_FAILED;
		}
		break;

	case CATS_SIG_MOVE:
		/* 运动中的新目标：驱动层不接受改目标，本段跑完再说 ⇒ 丢弃 */
		return SM_HANDLED();

	case CATS_SIG_STOP:
		return SM_TRAN(&st_halting); /* Halt 动作在 HALTING 的 ENTRY */

	case CATS_SIG_TICK: {
		const uint8_t all = (uint8_t)((1u << ctl->move.count) - 1u);
		uint8_t i;

		for (i = 0u; i < ctl->move.count; i++) {
			if ((ctl->move_done & (uint8_t)(1u << i)) != 0u) {
				continue; /* 这一轴已到位，结论已经取走 */
			}
			switch (axis_take_motion(&ctl->axes[ctl->move.axis[i].axis])) {
			case AXIS_MOTION_FAILED:
				ctl->fault_code = CATS_FAULT_MOTION_FAILED;
				return SM_SUPER(&st_online);
			case AXIS_MOTION_DONE:
				ctl->move_done |= (uint8_t)(1u << i);
				break;
			default:
				break;
			}
		}
		/* count 最多 CATS_MAX_AXES(2) 条，位图每一位对应一条指令 */
		if (ctl->move_done == all) {
			return SM_TRAN(&st_holding);
		}
		return SM_SUPER(&st_online);
	}

	case SM_SIG_EXIT:
		break;

	default:
		return SM_SUPER(&st_online);
	}

	return SM_HANDLED();
}

static int issue_move(struct controller *ctl)
{
	uint8_t i;

	if (ctl->move.count == 0u || ctl->move.count > CATS_MAX_AXES) {
		return -1;
	}
	for (i = 0u; i < ctl->move.count; i++) {
		const struct stm_move *m = &ctl->move.axis[i];

		if (m->axis >= ctl->axis_count) {
			return -1;
		}
		/* 非 0（拒绝或忙）都算失败：一批要么一起启动，要么整体报错 */
		if (axis_move_to(&ctl->axes[m->axis], m->mm, m->mm_s, m->mm_s2) != 0) {
			return -1;
		}
	}
	return 0;
}
