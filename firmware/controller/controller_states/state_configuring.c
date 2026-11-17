/*
 * state_configuring.c — 广播 NMT Start、请求各轴进入 PP 模式并等待配置结果
 *
 * 全部配置完成后进入 IDLE（已配置、未使能），等外部使能指令；
 * 失败或超时由 ROOT 统一迁移到 FAULT。
 *
 * Author: zager
 */

#include "states.h"

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
	case SM_SIG_ENTRY:
		/* 先让节点进入运行态（广播所有节点），再写参数。
		 * NMT 没有应答：对象访问失败或超时由配置流程给出结论。 */
		ctl->axis_done = 0u;
		(void)master_request_nmt(ctl->master, MASTER_NMT_ALL_NODES, MASTER_NMT_START);
		(void)controller_config_movemode(ctl->axes, ctl->axis_count, CONTROLLER_AXIS_ALL);
		break;

	case SM_SIG_EXIT:
		break; /* 失败的那一步已经停下，正常完成时也不再发帧 */

	case CONTROLLER_SIG_TICK: {
		uint32_t elapsed = controller_now_ms(ctl) - ctl->state_since_ms;
		struct controller_axes_failure failure;
		enum controller_axes_result result = controller_axes_collect(
			ctl->axes, ctl->axis_count, &ctl->axis_done, &failure);

		if (result == CONTROLLER_AXES_DONE) {
			return SM_TRAN(&st_idle_ctrl); /* 配置完成：停在未使能态等指令 */
		}
		if (result == CONTROLLER_AXES_FAILED) {
			controller_set_fault(ctl, CONTROLLER_STATE_CONFIGURING,
					     controller_fault_reason_of(failure.first.fail),
					     failure.first_axis, failure.first.abort_code);
		} else if (elapsed > CONFIG_TIMEOUT_MS) {
			controller_set_fault(ctl, CONTROLLER_STATE_CONFIGURING,
					     CONTROLLER_FAULT_REASON_BIZ_TIMEOUT,
					     CONTROLLER_FAULT_NO_AXIS, 0u);
		}
		return SM_SUPER(&st_root);
	}

	default:
		return SM_SUPER(&st_root);
	}

	return SM_HANDLED();
}
