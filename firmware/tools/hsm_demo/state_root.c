/*
 * state_root.c — 树的根
 *
 * 所有状态的最终祖先。**任何状态都要响应的事件**放这里（比如以后想加
 * "收到 ESTOP 事件就迁移"），这样只需写一份，所有子状态自动继承。
 *
 * 现在什么都不做：冒泡到根说明没人认识该事件，内核会丢弃它（不算异常）。
 */

#include "states.h"

#include <stdio.h>

static int st_root_h(struct hsm *me, const struct hsm_event *e);
/*
 * 状态对象：handler = 本状态的函数，super = 父状态（NULL 表示根），name = 日志用。
 * （以前写作 HSM_STATE_DEF(st_root, NULL, "ROOT")，宏展开后就是下面这四行）
 */
const struct hsm_state st_root = {
	.handler = st_root_h,
	.super = NULL,
	.name = "ROOT",
};

static int st_root_h(struct hsm *me, const struct hsm_event *e)
{
	int ret = HSM_RET_SUPER; /* 根也不认识 → 冒泡结束，事件被丢弃 */

	(void)me;

	switch (e->sig) {
	case HSM_SIG_ENTRY:
		printf("enter ROOT\n");
		/* TODO：进入根（目前无动作） */
		ret = HSM_RET_HANDLED;
		break;

	case HSM_SIG_EXIT:
		printf("exit ROOT\n");
		/* TODO：离开根（目前无动作） */
		ret = HSM_RET_HANDLED;
		break;

		/* TODO：任何状态都要响应的事件放这里，例如
	 * case DEMO_SIG_ESTOP:
	 *	me->target = &st_estop;
	 *	ret = HSM_RET_TRAN;
	 *	break;
	 */

	default:
		break; /* ret 仍是 SUPER */
	}

	return ret;
}
