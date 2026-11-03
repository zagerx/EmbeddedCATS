/*
 * state_running.c — 运行中
 *
 * 进入：清掉故障码；
 * 每拍：发 SYNC、查 402 状态字（待补）；
 * 退出：停 SYNC（出口动作的天然位置，待补）。
 *
 * 心跳不在这里判：cats_app_poll() 每拍算好 app->hb_ok，并由同一个入口
 * 在"该管心跳的状态"里统一迁到 FAULT（见 app.c 的 handle_heartbeat）。
 */

#include "states.h"

static int st_running_h(struct hsm *me, const struct hsm_event *e);
const struct hsm_state st_running = {
	.handler = st_running_h,
	.super = &st_operational,
	.name = "RUNNING",
};

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
		/* TODO(下一拍)：发 SYNC、检查 402 状态字 */
		break;

	default:
		return HSM_RET_SUPER;
	}

	return HSM_RET_HANDLED;
}
