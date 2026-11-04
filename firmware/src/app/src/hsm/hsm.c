/*
 * hsm.c — 层次状态机内核（QP 风格，机制层）
 *
 * 三个入口（init / dispatch / force）最终都走 transition()：
 *   EXIT 链 → 唯一一处 state 赋值 → ENTRY 链。
 *
 * 事件由调用方直接喂给 hsm_dispatch()，一次一个；
 * handler 里不要回调 dispatch()，这是 RTC 的唯一约定。
 */

#include "hsm.h"

static const struct hsm_event ENTRY_EVT = {HSM_SIG_ENTRY, 0};
static const struct hsm_event EXIT_EVT = {HSM_SIG_EXIT, 0};

/* ---- 层次工具 ---- */

static unsigned depth_of(const struct hsm_state *s)
{
	unsigned d = 0u;

	while (s != NULL && d <= HSM_MAX_DEPTH) {
		s = s->super;
		d++;
	}
	return d;
}

/* 最近公共祖先：两条父链对齐深度后一起上溯到相同节点 */
static const struct hsm_state *lca_of(const struct hsm_state *a, const struct hsm_state *b)
{
	unsigned da = depth_of(a);
	unsigned db = depth_of(b);
	const struct hsm_state *x = a;
	const struct hsm_state *y = b;

	while (da > db) {
		x = x->super;
		da--;
	}
	while (db > da) {
		y = y->super;
		db--;
	}
	while (x != y) {
		if (x == NULL || y == NULL) {
			return NULL; /* 父链成环/超深：调用方计一次 anomaly */
		}
		x = x->super;
		y = y->super;
	}
	return x;
}

/* 伪事件：内核负责逐级调用，handler 只应 HANDLED；返回 TRAN 属非法动作，忽略 */
static void exit_one(struct hsm *h, const struct hsm_state *s)
{
	(void)s->handler(h, &EXIT_EVT);
}

static void enter_one(struct hsm *h, const struct hsm_state *s)
{
	(void)s->handler(h, &ENTRY_EVT);
}

/* 从 LCA 的子级逐级向下 ENTRY 到 target */
static void enter_chain(struct hsm *h, const struct hsm_state *target, const struct hsm_state *lca)
{
	const struct hsm_state *chain[HSM_MAX_DEPTH];
	const struct hsm_state *s;
	unsigned n = 0u;
	unsigned i;

	for (s = target; s != NULL && s != lca && n < HSM_MAX_DEPTH; s = s->super) {
		chain[n] = s;
		n++;
	}
	if (s != lca) {
		return; /* 父链超深或成环：不 ENTRY */
	}
	for (i = n; i > 0u; i--) {
		enter_one(h, chain[i - 1u]);
	}
}

/* 唯一的迁移实现：init / dispatch / force 都走这里 */
static int transition(struct hsm *h, const struct hsm_state *target, uint32_t now)
{
	const struct hsm_state *from = h->state;
	const struct hsm_state *lca;
	const struct hsm_state *s;
	unsigned n = 0u;

	if (target == NULL || from == NULL) {
		return 0;
	}
	if (target == from) {
		return 0; /* 自迁移：不 EXIT 也不 ENTRY */
	}

	lca = lca_of(from, target);
	if (lca == NULL) {
		return 0;
	}

	/* 1. EXIT：当前状态逐级向上，到 LCA 之前为止（父状态只出口一次） */
	for (s = from; s != NULL && s != lca; s = s->super) {
		n++;
		if (n > HSM_MAX_DEPTH) {
			return 0;
		}
		exit_one(h, s);
	}

	/* 2. 全代码库唯一一处 state 赋值 */
	h->state = target;
	h->entered_ms = now;

	/* 3. ENTRY：从 LCA 的子级向下到 target */
	enter_chain(h, target, lca);

	return 1;
}

/* ---- 对外接口 ---- */

void hsm_init(struct hsm *h, const struct hsm_state *initial, uint32_t now)
{
	if (h == NULL || initial == NULL) {
		return;
	}

	h->state = initial;
	h->target = initial;
	h->entered_ms = now;

	/* 从根一路 ENTRY 到初态：父状态的公共初始化有位置可写 */
	enter_chain(h, initial, NULL);
}

int hsm_dispatch(struct hsm *h, int sig, int32_t arg, uint32_t now)
{
	const struct hsm_event evt = {sig, arg};
	const struct hsm_state *s;
	int ret = HSM_RET_SUPER;
	unsigned n = 0u;

	if (h == NULL || h->state == NULL) {
		return 0;
	}

	h->target = h->state; /* TRAN 未填则视为自迁移 */

	/* 冒泡：不认识就交给父状态，直到根 */
	for (s = h->state; s != NULL; s = s->super) {
		n++;
		if (n > HSM_MAX_DEPTH) {
			break;
		}
		ret = s->handler(h, &evt);
		if (ret != HSM_RET_SUPER) {
			break;
		}
	}

	if (ret != HSM_RET_TRAN) {
		return 0;
	}
	return transition(h, h->target, now);
}

void hsm_transition(struct hsm *h, const struct hsm_state *target, uint32_t now)
{
	if (h == NULL || target == NULL) {
		return;
	}
	(void)transition(h, target, now);
}

const struct hsm_state *hsm_current(const struct hsm *h)
{
	return h != NULL ? h->state : NULL;
}
