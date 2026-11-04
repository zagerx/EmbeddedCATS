/*
 * state_configuring.c — 配置中
 *
 * 进入：请求各轴写参数（模式 = PP 等，并发，互不阻塞）。
 * 每拍：只看两个事实——都配好了 → ENABLING；有轴失败 / 超时 → 置故障码，
 *       由 ROOT 统一迁 FAULT。
 *
 * 本状态不推进任何步骤：轴的推进在 axis_poll_all() 里（control.c 每拍调一次），
 * 这里只读 axis_all_configured() / axis_any_failed() 的结论。
 *
 * 位置：排在 PREPARING（等心跳）之后、ENABLING（NMT 启动 + 使能）之前。
 * 此刻节点还在 pre-operational，SDO 可用而 PDO 未开，是改参数最安全的时机；
 * 也使 6060 的写入满足"驱动器未使能才能改模式"这条 CiA 402 约束。
 */

#include "states.h"

#define CONFIG_TIMEOUT_MS 2000u /* 从进入本状态算起，这么久还没配完就判失败 */

static int st_configuring_h(struct hsm *me, const struct hsm_event *e);
const struct hsm_state st_configuring = {
	.handler = st_configuring_h,
	.super = &st_online, /* 此时心跳要求已经成立，掉了就该报心跳丢失 */
	.name = "CONFIGURING",
};

static int st_configuring_h(struct hsm *me, const struct hsm_event *e)
{
	struct cats_control *ctl = container_of(me, struct cats_control, hsm);

	switch (e->sig) {
	case HSM_SIG_ENTRY:
		(void)axis_config_all(ctl->axes, ctl->axis_count);
		break;

	case HSM_SIG_EXIT:
		break; /* 失败的那一步已经停下，正常完成时也不再发帧 */

	case CATS_SIG_TICK: {
		uint32_t elapsed = now_ms(ctl) - me->entered_ms;

		if (axis_any_failed(ctl->axes, ctl->axis_count)) {
			ctl->fault_code = CATS_FAULT_CONFIG_FAILED;
			return HSM_RET_SUPER; /* 由 ROOT 统一迁 FAULT */
		}
		if (axis_all_configured(ctl->axes, ctl->axis_count)) {
			HSM_TRAN(&st_enabling);
		}
		if (elapsed > CONFIG_TIMEOUT_MS) {
			ctl->fault_code = CATS_FAULT_CONFIG_TIMEOUT;
		}
		return HSM_RET_SUPER;
	}

	default:
		return HSM_RET_SUPER;
	}

	return HSM_RET_HANDLED;
}
