/*
 * state_ready.c — 就绪：已使能、没有运动在途、可以接受指令
 *
 * 进入：清故障码；丢掉上一个运动的结论（否则会被当成本次运动的结果）。
 * 每拍：取指令槽 —— 有位置指令就带着参数迁进 MOVING；
 *       停止指令在这里没有可做的事（本来就在保持位置）。
 * 退出：无动作。
 *
 * 心跳不在这里判：cats_app_poll() 每拍算好 app->hb_ok，再统一迁移（见 app.c）。
 */

#include "states.h"

static int st_ready_h(struct hsm *me, const struct hsm_event *e);
const struct hsm_state st_ready = {
	.handler = st_ready_h,
	.super = &st_operational,
	.name = "READY",
};

static int st_ready_h(struct hsm *me, const struct hsm_event *e)
{
	struct cats_app *app = container_of(me, struct cats_app, hsm);

	switch (e->sig) {
	case HSM_SIG_ENTRY: {
		uint8_t i;

		app->fault_code = CATS_FAULT_NONE;
		app->move_err = 0;
		for (i = 0; i < app->axis_count; i++) {
			(void)axis_take_motion(&app->axes[i]); /* 丢弃旧结论 */
		}
		break;
	}

	case CATS_SIG_TICK: {
		enum cats_cmd_kind cmd = cats_take_cmd(app);

		if (cmd == CATS_CMD_MOVE) {
			app->move = app->cmd; /* 取副本：之后指令槽再变也不影响本次 */
			HSM_TRAN(&st_moving);
		}
		/* CATS_CMD_STOP：已经在保持位置，忽略 */
		break;
	}

	case HSM_SIG_EXIT:
		break;

	default:
		return HSM_RET_SUPER;
	}

	return HSM_RET_HANDLED;
}
