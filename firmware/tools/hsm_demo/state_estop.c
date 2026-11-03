/*
 * state_estop.c — 急停
 *
 * 只能由 hsm_transition(&hsm, &st_estop, now) 进入（急停不走事件表，必须立即生效），
 * 进入后停住，不自动退出。
 *
 * TODO：
 *   ENTRY：停 SYNC、各轴 quick stop（安全动作只在这里做一次）
 */

#include "states.h"

#include <stdio.h>

static int st_estop_h(struct hsm *me, const struct hsm_event *e);
/*
 * 状态对象：handler = 本状态的函数，super = 父状态（NULL 表示根），name = 日志用。
 */
const struct hsm_state st_estop = {
	.handler = st_estop_h,
	.super = &st_root,
	.name = "ESTOP",
};

static int st_estop_h(struct hsm *me, const struct hsm_event *e)
{
	int ret = HSM_RET_SUPER; /* 默认：不认识的事件交给父状态 */

	(void)me;

	switch (e->sig) {
	case HSM_SIG_ENTRY:
		printf("enter ESTOP\n");
		/* TODO：停 SYNC、各轴 quick stop（安全动作只做一次） */
		ret = HSM_RET_HANDLED;
		break;

	case HSM_SIG_EXIT:
		printf("exit ESTOP\n");
		/* 急停不自动退，写出来是为了保持骨架一致 */
		ret = HSM_RET_HANDLED;
		break;

	default:
		break; /* ret 仍是 SUPER */
	}

	return ret;
}
