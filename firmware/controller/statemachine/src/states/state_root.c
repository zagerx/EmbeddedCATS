/*
 * state_root.c — 处理业务状态机的全局急停与故障出口
 *
 * 急停在任何状态下都迁移到 ESTOP；子状态设置的故障码在节拍中统一进入 FAULT。
 * 全局规则属于业务层，不属于通用 HSM 内核。
 *
 * Author: zager
 */

#include "states.h"

/**
 * st_root - 处理全局急停、故障迁移和事件上抛
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: 急停或故障迁移结果；其他事件交给 sm_top。
 */
sm_ret_t st_root(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct controller_sm *ctl = container_of(me, struct controller_sm, hsm);

	if (e->sig == CATS_SIG_ESTOP) {
		return SM_TRAN(&st_estop); /* 急停优先于一切，连故障出口规则也让位 */
	}

	if (e->sig == CATS_SIG_TICK && ctl->fault_code != CATS_FAULT_NONE) {
		return SM_TRAN(&st_fault); /* 任何状态下置了故障码，都在这里统一出去 */
	}

	return SM_SUPER(&sm_top); /* 交给框架顶层：冒泡结束，事件被忽略 */
}
