/*
 * state_stopping.c — 停止中
 *
 * 进入：下各轴使能 + NMT pre-op（待补）；
 * 每拍：无动作；
 * 退出：无动作。
 */

#include "states.h"

static int st_stopping_h(struct hsm *me, const struct hsm_event *e);
const struct hsm_state st_stopping = {
	.handler = st_stopping_h,
	.super = &st_operational,
	.name = "STOPPING",
};

static int st_stopping_h(struct hsm *me, const struct hsm_event *e)
{
	(void)me;

	switch (e->sig) {
	case HSM_SIG_ENTRY:
		/* TODO(下一拍)：下各轴使能 → co_master_nmt(..., 0x80) */
		break;

	case HSM_SIG_EXIT:
		/* 无出口动作 */
		break;

	default:
		return HSM_RET_SUPER;
	}

	return HSM_RET_HANDLED;
}
