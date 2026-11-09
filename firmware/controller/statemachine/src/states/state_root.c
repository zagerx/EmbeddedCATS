/*
 * state_root.c — 层次状态机的根（父状态 = 框架的 sm_top）
 *
 * 只做三件事：
 *   1. 急停出口：CATS_SIG_ESTOP 在任何状态下都由这里统一迁 ESTOP；
 *   2. 统一故障出口：每拍 TICK 冒泡到这里时，只要 fault_code != NONE 就迁 FAULT。
 *      子状态只负责"置码"，全库不再有第二处 SM_TRAN(&st_fault)；
 *   3. 其余没人认的事件交给 sm_top（返回 IGNORED，事件被忽略）。
 *
 * 根放在控制层而不是内核：根上挂哪些"全局规则"是业务决定，内核不该知道。
 */

#include "states.h"

sm_ret_t st_root(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct stm *ctl = container_of(me, struct stm, hsm);

	if (e->sig == CATS_SIG_ESTOP) {
		return SM_TRAN(&st_estop); /* 急停优先于一切，连故障出口规则也让位 */
	}

	if (e->sig == CATS_SIG_TICK && ctl->fault_code != CATS_FAULT_NONE) {
		return SM_TRAN(&st_fault); /* 任何状态下置了故障码，都在这里统一出去 */
	}

	return SM_SUPER(&sm_top); /* 交给框架顶层：冒泡结束，事件被忽略 */
}
