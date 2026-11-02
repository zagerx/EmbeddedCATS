/*
 * state_running.c — 运行中
 *
 * TICK：发 SYNC、收心跳、检查节点超时与 402 Fault；
 * EXIT：不管因何离开运行态，都停 SYNC（出口动作的天然位置）；
 * STOP：进 STOPPING。
 */

#include "states.h"

int st_running(struct hsm *me, const struct hsm_event *e)
{
	struct cats_app *app = container_of(me, struct cats_app, hsm);

	switch (e->sig) {
	case HSM_SIG_TICK:
		/* TODO(下一拍)：发 SYNC、收心跳、检查节点超时与 402 Fault */
		(void)app;
		HSM_HANDLED();

	case HSM_SIG_EXIT:
		/* TODO(下一拍)：停 SYNC */
		HSM_HANDLED();

	case HSM_SIG_STOP:
		HSM_TRAN(st_stopping);

	default:
		HSM_IGNORED();
	}
}
