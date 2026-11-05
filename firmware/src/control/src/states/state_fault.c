/*
 * state_fault.c — 故障态
 *
 * ENTRY：无动作（原因已由出事的那个位置写进 fault_code，查询接口可读）；
 * 之后停住不动：没有出口事件，只能重启程序。
 *
 * 父状态是 st_root 而不是 st_online：故障态不继承运行期语义。
 */

#include "states.h"

sm_ret_t st_fault(struct sm_hsm *const me, const struct sm_event *const e)
{
	(void)me;

	switch (e->sig) {
	case SM_SIG_ENTRY:
		/* 无入口动作 */
		break;

	case SM_SIG_EXIT:
		/* 无出口动作 */
		break;

	case CATS_SIG_TICK:
		/* 终态吃掉节拍：不往上抛，免得 ROOT 再判一次故障码（自迁移）*/
		return SM_HANDLED();

	default:
		return SM_SUPER(&st_root);
	}

	return SM_HANDLED();
}
