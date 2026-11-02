/*
 * state_stopping.c — 停止中
 *
 * ENTRY：下各轴使能 + NMT pre-op；
 * TICK ：清理完成后回 IDLE。
 */

#include "states.h"

int st_stopping(struct hsm *me, const struct hsm_event *e)
{
	struct cats_app *app = container_of(me, struct cats_app, hsm);

	switch (e->sig) {
	case HSM_SIG_ENTRY:
		/* TODO(下一拍)：下各轴使能 → co_master_nmt(..., 0x80) */
		(void)app;
		HSM_HANDLED();

	case HSM_SIG_TICK:
		HSM_TRAN(st_idle); /* TODO: 改成"清理完成后"再迁移 */

	default:
		HSM_IGNORED();
	}
}
