/*
 * state_stopping.c — 停止中
 *
 * 暂为静止态：进入后不动，出口逻辑（下各轴使能、NMT pre-op）后续再补。
 */

#include "states.h"

int st_stopping(struct hsm *me, const struct hsm_event *e)
{
	(void)me;
	(void)e;
	HSM_IGNORED();
}
