/*
 * state_idle.c — 空闲
 *
 * 上电初态：不输出、不使能，等 START。
 *
 * TODO：
 *   ENTRY：清故障码、复位记录
 *   TICK ：检查设备是否就绪，就绪了可以发 READY
 *   EXIT ：停掉空闲期的周期动作
 */

#include "states.h"

#include <stdio.h>

static int st_idle_h(struct hsm *me, const struct hsm_event *e);
/*
 * 状态对象：handler = 本状态的函数，super = 父状态（NULL 表示根），name = 日志用。
 * （以前写作 HSM_STATE_DEF(st_idle, &st_operational, "IDLE")）
 */
const struct hsm_state st_idle = {
	.handler = st_idle_h,
	.super = &st_operational,
	.name = "IDLE",
};

static int st_idle_h(struct hsm *me, const struct hsm_event *e)
{
	int ret = HSM_RET_SUPER; /* 默认：不认识的事件交给父状态 */

	(void)me;

	switch (e->sig) {
	case HSM_SIG_ENTRY:
		printf("enter IDLE\n");
		/* TODO：进入空闲要做的事 */
		ret = HSM_RET_HANDLED;
		break;

	case HSM_SIG_EXIT:
		printf("exit IDLE\n");
		/* TODO：离开空闲要做的事 */
		ret = HSM_RET_HANDLED;
		break;

		/* TODO 业务事件示例（需要时打开）：
	 * case DEMO_SIG_START:
	 *	me->target = &st_running;
	 *	ret = HSM_RET_TRAN;
	 *	break;
	 */

	default:
		break; /* ret 仍是 SUPER */
	}

	return ret;
}
