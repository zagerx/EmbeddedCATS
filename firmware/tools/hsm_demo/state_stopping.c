/*
 * state_stopping.c — 停止中
 *
 * 从运行态退出的过渡态：先下使能/停机，等设备真的停了再回 IDLE。
 *
 * TODO：
 *   ENTRY：下发停机命令
 *   TICK ：等设备停止（内核无定时器，用 me->entered_ms 自己算超时）
 *   EXIT ：无（停机命令只发一次）
 */

#include "states.h"

#include <stdio.h>

static int st_stopping_h(struct hsm *me, const struct hsm_event *e);
/*
 * 状态对象：handler = 本状态的函数，super = 父状态（NULL 表示根），name = 日志用。
 */
const struct hsm_state st_stopping = {
	.handler = st_stopping_h,
	.super = &st_operational,
	.name = "STOPPING",
};

static int st_stopping_h(struct hsm *me, const struct hsm_event *e)
{
	int ret = HSM_RET_SUPER; /* 默认：不认识的事件交给父状态 */

	(void)me;

	switch (e->sig) {
	case HSM_SIG_ENTRY:
		printf("enter STOPPING\n");
		/* TODO：下发停机命令 */
		ret = HSM_RET_HANDLED;
		break;

	case HSM_SIG_EXIT:
		printf("exit STOPPING\n");
		/* TODO：停机命令只发一次，这里通常什么都不用做 */
		ret = HSM_RET_HANDLED;
		break;

		/* TODO 业务事件示例（需要时打开）：
	 * case DEMO_SIG_READY:  设备已停稳
	 *	me->target = &st_idle;
	 *	ret = HSM_RET_TRAN;
	 *	break;
	 */

	default:
		break; /* ret 仍是 SUPER */
	}

	return ret;
}
