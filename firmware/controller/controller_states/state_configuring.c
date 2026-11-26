/*
 * state_configuring.c — 广播 NMT Start、请求各轴进入 PP 模式并等待配置结果
 *
 * 全部配置完成后进入 IDLE（已配置、未使能），等外部使能指令；
 * 失败或超时由 ROOT 统一迁移到 FAULT。
 *
 * Author: zager
 */

#include "states.h"
#include "controller_log.h"

#define CONFIG_TIMEOUT_MS 2000u /* 从进入本状态算起，这么久还没配完就判失败 */

/**
 * st_configuring - 驱动各轴配置并监视完成、失败和超时
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: 配置完成迁移 IDLE；其他结果按状态层次处理或上抛。
 */
sm_ret_t st_configuring(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct controller *ctl = container_of(me, struct controller, hsm);

	switch (e->sig) {
	case SM_SIG_ENTRY: {
		uint8_t i;

		/* NMT 无应答；只有实际受理的配置请求才等待结果 */
		ctl->axis_done = 0u;
		ctl->axis_active = 0u;
		(void)master_request_nmt(ctl->master, MASTER_NMT_ALL_NODES, MASTER_NMT_START);
		for (i = 0u; i < ctl->axis_count; i++) {
			if (axis_config_movemode(&ctl->axes[i]) == 0) {
				ctl->axis_active |= (uint8_t)(1u << i);
			} else {
				controller_log_not_executed(ctl, CONTROLLER_SIG_TICK, i,
							    CONTROLLER_NOT_EXECUTED_DEVICE_BUSY);
			}
		}
		break;
	}

	case SM_SIG_EXIT:
		break; /* 失败的那一步已经停下，正常完成时也不再发帧 */

	case CONTROLLER_SIG_TICK: {
		uint32_t elapsed = controller_now_ms(ctl) - ctl->state_since_ms;
		struct controller_axes_failure failure;
		enum controller_axes_result result = controller_axes_collect(
			ctl->axes, ctl->axis_count, &ctl->axis_active, &failure);

		if (result == CONTROLLER_AXES_FAILED) {
			if (controller_set_axes_fault(ctl, CONTROLLER_STATE_CONFIGURING,
						      &failure) != 0) {
				return SM_SUPER(&st_root);
			}
			return SM_TRAN(&st_idle_ctrl); /* 配置没成功：不进 FAULT，回空闲重试 */
		}
		if (ctl->axis_active == 0u) {
			return SM_TRAN(&st_idle_ctrl);
		}
		if (elapsed > CONFIG_TIMEOUT_MS) {
			controller_set_fault(ctl, CONTROLLER_STATE_CONFIGURING,
					     CONTROLLER_FAULT_REASON_BIZ_TIMEOUT,
					     CONTROLLER_FAULT_NO_AXIS);
		}
		return SM_SUPER(&st_root);
	}

	default:
		return SM_SUPER(&st_root);
	}

	return SM_HANDLED();
}
