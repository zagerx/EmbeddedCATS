/*
 * od_sdo.c — 用 SDO 实现 od_access
 *
 * 关键点：**阻塞的是服务线程，不是 I/O 线程**。
 * 服务线程等条件变量（睡眠，不占 CPU）；I/O 线程负责真正的收发与超时。
 *
 * 调用方超时后置 cancel，由 I/O 线程调 co_sdo_close() 放弃在途事务，
 * 通道可以立刻复用，不会卡住。
 */

#include <drive/od_sdo.h>

#include <canopen/sdo.h>

#include <errno.h>
#include <string.h>
#include <time.h>

#define OD_SDO_DEFAULT_TXN_MS 200u
/* 等条件变量时比事务超时多留一点：让 canopen 自己先判超时并回 abort */
#define OD_SDO_WAIT_SLACK_MS 100u

static void deadline_after(struct timespec *ts, uint32_t ms)
{
	clock_gettime(CLOCK_MONOTONIC, ts);
	ts->tv_sec += (time_t)(ms / 1000u);
	ts->tv_nsec += (long)(ms % 1000u) * 1000000L;
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_sec += 1;
		ts->tv_nsec -= 1000000000L;
	}
}

void od_sdo_init(struct od_sdo *s, struct co_sdo *sdo)
{
	pthread_condattr_t ca;

	memset(s, 0, sizeof(*s));
	pthread_mutex_init(&s->lock, NULL);
	/* 条件变量必须用单调时钟：deadline 也是按 CLOCK_MONOTONIC 算的。
	 * 默认是 CLOCK_REALTIME（epoch），两者一混，timedwait 会立刻超时。 */
	pthread_condattr_init(&ca);
	pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
	pthread_cond_init(&s->cv, &ca);
	pthread_condattr_destroy(&ca);

	s->sdo = sdo;
	s->status = -1;
}

/* ---------------- I/O 线程侧 ---------------- */

void od_sdo_io_step(struct od_sdo *s, uint32_t now_ms)
{
	uint8_t data[4] = {0, 0, 0, 0};
	uint8_t len = 0u;
	int r;

	(void)now_ms;
	pthread_mutex_lock(&s->lock);

	if (s->cancel != 0u) {
		/* 调用方已放弃：关掉在途事务，通道立刻可用 */
		s->cancel = 0u;
		s->pending = 0u;
		s->busy = 0u;
		s->done = 0u;
		pthread_mutex_unlock(&s->lock);
		co_sdo_close(s->sdo);
		return;
	}

	if (s->pending != 0u && s->busy == 0u && s->done == 0u) {
		if (s->is_write != 0u) {
			r = co_sdo_write(s->sdo, s->index, s->sub, s->value, s->size, s->txn_ms);
		} else {
			r = co_sdo_read(s->sdo, s->index, s->sub, (uint16_t)s->txn_ms);
		}
		if (r == 0) {
			s->pending = 0u;
			s->busy = 1u;
		} else if (r < 0) {
			s->pending = 0u;
			s->done = 1u;
			s->status = -1;
			s->last_code = 0u;
			pthread_cond_broadcast(&s->cv);
		}
		/* r > 0：通道还忙（上一笔没取走结果），下拍再试 */
	}

	if (s->busy != 0u) {
		r = co_sdo_result(s->sdo, data, &len);
		if (r != 0) {
			s->busy = 0u;
			s->done = 1u;
			s->status = (r > 0) ? 0 : -1;
			s->code = (r > 0) ? 0u : co_sdo_abort_code(s->sdo);
			s->last_code = s->code;
			if (r > 0) {
				s->data = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
					  ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
				s->len = len;
			}
			pthread_cond_broadcast(&s->cv);
		}
	}

	pthread_mutex_unlock(&s->lock);
}

/* ---------------- 服务线程侧（阻塞） ---------------- */

static int od_wait(struct od_sdo *s, uint32_t timeout_ms)
{
	struct timespec dl;
	struct timespec now;
	int rc;

	deadline_after(&dl, timeout_ms + OD_SDO_WAIT_SLACK_MS);
	/* 每一轮重新算剩余时间：abort 可能已经很晚了，不能再用固定的 dl */
	for (;;) {
		long ms;

		if (s->done != 0u) {
			break;
		}
		if (s->abort != 0u) {
			/* 外部打断（安全动作）：这一笔作废，让 I/O 线程关掉事务 */
			s->abort = 0u;
			s->cancel = 1u;
			s->pending = 0u;
			s->done = 0u;
			return -1;
		}
		clock_gettime(CLOCK_MONOTONIC, &now);
		ms = (long)(dl.tv_sec - now.tv_sec) * 1000L +
		     (long)(dl.tv_nsec - now.tv_nsec) / 1000000L;
		if (ms <= 0) {
			s->cancel = 1u; /* 让 I/O 线程收尾，通道不占用 */
			return -1;
		}
		if (pthread_cond_timedwait(&s->cv, &s->lock, &dl) == ETIMEDOUT) {
			s->cancel = 1u;
			return -1;
		}
	}
	rc = s->status;
	s->done = 0u;
	return rc;
}

static int od_do(struct od_sdo *s, int is_write, uint16_t index, uint8_t sub, uint32_t value,
		 uint8_t size, uint32_t *out, uint8_t *len, uint32_t timeout_ms)
{
	uint32_t txn = (timeout_ms == 0u) ? OD_SDO_DEFAULT_TXN_MS : timeout_ms;
	int rc;

	pthread_mutex_lock(&s->lock);
	if (s->sdo == NULL || s->pending != 0u || s->busy != 0u || s->done != 0u) {
		pthread_mutex_unlock(&s->lock);
		return -1; /* 通道忙：一笔没结束 */
	}
	s->is_write = (uint8_t)(is_write ? 1 : 0);
	s->index = index;
	s->sub = sub;
	s->value = value;
	s->size = size;
	s->txn_ms = txn;
	s->status = -1;
	s->pending = 1u;

	rc = od_wait(s, txn);
	if (rc == 0 && out != NULL) {
		*out = s->data;
	}
	if (rc == 0 && len != NULL) {
		*len = s->len;
	}
	pthread_mutex_unlock(&s->lock);
	return rc;
}

static int od_read(void *ctx, uint16_t index, uint8_t sub, uint32_t *value, uint8_t *len,
		   uint32_t timeout_ms)
{
	return od_do((struct od_sdo *)ctx, 0, index, sub, 0u, 0u, value, len, timeout_ms);
}

static int od_write(void *ctx, uint16_t index, uint8_t sub, uint32_t value, uint8_t size,
		    uint32_t timeout_ms)
{
	return od_do((struct od_sdo *)ctx, 1, index, sub, value, size, NULL, NULL, timeout_ms);
}

static uint32_t od_now_ms(void *ctx)
{
	struct timespec ts;

	(void)ctx;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

static uint32_t od_last_error(void *ctx)
{
	struct od_sdo *s = ctx;

	return s->last_code;
}

/* 任意线程：打断在途事务（等待方立刻返回，事务由 I/O 线程关掉） */
void od_sdo_abort(struct od_sdo *s)
{
	if (s == NULL) {
		return;
	}
	pthread_mutex_lock(&s->lock);
	s->abort = 1u;
	pthread_cond_broadcast(&s->cv);
	pthread_mutex_unlock(&s->lock);
}

static void od_abort(void *ctx)
{
	od_sdo_abort((struct od_sdo *)ctx);
}

struct od_access od_sdo_iface(struct od_sdo *s)
{
	struct od_access a;

	a.read = od_read;
	a.write = od_write;
	a.abort = od_abort;
	a.now_ms = od_now_ms;
	a.last_error = od_last_error;
	a.ctx = s;
	return a;
}
