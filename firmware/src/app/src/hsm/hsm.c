/*
 * hsm.c — QP 风格状态机引擎
 *
 * 整个状态机只有这一个引擎。状态的切换、ENTRY/EXIT 的调用时序、
 * 状态赋值唯一性，全部由本文件保证。
 *
 * 本文件的产出只有一个：h->state 被改写成新状态。谁要知道迁移结果，
 * 比对调用前后的 h->state 即可（见 app.c 的 dispatch()）。
 */

#include "hsm.h"

static const struct hsm_event ENTRY_EVT = {HSM_SIG_ENTRY};
static const struct hsm_event EXIT_EVT = {HSM_SIG_EXIT};

void hsm_init(struct hsm *h, hsm_state_fn initial, uint32_t now)
{
	h->state = initial;
	h->target = initial;
	h->entered_ms = now;
	h->state(h, &ENTRY_EVT); /* 初态也走入口动作 */
}

void hsm_dispatch(struct hsm *h, const struct hsm_event *e, uint32_t now)
{
	hsm_state_fn from;

	if (h->state(h, e) != HSM_RET_TRAN) {
		return;
	}

	from = h->state;
	from(h, &EXIT_EVT);   /* 1. 旧状态出口动作 */
	h->state = h->target; /* 2. 全代码库唯一的状态赋值点 */
	h->entered_ms = now;
	h->state(h, &ENTRY_EVT); /* 3. 新状态入口动作 */
}

void hsm_force(struct hsm *h, hsm_state_fn target, uint32_t now)
{
	hsm_state_fn from = h->state;

	if (from == target) {
		return;
	}
	from(h, &EXIT_EVT);
	h->state = target;
	h->entered_ms = now;
	h->state(h, &ENTRY_EVT);
}
