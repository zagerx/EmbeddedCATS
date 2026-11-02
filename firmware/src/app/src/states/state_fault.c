/*
 * state_fault.c — 故障态
 *
 * 静止不动，只等「复位」事件。故障原因在 app->fault_code，供界面显示。
 */

#include "states.h"

int st_fault(struct hsm *me, const struct hsm_event *e)
{
	switch (e->sig) {
	case HSM_SIG_RESET:
		HSM_TRAN(st_stopping); /* 复位先走一遍停止流程，清理完回 IDLE */
	default:
		HSM_IGNORED();
	}
}
