/*
 * state_operational.c — 运行期父状态
 *
 * IDLE / RUNNING / STOPPING 的共同祖先。放在 FAULT / ESTOP 之外，
 * 是为了让"运行期才有意义的公共动作"不牵连安全态。
 *
 * 两个使用原则（LCA 决定的）：
 *   1. 子状态之间互迁（IDLE ↔ RUNNING）时 LCA 就是本状态 ⇒ 本状态的
 *      ENTRY **不会**重跑。想在"每进一个子状态"都做的事，必须写在子状态里。
 *   2. 从 FAULT / ESTOP 回到运行期，LCA 是 ROOT ⇒ 本状态会重新 ENTRY 一次，
 *      适合放"进入运行期"的公共初始化。
 *
 * TODO（候选公共逻辑）：
 *   ENTRY：使能总线心跳监控 / 清故障计数
 *   EXIT ：公共下使能（任何离开运行期的路径都会经过）
 *   TICK ：心跳超时、节点重启检查（比放在 RUNNING 覆盖更全）
 */

#include "states.h"

#include <stdio.h>

static int st_operational_h(struct hsm *me, const struct hsm_event *e);
/*
 * 状态对象：handler = 本状态的函数，super = 父状态（NULL 表示根），name = 日志用。
 * （以前写作 HSM_STATE_DEF(st_operational, &st_root, "OPERATIONAL")）
 */
const struct hsm_state st_operational = {
	.handler = st_operational_h,
	.super = &st_root,
	.name = "OPERATIONAL",
};

static int st_operational_h(struct hsm *me, const struct hsm_event *e)
{
	int ret = HSM_RET_SUPER; /* 默认：不认识的事件交给父状态 */

	(void)me;

	switch (e->sig) {
	case HSM_SIG_ENTRY:
		printf("enter OPERATIONAL\n");
		/* TODO：进入运行期的公共初始化（从 FAULT/ESTOP 回来时会执行） */
		ret = HSM_RET_HANDLED;
		break;

	case HSM_SIG_EXIT:
		printf("exit OPERATIONAL\n");
		/* TODO：离开运行期的公共清理（→ FAULT / → ESTOP 都会经过） */
		ret = HSM_RET_HANDLED;
		break;

		/* TODO：运行期公共事件（比放在 RUNNING 里覆盖更全），例如
	 * case DEMO_SIG_TICK:
	 *	// 心跳超时检查 → me->target = &st_fault;
	 *	break;
	 */

	default:
		break; /* ret 仍是 SUPER */
	}

	return ret;
}
