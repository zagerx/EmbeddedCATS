/*
 * state_enabling.c — 请求各轴使能并等待完成
 *
 * 全部使能完成后进入 HOLDING；失败或超时由 ROOT 统一迁移到 FAULT。
 *
 * Author: zager
 */

#include "states.h"

#define ENABLE_TIMEOUT_MS 3000u /* 从进入本状态算起，这么久还没全部使能上就判失败 */

/**
 * st_enabling - 驱动各轴使能并监视完成、失败和超时
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: 全部使能迁移 HOLDING；其他结果按状态层次处理或上抛。
 */
sm_ret_t st_enabling(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct controller *ctl = container_of(me, struct controller, hsm);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		/* 节点已在 CONFIGURING 里进入运行态，这里只下发使能请求 */
		ctl->axis_done = 0u;
		(void)controller_enable_all(ctl);
		break;

	case SM_SIG_EXIT:
		break; /* 失败的那一步已经停下，正常完成时也不再发帧 */

	case CONTROLLER_SIG_TICK: {
		uint32_t elapsed = controller_now_ms(ctl) - ctl->state_since_ms;
		uint8_t all = controller_axis_mask(ctl);
		uint8_t i;

		for (i = 0u; i < ctl->axis_count; i++) {
			if ((ctl->axis_done & (uint8_t)(1u << i)) != 0u) {
				continue; /* 这一轴已确认使能 */
			}
			switch (axis_take_motion(&ctl->axes[i])) {
			case AXIS_MOTION_FAILED:
				ctl->fault_code = CONTROLLER_FAULT_ENABLE_FAILED;
				return SM_SUPER(&st_online); /* 交 ROOT 迁 FAULT */
			case AXIS_MOTION_DONE:
				ctl->axis_done |= (uint8_t)(1u << i);
				break;
			default:
				break;
			}
		}
		if (all != 0u && ctl->axis_done == all) {
			return SM_TRAN(&st_holding);
		}
		if (elapsed > ENABLE_TIMEOUT_MS) {
			ctl->fault_code = CONTROLLER_FAULT_ENABLE_TIMEOUT;
		}
		return SM_SUPER(&st_online);
	}

	default:
		return SM_SUPER(&st_online);
	}

	return SM_HANDLED();
}
