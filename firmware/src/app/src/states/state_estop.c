/*
 * state_estop.c — 急停态
 *
 * ENTRY：停 SYNC + 各轴 quick stop（安全动作只在这里做一次）；
 * RESET：人工确认后回 IDLE。其余事件一律不理，绝不自动重试。
 */

#include "states.h"

int st_estop(struct hsm *me, const struct hsm_event *e)
{
	struct cats_app *app = container_of(me, struct cats_app, hsm);

	switch (e->sig) {
	case HSM_SIG_ENTRY:
		/* TODO(下一拍)：停 SYNC + 各轴 cia402_quick_stop() */
		(void)app;
		HSM_HANDLED();

	case HSM_SIG_RESET:
		HSM_TRAN(st_idle);

	default:
		HSM_IGNORED();
	}
}
