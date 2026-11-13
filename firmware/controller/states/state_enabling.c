/*
 * state_enabling.c — 广播 NMT Start 并等待所有轴使能
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
		/* 先让节点进入运行态（广播所有节点）。手册例子也是先 NMT 再 SDO。
		 * 工作线程直接发 NMT；命令没有应答，发不出去或从站没收到，
		 * 都由后面“心跳上来 + 使能成功”来判定，判不出来也会落 ENABLE_TIMEOUT。 */
		controller_nmt_start(ctl);
		(void)axis_enable_all(ctl->axes, ctl->axis_count);
		break;

	case SM_SIG_EXIT:
		break; /* 失败的那一步已经停下，正常完成时也不再发帧 */

	case CONTROLLER_SIG_TICK: {
		uint32_t elapsed = controller_now_ms(ctl) - ctl->state_since_ms;

		if (axis_any_failed(ctl->axes, ctl->axis_count)) {
			ctl->fault_code = CONTROLLER_FAULT_ENABLE_FAILED;
			return SM_SUPER(&st_online); /* 交 ROOT 迁 FAULT */
		}
		if (axis_all_enabled(ctl->axes, ctl->axis_count)) {
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
