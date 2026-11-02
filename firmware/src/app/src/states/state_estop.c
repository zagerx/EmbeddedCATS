/*
 * state_estop.c — 急停态
 *
 * ENTRY：停 SYNC + 各轴 quick stop（安全动作只在这里做一次）；
 * 之后停住不动：没有出口事件，只能重启程序。
 */

#include "states.h"

static int st_estop_h(struct hsm *me, const struct hsm_event *e);
HSM_STATE_DEF(st_estop, &st_root, "ESTOP");

static int st_estop_h(struct hsm *me, const struct hsm_event *e)
{
	struct cats_app *app = container_of(me, struct cats_app, hsm);

	switch (e->sig) {
	case HSM_SIG_ENTRY:
		/* TODO(下一拍)：停 SYNC + 各轴 cia402_quick_stop() */
		(void)app;
		HSM_HANDLED();

	default:
		HSM_SUPER();
	}
}
