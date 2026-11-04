/*
 * state_estop.c — 急停态
 *
 * ENTRY：各轴 quick stop → 断使能（两段式，由 402 层异步走完）。
 *        急停是安全动作，**抢占**任何在途过程（运动、停车、使能都不排队）。
 * 之后：停住不动。没有出口事件，只能重启程序——自动恢复会把风险带回来。
 *
 * 进本状态不走"允许/拒绝"判断：cats_control_poll() 里急停请求优先于一切。
 */

#include "states.h"

static int st_estop_h(struct hsm *me, const struct hsm_event *e);
const struct hsm_state st_estop = {
	.handler = st_estop_h,
	.super = &st_root,
	.name = "ESTOP",
};

static int st_estop_h(struct hsm *me, const struct hsm_event *e)
{
	struct cats_control *app = container_of(me, struct cats_control, hsm);

	switch (e->sig) {
	case HSM_SIG_ENTRY:
		axis_quick_stop_all(app->axes, app->axis_count);
		break;

	case HSM_SIG_EXIT:
		break;

	default:
		return HSM_RET_SUPER;
	}

	return HSM_RET_HANDLED;
}
