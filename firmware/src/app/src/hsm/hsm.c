/*
 * hsm.c — 层次状态机内核（QP 风格，机制层）
 *
 * 三个入口（init / dispatch / force）最终都走 transition()：
 *   EXIT 链 → 唯一一处 state 赋值 → ENTRY 链 → trace 回调。
 *
 * 队列与定时器只往队列里投事件，真正执行永远在 hsm_dispatch() 里，
 * 所以 handler 之间不会互相嵌套（RTC）。
 */

#include "hsm.h"

static const struct hsm_event ENTRY_EVT = {HSM_SIG_ENTRY, 0};
static const struct hsm_event EXIT_EVT = {HSM_SIG_EXIT, 0};

/* ---- 环形队列：满则丢最旧（新的安全事件仍能进队） ---- */

static void queue_push(struct hsm *h, const struct hsm_event *e)
{
	if (h->q_count == HSM_QUEUE_LEN) {
		h->q_head = (uint8_t)((h->q_head + 1u) % HSM_QUEUE_LEN);
		h->q_count--;
		h->dropped++; /* 丢弃次数对外可见，不静默 */
	}
	h->queue[h->q_tail] = *e;
	h->q_tail = (uint8_t)((h->q_tail + 1u) % HSM_QUEUE_LEN);
	h->q_count++;
}

static int queue_pop(struct hsm *h, struct hsm_event *out)
{
	if (h->q_count == 0u) {
		return 0;
	}
	*out = h->queue[h->q_head];
	h->q_head = (uint8_t)((h->q_head + 1u) % HSM_QUEUE_LEN);
	h->q_count--;
	return 1;
}

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

/* 伪事件：内核负责逐级调用，handler 只应 HANDLED；返回 TRAN 属非法动作 */
static void exit_one(struct hsm *h, const struct hsm_state *s)
{
	h->current = EXIT_EVT;
	if (s->handler(h, &EXIT_EVT) == HSM_RET_TRAN) {
		h->anomalies++;
	}
}

static void enter_one(struct hsm *h, const struct hsm_state *s)
{
	h->current = ENTRY_EVT;
	if (s->handler(h, &ENTRY_EVT) == HSM_RET_TRAN) {
		h->anomalies++;
	}
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
		h->anomalies++; /* 父链超深或成环：不 ENTRY，保持可诊断 */
		return;
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
		h->anomalies++;
		return 0;
	}
	if (target == from) {
		return 0; /* 自迁移：不 EXIT 也不 ENTRY */
	}

	lca = lca_of(from, target);
	if (lca == NULL) {
		h->anomalies++;
		return 0;
	}

	/* 1. EXIT：当前状态逐级向上，到 LCA 之前为止（父状态只出口一次） */
	for (s = from; s != NULL && s != lca; s = s->super) {
		n++;
		if (n > HSM_MAX_DEPTH) {
			h->anomalies++;
			return 0;
		}
		exit_one(h, s);
	}

	/* 2. 全代码库唯一一处 state 赋值 */
	h->state = target;
	h->entered_ms = now;

	/* 3. ENTRY：从 LCA 的子级向下到 target */
	enter_chain(h, target, lca);

	if (h->trace != NULL) {
		h->trace(h->trace_ctx, from, target);
	}
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
	h->q_head = 0u;
	h->q_tail = 0u;
	h->q_count = 0u;
	h->d_count = 0u;
	h->timer_evt = ENTRY_EVT;
	h->timer_deadline = 0u;
	h->timer_armed = 0u;
	h->current = ENTRY_EVT;
	h->trace = NULL;
	h->trace_ctx = NULL;
	h->dropped = 0u;
	h->anomalies = 0u;

	/* 从根一路 ENTRY 到初态：父状态的公共初始化有位置可写 */
	enter_chain(h, initial, NULL);
}

int hsm_dispatch(struct hsm *h, const struct hsm_event *e, uint32_t now)
{
	const struct hsm_state *s;
	int ret = HSM_RET_SUPER;
	unsigned n = 0u;

	if (h == NULL || e == NULL || h->state == NULL) {
		return 0;
	}

	h->current = *e;      /* hsm_defer 取的是这份副本 */
	h->target = h->state; /* TRAN 未填则视为自迁移 */

	/* 冒泡：不认识就交给父状态，直到根 */
	for (s = h->state; s != NULL; s = s->super) {
		n++;
		if (n > HSM_MAX_DEPTH) {
			h->anomalies++;
			break;
		}
		ret = s->handler(h, e);
		if (ret != HSM_RET_SUPER) {
			break;
		}
	}

	if (ret != HSM_RET_TRAN) {
		return 0;
	}
	return transition(h, h->target, now);
}

void hsm_poll(struct hsm *h, uint32_t now)
{
	struct hsm_event e;
	unsigned n;

	if (h == NULL || h->state == NULL) {
		return;
	}

	/* 1. 定时器到期：投递到队列，走与外部事件相同的路径（RTC） */
	if (h->timer_armed != 0u && (int32_t)(now - h->timer_deadline) >= 0) {
		e = h->timer_evt;
		h->timer_armed = 0u;
		queue_push(h, &e);
	}

	/* 2. 排空：每拍上限 HSM_QUEUE_LEN 个，防止 handler 里持续 post 饿死调用方 */
	for (n = 0u; n < HSM_QUEUE_LEN; n++) {
		if (!queue_pop(h, &e)) {
			break;
		}
		(void)hsm_dispatch(h, &e, now);
	}
}

void hsm_post(struct hsm *h, int sig, int32_t arg)
{
	struct hsm_event e;

	if (h == NULL) {
		return;
	}
	e.sig = sig;
	e.arg = arg;
	queue_push(h, &e);
}

void hsm_arm(struct hsm *h, int sig, int32_t arg, uint32_t ms, uint32_t now)
{
	if (h == NULL) {
		return;
	}
	h->timer_evt.sig = sig;
	h->timer_evt.arg = arg;
	h->timer_deadline = now + ms;
	h->timer_armed = 1u;
}

void hsm_disarm(struct hsm *h)
{
	if (h == NULL) {
		return;
	}
	h->timer_armed = 0u;
}

void hsm_force(struct hsm *h, const struct hsm_state *target, uint32_t now)
{
	if (h == NULL || target == NULL) {
		return;
	}
	(void)transition(h, target, now);
}

void hsm_defer(struct hsm *h)
{
	if (h == NULL) {
		return;
	}
	if (h->d_count >= HSM_DEFER_LEN) {
		h->dropped++;
		return;
	}
	h->deferred[h->d_count] = h->current;
	h->d_count++;
}

void hsm_recall(struct hsm *h)
{
	struct hsm_event tmp[HSM_DEFER_LEN + HSM_QUEUE_LEN];
	uint8_t d;
	uint8_t m = 0u;
	uint8_t skip;
	uint8_t i;

	if (h == NULL || h->d_count == 0u) {
		return;
	}

	/* 顺序：先 defer 的在前，再是原队列内容 */
	d = h->d_count;
	for (i = 0u; i < d; i++) {
		tmp[m] = h->deferred[i];
		m++;
	}
	for (i = 0u; i < h->q_count; i++) {
		tmp[m] = h->queue[(uint8_t)((h->q_head + i) % HSM_QUEUE_LEN)];
		m++;
	}

	h->d_count = 0u;
	h->q_head = 0u;
	h->q_tail = 0u;
	h->q_count = 0u;

	/* 超容量时丢最旧的（即 tmp 前面那段） */
	skip = (m > HSM_QUEUE_LEN) ? (uint8_t)(m - HSM_QUEUE_LEN) : 0u;
	h->dropped += skip;
	for (i = skip; i < m; i++) {
		queue_push(h, &tmp[i]);
	}
}

void hsm_set_trace(struct hsm *h,
		   void (*fn)(void *ctx, const struct hsm_state *from, const struct hsm_state *to),
		   void *ctx)
{
	if (h == NULL) {
		return;
	}
	h->trace = fn;
	h->trace_ctx = ctx;
}

const struct hsm_state *hsm_current(const struct hsm *h)
{
	return h != NULL ? h->state : NULL;
}

uint32_t hsm_entered_ms(const struct hsm *h)
{
	return h != NULL ? h->entered_ms : 0u;
}

unsigned hsm_dropped(const struct hsm *h)
{
	return h != NULL ? h->dropped : 0u;
}

unsigned hsm_anomalies(const struct hsm *h)
{
	return h != NULL ? h->anomalies : 0u;
}
