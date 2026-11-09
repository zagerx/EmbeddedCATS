/*
 * app.c — 业务状态机的驱动（事件入队、每拍推进、事实查询）
 *
 * 这里没有 I/O、没有 402、没有线程创建：只把事件喂给状态机，再把事实读出来。
 */

#include "statemachine_internal.h"

#include "states/states.h"

#include <stddef.h>
#include <string.h>

static struct stm s_app;

struct stm *stm_instance(void)
{
	return &s_app;
}

uint32_t stm_now_ms(struct stm *a)
{
	return (a != NULL && a->ops.now_ms != NULL) ? a->ops.now_ms(a->ops.ctx) : 0u;
}

void stm_init(struct stm *a, struct cats_axis *axes, uint8_t axis_count, const struct stm_ops *ops)
{
	if (a == NULL) {
		return;
	}
	memset(a, 0, sizeof(*a));
	a->axes = axes;
	a->axis_count = axis_count;
	if (ops != NULL) {
		a->ops = *ops;
	}
	cats_evtq_init(&a->evtq);
	/* initial() 返回 SM_TRAN(&st_preparing)：内核把 top → root → preparing 的
	 * ENTRY 链走一遍，广播复位从 PREPARING 的 ENTRY 里发出 */
	sm_hsm_init(&a->hsm, &initial);
	a->state_since_ms = stm_now_ms(a);
}

void stm_poll(struct stm *a, uint8_t hb_ok, uint32_t now_ms)
{
	static const struct sm_event evt_tick = {CATS_SIG_TICK};
	struct cats_evt evt;
	sm_state_t before;
	unsigned n;

	if (a == NULL) {
		return;
	}
	a->hb_ok = hb_ok;

	/* 1. 先消费外部事件：停止/急停插在队头，所以这一步永远先于本拍推进 */
	for (n = 0u; n < CATS_EVTQ_DRAIN_MAX; n++) {
		if (!cats_evtq_pop(&a->evtq, &evt)) {
			break;
		}
		before = sm_hsm_state(&a->hsm);
		sm_hsm_dispatch(&a->hsm, &evt.super);
		if (sm_hsm_state(&a->hsm) != before) {
			a->state_since_ms = now_ms;
		}
	}

	/* 2. 一拍一次节拍事件 */
	before = sm_hsm_state(&a->hsm);
	sm_hsm_dispatch(&a->hsm, &evt_tick);
	if (sm_hsm_state(&a->hsm) != before) {
		a->state_since_ms = now_ms;
	}
}

enum cats_result stm_command(struct stm *a, enum stm_cmd kind, const struct stm_move *mv)
{
	struct cats_evt e;

	if (a == NULL) {
		return CATS_ERR_STATE;
	}
	memset(&e, 0, sizeof(e));

	switch (kind) {
	case STM_CMD_MOVE:
		if (mv == NULL) {
			return CATS_ERR_PARAM;
		}
		e.super.sig = CATS_SIG_MOVE;
		e.u.move.axis = mv->axis;
		e.u.move.mm = mv->mm;
		e.u.move.mm_s = mv->mm_s;
		e.u.move.mm_s2 = mv->mm_s2;
		/* 排到队尾：多条指令按顺序保留；队列满则本条被丢弃 */
		return cats_evtq_push(&a->evtq, &e) ? CATS_OK : CATS_ERR_BUSY;

	case STM_CMD_STOP:
		e.super.sig = CATS_SIG_STOP;
		cats_evtq_push_front(&a->evtq, &e); /* 插队：优先于已排队的命令 */
		return CATS_OK;

	case STM_CMD_ESTOP:
		e.super.sig = CATS_SIG_ESTOP;
		cats_evtq_push_front(&a->evtq, &e);
		return CATS_OK;

	default:
		return CATS_ERR_PARAM;
	}
}

enum cats_state stm_state(const struct stm *a)
{
	int i;

	if (a == NULL) {
		return CATS_STATE_PREPARING;
	}
	for (i = 0; i < CATS_STATE_COUNT; i++) {
		if (states[i] == sm_hsm_state(&a->hsm)) {
			return (enum cats_state)i;
		}
	}
	return CATS_STATE_PREPARING;
}

enum cats_fault stm_fault(const struct stm *a)
{
	return (a == NULL) ? CATS_FAULT_NONE : a->fault_code;
}
