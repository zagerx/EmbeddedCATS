/*
 * state_online.c — 为运行期子状态提供统一心跳检查
 *
 * 任一被监控节点掉线时设置 HEARTBEAT_LOST，再由 ROOT 迁移到 FAULT。
 * PREPARING 不属于 ONLINE，因此启动期没有心跳不会触发运行期掉线规则。
 *
 * Author: zager
 */

#include "states.h"

/**
 * st_online - 执行运行期父状态的心跳检查
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: 节拍处理后上抛 ROOT；未识别事件上抛父状态。
 */
sm_ret_t st_online(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct controller_sm *ctl = container_of(me, struct controller_sm, hsm);

	switch (e->sig) {
	case CATS_SIG_TICK:
		if (!ctl->hb_ok) {
			ctl->fault_code = CATS_FAULT_HEARTBEAT_LOST; /* 交 ROOT 迁 FAULT */
		}
		return SM_SUPER(&st_root);

	case SM_SIG_ENTRY:
	case SM_SIG_EXIT:
		break; /* 进/出整个运行期：暂无公共动作 */

	default:
		return SM_SUPER(&st_root);
	}

	return SM_HANDLED();
}
