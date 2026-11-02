/*
 * state_idle.c — 空闲态
 *
 * 什么都不干，只等「启动」事件。
 */

#include "states.h"

int st_idle(struct hsm *me, const struct hsm_event *e)
{
	switch (e->sig) {
	case HSM_SIG_START:
		HSM_TRAN(st_starting);
	default:
		HSM_IGNORED();
	}
}
