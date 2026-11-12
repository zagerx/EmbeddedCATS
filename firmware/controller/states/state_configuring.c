/*
 * state_configuring.c — 请求各轴进入 PP 模式并等待配置结果
 *
 * 全部配置完成后进入 ENABLING；失败或超时由 ROOT 统一迁移到 FAULT。
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
 * Return: 配置完成迁移 ENABLING；其他结果按状态层次处理或上抛。
 */
sm_ret_t st_configuring(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct controller *ctl = container_of(me, struct controller, hsm);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		(void)axis_config_all(ctl->axes, ctl->axis_count);
		break;

	case SM_SIG_EXIT:
		break; /* 失败的那一步已经停下，正常完成时也不再发帧 */

	case CATS_SIG_TICK: {
		uint32_t elapsed = ctl->ops.now_ms(ctl->ops.ctx) - ctl->state_since_ms;

		if (axis_any_failed(ctl->axes, ctl->axis_count)) {
			ctl->fault_code = CATS_FAULT_CONFIG_FAILED;
			return SM_SUPER(&st_online); /* 由 ROOT 统一迁 FAULT */
		}
		if (axis_all_configured(ctl->axes, ctl->axis_count)) {
			return SM_TRAN(&st_enabling);
		}
		if (elapsed > CONFIG_TIMEOUT_MS) {
			ctl->fault_code = CATS_FAULT_CONFIG_TIMEOUT;
		}
		return SM_SUPER(&st_online);
	}

	default:
		return SM_SUPER(&st_online);
	}

	return SM_HANDLED();
}
