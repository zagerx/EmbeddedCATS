/*
 * 配置中：在 pre-operational 状态请求各轴进入 PP 模式。
 * 全部完成则转 ENABLING；失败或超时由 ROOT 转 FAULT。
 */

#include "states.h"

#define CONFIG_TIMEOUT_MS 2000u /* 从进入本状态算起，这么久还没配完就判失败 */

sm_ret_t st_configuring(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct stm *ctl = container_of(me, struct stm, hsm);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		(void)axis_config_all(ctl->axes, ctl->axis_count);
		break;

	case SM_SIG_EXIT:
		break; /* 失败的那一步已经停下，正常完成时也不再发帧 */

	case CATS_SIG_TICK: {
		uint32_t elapsed = stm_now_ms(ctl) - ctl->state_since_ms;

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
