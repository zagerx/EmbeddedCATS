/*
 * state_root.c — 层次状态机的根
 *
 * 所有状态的最终祖先。只做两件事：
 *   1. 统一故障出口：每拍 TICK 冒泡到这里时，只要 fault_code != NONE 就迁 FAULT。
 *      子状态只负责“置码”，全库不再有第二处 HSM_TRAN(&st_fault)。
 *   2. 其余没人认的事件返回 SUPER，由内核丢弃（不认识事件是正常情况）。
 *
 * 根放在控制层而不是内核：根上挂哪些“全局规则”是业务决定，内核不该知道。
 */

#include "states.h"

static int st_root_h(struct hsm *me, const struct hsm_event *e);
const struct hsm_state st_root = {
	.handler = st_root_h,
	.super = NULL,
	.name = "ROOT",
};

static int st_root_h(struct hsm *me, const struct hsm_event *e)
{
	struct cats_control *ctl = container_of(me, struct cats_control, hsm);

	if (e->sig == CATS_SIG_TICK && ctl->fault_code != CATS_FAULT_NONE) {
		HSM_TRAN(&st_fault); /* 任何状态下置了故障码，都在这里统一出去 */
	}

	return HSM_RET_SUPER;
}
