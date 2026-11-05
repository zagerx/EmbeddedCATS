/*
 * state_online.c — 通信建立之后的运行期父状态
 *
 * 只做一件事：心跳。任一被监控节点掉线 ⇒ 置 HEARTBEAT_LOST，
 * 由 ROOT 统一迁 FAULT（本状态不自己迁移）。
 *
 * 原来这条规则写在 control.c 里，用状态白名单判断"哪些状态管心跳"；
 * 现在由"是不是 online 的子状态"表达 —— 新增运行期状态不用改任何名单。
 *
 * PREPARING 故意不挂在 ONLINE 下：那时心跳还没有是正常的，
 * "掉线就是故障"对它不成立。层次结构本身就说明了这件事。
 *
 * 约定：子状态的 TICK 处理完自己的事后返回 SM_SUPER(&st_online)，
 *       依次让 ONLINE（心跳）和 ROOT（故障码）补做公共判定。
 */

#include "states.h"

sm_ret_t st_online(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cats_control *ctl = container_of(me, struct cats_control, hsm);

	switch (e->sig) {
	case CATS_SIG_TICK:
		if (!ctl->hb_ok) {
			ctl->fault_code = CATS_FAULT_HEARTBEAT_LOST; /* 交 ROOT 迁 FAULT */
		}
		return SM_SUPER(&st_root);

	case SM_SIG_ENTRY:
	case SM_SIG_EXIT:
		break; /* 进/出整个运行期：暂无公共动作 */

	default:
		return SM_SUPER(&st_root);
	}

	return SM_HANDLED();
}
