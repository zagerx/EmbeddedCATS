/*
 * state_fault.c — 故障态
 *
 * ENTRY：把故障原因写进日志，便于定位是“没起来”还是“掉线”还是“被重启”；
 * 静止不动，只等「复位」事件。
 */

#include "states.h"

int st_fault(struct hsm *me, const struct hsm_event *e)
{
	struct cats_app *app = container_of(me, struct cats_app, hsm);

	switch (e->sig) {
	case HSM_SIG_ENTRY:
		app_log(app, "FAULT: %s", fault_name(app->fault_code));
		HSM_HANDLED();

	case HSM_SIG_RESET:
		HSM_TRAN(st_stopping); /* 复位先走一遍停止流程，清理完回 IDLE */

	default:
		HSM_IGNORED();
	}
}
