/*
 * state_fault.c — 故障
 *
 * 父状态是 ROOT 而不是 OPERATIONAL：故障态不继承"运行期"语义，
 * 以后 OPERATIONAL 的 EXIT 里放下使能之类动作也不会牵连到这里。
 *
 * TODO：
 *   ENTRY：记录/上报故障原因
 *   RESET：清除故障后回 IDLE（要不要允许由业务定）
 */

#include "states.h"

#include <stdio.h>

static int st_fault_h(struct hsm *me, const struct hsm_event *e);
/*
 * 状态对象：handler = 本状态的函数，super = 父状态（NULL 表示根），name = 日志用。
 * （以前写作 HSM_STATE_DEF(st_fault, &st_root, "FAULT")）
 */
const struct hsm_state st_fault = {
	.handler = st_fault_h,
	.super = &st_root,
	.name = "FAULT",
};

static int st_fault_h(struct hsm *me, const struct hsm_event *e)
{
	int ret = HSM_RET_SUPER; /* 默认：不认识的事件交给父状态 */

	(void)me;

	switch (e->sig) {
	case HSM_SIG_ENTRY:
		printf("enter FAULT\n");
		/* TODO：记录/上报故障原因 */
		ret = HSM_RET_HANDLED;
		break;

	case HSM_SIG_EXIT:
		printf("exit FAULT\n");
		/* TODO：通常什么都不用做 */
		ret = HSM_RET_HANDLED;
		break;

		/* TODO 业务事件示例（需要时打开）：
	 * case DEMO_SIG_RESET:
	 *	me->target = &st_idle;
	 *	ret = HSM_RET_TRAN;
	 *	break;
	 */

	default:
		break; /* ret 仍是 SUPER */
	}

	return ret;
}
