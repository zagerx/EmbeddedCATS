/*
 * hsm.c — QP 风格状态机引擎
 *
 * 整个状态机只有这一个引擎。状态的切换、ENTRY/EXIT 的调用时序、
 * 状态赋值唯一性、迁移日志，全部由本文件保证。
 */

#include "hsm.h"

#include <stdio.h>

static const struct hsm_event ENTRY_EVT = {HSM_SIG_ENTRY};
static const struct hsm_event EXIT_EVT = {HSM_SIG_EXIT};

static void log_transition(struct hsm *h, hsm_state_fn from, hsm_state_fn to)
{
	char buf[64];

	if (h->log == NULL) {
		return;
	}
	snprintf(buf, sizeof buf, "%s -> %s", h->state_name ? h->state_name(from) : "?",
		 h->state_name ? h->state_name(to) : "?");
	h->log(buf);
}

void hsm_init(struct hsm *h, hsm_state_fn initial, const char *(*name)(hsm_state_fn),
	      void (*log)(const char *), uint32_t now)
{
	h->state = initial;
	h->target = initial;
	h->entered_ms = now;
	h->log = log;
	h->state_name = name;
	h->state(h, &ENTRY_EVT); /* 初态也走入口动作 */
}

void hsm_dispatch(struct hsm *h, const struct hsm_event *e, uint32_t now)
{
	hsm_state_fn from;

	if (h->state(h, e) != HSM_RET_TRAN) {
		return;
	}

	from = h->state;
	from(h, &EXIT_EVT); /* 1. 旧状态出口动作 */
	log_transition(h, from, h->target);
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
	log_transition(h, from, target);
	h->state = target;
	h->entered_ms = now;
	h->state(h, &ENTRY_EVT);
}
