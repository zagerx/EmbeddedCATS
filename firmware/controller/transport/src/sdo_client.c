/*
 * sdo_client.c — sdo_access 的 SDO 实现
 *
 * 两边各做一半：
 *   I/O 线程（sdo_client_io_poll）：投递请求、逐拍推进、收结果、唤醒等待者；
 *   服务线程（sc_xfer）           ：提交后睡在条件变量上等结果，让出 CPU。
 * 只有 I/O 线程碰协议栈，所以栈本身不需要锁；本文件的锁只保护自己的字段。
 *
 * Author: zager
 */

#include <transport/sdo_client.h>

#include <errno.h>
#include <string.h>
#include <time.h>

#define DEFAULT_TXN_MS 200u
/* 给协议栈自己先判超时留的余量 */
#define WAIT_SLACK_MS 100u

/* 进入时持锁，返回时仍持锁：等结果，并处理超时与外部打断 */
static int sc_wait(struct sdo_client *s, uint32_t timeout_ms);

/* 一次事务（阻塞）：提交 → 等 → 取结果；方向由 dir 决定。
 * 返回 0 成功，-1 失败或通道忙（忙时不更新 last_error）。 */
static int sc_xfer(void *ctx, uint8_t dir, uint16_t index, uint8_t sub, uint32_t in_value,
		   uint8_t size, uint32_t *out_value, uint8_t *out_len, uint32_t timeout_ms);

/* 函数表适配：打断在途事务 */
static void sc_abort(void *ctx);

/* 函数表适配：单调毫秒时基 */
static uint32_t sc_now_ms(void *ctx);

/* 函数表适配：最近一次失败原因 */
static uint32_t sc_last_error(void *ctx);

/**
 * sdo_client_init - 装配一个 SDO 通道
 * @s:      通道对象，调用方持有，生命周期内地址不变
 * @client: CANopenNode 的 SDO 客户端
 *
 * 条件变量绑定 CLOCK_MONOTONIC，与 sc_wait 算 deadline 的时钟一致。
 * 函数表随对象一起备好，调用方拿到稳定指针，不必自己再存一份。
 */
void sdo_client_init(struct sdo_client *s, CO_SDOclient_t *client)
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

	s->co = client;
	s->status = -1;
	s->access.xfer = sc_xfer;
	s->access.abort = sc_abort;
	s->access.now_ms = sc_now_ms;
	s->access.last_error = sc_last_error;
	s->access.ctx = s;
}

/**
 * sdo_client_io_poll - I/O 线程每拍调用：投递请求、推进事务、收尾
 * @s:      通道对象
 * @now_ms: 本拍时刻（毫秒），相邻两拍相减得到协议栈要的 timeDifference_us
 *
 * 全程持锁：栈只在 I/O 线程里被调用，这把锁只串行化本文件的字段。
 * 每拍按「发起 → 推进 → 收尾」三条路径走，收尾点唯一：写结论 + 唤醒等待者。
 */
void sdo_client_io_poll(struct sdo_client *s, uint32_t now_ms)
{
	uint32_t now_us = now_ms * 1000u;
	uint32_t dt_us;
	CO_SDO_abortCode_t abort_code = CO_SDO_AB_NONE;
	CO_SDO_return_t r = CO_SDO_RT_waitingResponse;
	int ok = 0;
	int failed = 0;

	/* 全程持锁：栈是单线程的（收发都在 I/O 线程），这把锁只串行化本文件的字段 */
	pthread_mutex_lock(&s->lock);
	dt_us = (s->last_us == 0u) ? 0u : (uint32_t)(now_us - s->last_us);
	s->last_us = now_us;

	if (s->cancel != 0u) {
		/*
		 * 上一笔被放弃：只收尾协议栈，不碰 state。
		 * abort 的等待方已经把 state 置 IDLE，新事务可能已经排上队（QUEUED）；
		 * 这里再改 state 会把它吞掉，让新事务等到超时。
		 */
		s->cancel = 0u;
		if (s->state == SDO_CLIENT_RUNNING) {
			s->state = SDO_CLIENT_IDLE; /* 超时残留：旧事务确实要收尾 */
		}
		CO_SDOclientClose(s->co);
		pthread_cond_broadcast(&s->cv);
		pthread_mutex_unlock(&s->lock);
		return;
	}

	/* 发起：Initiate 只做准备；写事务要先把值放进发送缓冲 */
	if (s->state == SDO_CLIENT_QUEUED) {
		if (s->dir == SDO_DIR_WRITE) {
			uint8_t buf[4];
			uint8_t i;

			r = CO_SDOclientDownloadInitiate(s->co, s->index, s->sub, (size_t)s->size,
							 (uint16_t)s->txn_ms, false);
			for (i = 0u; i < s->size && i < 4u; i++) {
				buf[i] = (uint8_t)((s->value >> (8u * i)) & 0xFFu);
			}
			if (r >= 0) {
				(void)CO_SDOclientDownloadBufWrite(s->co, buf, (size_t)s->size);
			}
		} else {
			r = CO_SDOclientUploadInitiate(s->co, s->index, s->sub, (uint16_t)s->txn_ms,
						       false);
		}
		if (r < 0) {
			failed = 1;
		} else {
			s->state = SDO_CLIENT_RUNNING;
		}
	}

	/* 推进：发帧 / 收应答 / 判超时都在栈里 */
	if (failed == 0 && s->state == SDO_CLIENT_RUNNING) {
		if (s->dir == SDO_DIR_WRITE) {
			/* download 多一个 bufferPartial（不分段传，固定 false） */
			r = CO_SDOclientDownload(s->co, dt_us, false, false, &abort_code, NULL,
						 NULL);
		} else {
			r = CO_SDOclientUpload(s->co, dt_us, false, &abort_code, NULL, NULL, NULL);
		}
		if (r == CO_SDO_RT_ok_communicationEnd) {
			/* 读事务：把栈缓冲里的数据取出来 */
			if (s->dir != SDO_DIR_WRITE) {
				uint8_t data[4] = {0, 0, 0, 0};
				size_t n = CO_SDOclientUploadBufRead(s->co, data, sizeof(data));

				s->data = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
					  ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
				s->len = (uint8_t)n;
			}
			ok = 1;
		} else if (r < 0) {
			failed = 1;
		}
		/* r > 0：还在进行（waitingResponse 等），下一拍继续 */
	}

	/* 唯一的收尾点：写结论并唤醒等待者 */
	if (ok != 0 || failed != 0) {
		s->status = (ok != 0) ? 0 : -1;
		s->last_code = (ok != 0) ? 0u : (uint32_t)abort_code;
		s->state = SDO_CLIENT_DONE;
		pthread_cond_broadcast(&s->cv);
	}

	pthread_mutex_unlock(&s->lock);
}

/**
 * sdo_client_abort - 任意线程打断在途事务
 * @s: 通道对象；NULL 安全
 *
 * 等待方立刻返回 -1；在途事务由下一次 sdo_client_io_poll() 关掉，
 * 调用者不必等协议栈收尾。
 */
void sdo_client_abort(struct sdo_client *s)
{
	if (s == NULL) {
		return;
	}
	pthread_mutex_lock(&s->lock);
	s->abort = 1u;
	pthread_cond_broadcast(&s->cv);
	pthread_mutex_unlock(&s->lock);
}

/**
 * sdo_client_access - 取本通道的访问接口
 * @s: 通道对象；NULL 安全
 *
 * 指针跟随对象生命周期，可直接交给设备层长期保存。
 *
 * Return: 函数表指针；@s 为 NULL 时返回 NULL。
 */
const struct sdo_access *sdo_client_access(const struct sdo_client *s)
{
	return (s == NULL) ? NULL : &s->access;
}

static int sc_wait(struct sdo_client *s, uint32_t timeout_ms)
{
	struct timespec dl;
	uint32_t wait_ms = timeout_ms + WAIT_SLACK_MS;

	clock_gettime(CLOCK_MONOTONIC, &dl);
	dl.tv_sec += (time_t)(wait_ms / 1000u);
	dl.tv_nsec += (long)(wait_ms % 1000u) * 1000000L;
	if (dl.tv_nsec >= 1000000000L) {
		dl.tv_sec += 1;
		dl.tv_nsec -= 1000000000L;
	}

	/* 到期由 timedwait 自己返回 ETIMEDOUT；这里只处理"结果到了"和"被打断" */
	while (s->state != SDO_CLIENT_DONE && s->abort == 0u) {
		if (pthread_cond_timedwait(&s->cv, &s->lock, &dl) == ETIMEDOUT) {
			break;
		}
	}

	if (s->abort != 0u) {
		/* 外部打断（安全动作）：这一笔作废，事务由 I/O 线程关掉 */
		s->abort = 0u;
		s->cancel = 1u;
		s->state = SDO_CLIENT_IDLE;
		return -1;
	}

	if (s->state != SDO_CLIENT_DONE) {
		s->cancel = 1u; /* 超时：让 I/O 线程收尾，通道不占用 */
		return -1;
	}
	return s->status;
}

static int sc_xfer(void *ctx, uint8_t dir, uint16_t index, uint8_t sub, uint32_t in_value,
		   uint8_t size, uint32_t *out_value, uint8_t *out_len, uint32_t timeout_ms)
{
	struct sdo_client *s = ctx;
	uint32_t txn = (timeout_ms == 0u) ? DEFAULT_TXN_MS : timeout_ms;
	int rc;

	if (s == NULL) {
		return -1;
	}
	pthread_mutex_lock(&s->lock);
	if (s->co == NULL || s->state != SDO_CLIENT_IDLE) {
		pthread_mutex_unlock(&s->lock);
		return -1; /* 通道忙：一笔没结束 */
	}
	s->dir = dir;
	s->index = index;
	s->sub = sub;
	s->value = in_value;
	s->size = size;
	s->txn_ms = (uint16_t)txn;
	s->status = -1;
	/*
	 * 提交前清掉残留的打断请求：abort 只在"提交时已有事务在等"才有意义。
	 * 否则上一笔刚结束时的打断标志会把这一笔立刻打掉，直接报失败。
	 */
	s->abort = 0u;
	s->state = SDO_CLIENT_QUEUED;

	rc = sc_wait(s, txn);
	if (rc == 0 && out_value != NULL) {
		*out_value = s->data;
	}
	if (rc == 0 && out_len != NULL) {
		*out_len = s->len;
	}
	s->state = SDO_CLIENT_IDLE;
	pthread_mutex_unlock(&s->lock);
	return rc;
}

static void sc_abort(void *ctx)
{
	sdo_client_abort((struct sdo_client *)ctx);
}

static uint32_t sc_now_ms(void *ctx)
{
	struct timespec ts;

	(void)ctx;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

static uint32_t sc_last_error(void *ctx)
{
	struct sdo_client *s = ctx;

	return (s == NULL) ? 0u : s->last_code;
}
