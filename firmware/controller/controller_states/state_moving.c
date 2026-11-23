/*
 * state_moving.c — 下发一批 PP 运动（可多轴）并等待全部到位
 *
 * 同一批里的轴一起启动，任何一轴失败即整体失败；全部到位才回 HOLDING。
 * 运动期间的新位置指令算重规划：仍在跑的轴由驱动器在线换目标（不停车也不
 * 重复起步），新批次里撤掉的轴减速停下并保持使能，没在跑的轴重新起步。
 * 暂停迁移到 HALTING。
 *
 * Author: zager
 */

#include "states.h"

/* 把本批请求逐轴下发；有轴被拒就返回 -1，并通过 first_axis 带回轴号 */
static int issue_move(struct controller *ctl, uint8_t *first_axis);
/* 这个轴在不在这批请求里 */
static int batch_has(const struct controller_move_cmd *batch, uint8_t axis);

/**
 * st_moving - 处理运动发起、重规划、暂停请求和运动结论
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: 全部到位迁 HOLDING；暂停迁 HALTING；重规划就地换目标，不迁移；
 *         失败交由 ROOT 处理。
 */
sm_ret_t st_moving(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct controller *ctl = container_of(me, struct controller, hsm);

	switch (e->sig) {
	case SM_SIG_ENTRY: {
		uint8_t axis = CONTROLLER_FAULT_NO_AXIS;

		ctl->move_done = 0u;
		if (issue_move(ctl, &axis) < 0) {
			/* 设备没受理（忙或轴号不合法）：本批没有启动任何轴 */
			controller_set_fault(ctl, CONTROLLER_STATE_MOVING,
					     CONTROLLER_FAULT_REASON_REFUSED, axis, 0u);
		}
		break;
	}

	case CONTROLLER_SIG_MOVE: {
		const struct controller_move_cmd *next =
			&SM_EVT_CAST(struct controller_evt, e)->u.move;
		uint8_t axis = CONTROLLER_FAULT_NO_AXIS;
		uint8_t i;

		/*
		 * 旧结论先取走：已经到位的轴若留着 DONE，新批次会在它还没走到新目标
		 * 时就误判为整批完成。
		 */
		for (i = 0u; i < ctl->axis_count; i++) {
			(void)axis_take_motion(&ctl->axes[i], NULL);
		}
		/* 新批次里撤掉的轴：减速停、保持使能，不能让它留在那儿自己跑 */
		for (i = 0u; i < ctl->move.count; i++) {
			const uint8_t a = ctl->move.axis[i].axis;

			if (batch_has(next, a) == 0 && axis_halt(&ctl->axes[a]) != 0) {
				controller_set_fault(ctl, CONTROLLER_STATE_MOVING,
						     CONTROLLER_FAULT_REASON_REFUSED, a, 0u);
			}
		}
		ctl->move = *next;
		ctl->move_done = 0u;
		if (issue_move(ctl, &axis) < 0) {
			controller_set_fault(ctl, CONTROLLER_STATE_MOVING,
					     CONTROLLER_FAULT_REASON_REFUSED, axis, 0u);
		}
		return SM_HANDLED();
	}

	case CONTROLLER_SIG_HALT:
		return SM_TRAN(&st_halting); /* Halt 动作在 HALTING 的 ENTRY */

	case CONTROLLER_SIG_TICK: {
		const uint8_t all = (uint8_t)((1u << ctl->move.count) - 1u);
		uint8_t i;

		for (i = 0u; i < ctl->move.count; i++) {
			const uint8_t axis = ctl->move.axis[i].axis;
			struct axis_failure failure;

			if ((ctl->move_done & (uint8_t)(1u << i)) != 0u) {
				continue; /* 这一轴已到位，结论已经取走 */
			}
			switch (axis_take_motion(&ctl->axes[axis], &failure)) {
			case AXIS_MOTION_FAILED:
				controller_set_fault(ctl, CONTROLLER_STATE_MOVING,
						     controller_fault_reason_of(failure.fail), axis,
						     failure.abort_code);
				return SM_SUPER(&st_root);
			case AXIS_MOTION_DONE:
				ctl->move_done |= (uint8_t)(1u << i);
				break;
			default:
				break;
			}
		}
		/* count 最多 CONTROLLER_AXIS_COUNT(2) 条，位图每一位对应一条指令 */
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

static int batch_has(const struct controller_move_cmd *batch, uint8_t axis)
{
	uint8_t i;

	for (i = 0u; i < batch->count; i++) {
		if (batch->axis[i].axis == axis) {
			return 1;
		}
	}
	return 0;
}

static int issue_move(struct controller *ctl, uint8_t *first_axis)
{
	uint8_t i;

	if (ctl->move.count == 0u || ctl->move.count > CONTROLLER_AXIS_COUNT) {
		return -1;
	}
	for (i = 0u; i < ctl->move.count; i++) {
		const struct controller_move_req *m = &ctl->move.axis[i];

		if (m->axis >= ctl->axis_count) {
			return -1;
		}
		/* 非 0（拒绝或忙）都算失败：一批要么一起启动，要么整体报错 */
		if (axis_move_to(&ctl->axes[m->axis], m->mm, m->mm_s, m->mm_s2) != 0) {
			if (first_axis != NULL) {
				*first_axis = m->axis;
			}
			return -1;
		}
	}
	return 0;
}
