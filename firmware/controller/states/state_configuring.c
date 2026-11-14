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
		 * NMT 没有应答：发不出去或从站没收到，由后面“配置成功 + 心跳”判定，
		 * 判不出来也会落 CONFIG_TIMEOUT。 */
		(void)master_request_nmt(ctl->master, MASTER_NMT_ALL_NODES, MASTER_NMT_START);
		(void)controller_config_movemode(ctl, AXIS_ALL);
		break;

	case SM_SIG_EXIT:
		break; /* 失败的那一步已经停下，正常完成时也不再发帧 */

	case CONTROLLER_SIG_TICK: {
		uint32_t elapsed = controller_now_ms(ctl) - ctl->state_since_ms;

		if (controller_any_failed(ctl)) {
			ctl->fault_code = CONTROLLER_FAULT_CONFIG_FAILED;
			return SM_SUPER(&st_online); /* 由 ROOT 统一迁 FAULT */
		}
		if (controller_all_configured(ctl)) {
			return SM_TRAN(&st_idle_ctrl); /* 配置完成：停在未使能态等指令 */
		}
		if (elapsed > CONFIG_TIMEOUT_MS) {
			ctl->fault_code = CONTROLLER_FAULT_CONFIG_TIMEOUT;
		}
		return SM_SUPER(&st_online);
	}

	default:
		return SM_SUPER(&st_online);
	}

	return SM_HANDLED();
}
