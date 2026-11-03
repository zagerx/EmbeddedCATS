/*
 * state_running.c — 运行中
 *
 * 进入：清掉故障码；
 * 每拍：任一轴对应节点心跳不正常 → FAULT；
 * 退出：停 SYNC（出口动作的天然位置，待补）。
 *
 * 超时门槛不在本文件：OD 0x1016 配了心跳周期，内核按 1.5 倍判掉线，
 * 应用层只问“是不是都还活着”。
 */

#include "states.h"

static int st_running_h(struct hsm *me, const struct hsm_event *e);
HSM_STATE_DEF(st_running, &st_operational, "RUNNING");

static int st_running_h(struct hsm *me, const struct hsm_event *e)
{
	struct cats_app *app = container_of(me, struct cats_app, hsm);

	switch (e->sig) {
	case HSM_SIG_ENTRY:
		app->fault_code = CATS_FAULT_NONE;
		break;

	case HSM_SIG_EXIT:
		/* TODO(下一拍)：停 SYNC */
		break;

	case CATS_SIG_TICK:
		/* 任一个节点掉线就整体 FAULT：安全优先 */
		if (!all_axes_alive(app)) {
			app->fault_code = CATS_FAULT_HEARTBEAT_LOST;
			HSM_TRAN(&st_fault);
		}
		/* TODO(下一拍)：发 SYNC、检查 402 状态字 */
		break;

	default:
		HSM_SUPER();
	}

	return HSM_RET_HANDLED;
}
