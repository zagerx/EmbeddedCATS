/*
 * state_running.c — 运行中
 *
 * 正常出力阶段。
 *
 * TODO：
 *   ENTRY：发 SYNC、下使能
 *   TICK ：周期控制 / 状态字检查 / 心跳判定
 *   EXIT ：停 SYNC（离开运行态就停）
 */

#include "states.h"

#include <stdio.h>

static int st_running_h(struct hsm *me, const struct hsm_event *e);
/*
 * 状态对象：handler = 本状态的函数，super = 父状态（NULL 表示根），name = 日志用。
 */
const struct hsm_state st_running = {
	.handler = st_running_h,
	.super = &st_operational,
	.name = "RUNNING",
};

static int st_running_h(struct hsm *me, const struct hsm_event *e)
{
	int ret = HSM_RET_SUPER; /* 默认：不认识的事件交给父状态 */

	(void)me;

	switch (e->sig) {
	case HSM_SIG_ENTRY:
		printf("enter RUNNING\n");
		/* TODO：发 SYNC、下使能 */
		ret = HSM_RET_HANDLED;
		break;

	case HSM_SIG_EXIT:
		printf("exit RUNNING\n");
		/* TODO：停 SYNC */
		ret = HSM_RET_HANDLED;
		break;

		/* TODO 业务事件示例（需要时打开）：
	 * case DEMO_SIG_STOP:
	 *	me->target = &st_stopping;
	 *	ret = HSM_RET_TRAN;
	 *	break;
	 *
	 * case DEMO_SIG_FAULT:
	 *	me->target = &st_fault;
	 *	ret = HSM_RET_TRAN;
	 *	break;
	 */

	default:
		break; /* ret 仍是 SUPER */
	}

	return ret;
}
