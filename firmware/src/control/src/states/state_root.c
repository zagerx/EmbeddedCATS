/*
 * state_root.c — 层次状态机的根
 *
 * 所有状态的最终祖先。它不处理任何事件：冒泡到这里说明没人认识该事件，
 * 由内核丢弃（不计 anomaly——不认识事件是正常情况）。
 *
 * 根放在应用层而不是内核：根上挂哪些"全局事件"（比如以后想加 ESTOP 事件）
 * 是业务决定，内核不该知道。
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
	(void)me;
	(void)e;
	return HSM_RET_SUPER;
}
