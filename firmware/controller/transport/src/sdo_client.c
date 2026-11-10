/*
 * sdo_client.c — 实现异步 SDO 对象访问
 *
 * 服务线程只提交和收取结果，I/O 线程独占 CANopenNode。
 * 互斥锁仅保护通道字段，不用于等待事务完成。
 *
 * Author: zager
 */

#include <transport/sdo_client.h>

#include <stddef.h>
#include <string.h>

static int sc_start(void *ctx, uint8_t dir, uint16_t index, uint8_t sub, uint32_t value,
		    uint8_t size, uint32_t timeout_ms);
static int sc_take_result(void *ctx, struct sdo_result *result);
static void sc_abort(void *ctx);

/**
 * sdo_client_init - 初始化一个异步 SDO 通道
 * @s: 通道对象，生命周期内地址不变
 * @client: 主站分配的 CANopenNode 客户端
 */
void sdo_client_init(struct sdo_client *s, CO_SDOclient_t *client)
{
	memset(s, 0, sizeof(*s));
	pthread_mutex_init(&s->lock, NULL);
	s->co = client;
	s->result.status = -1;
	s->access.start = sc_start;
	s->access.take_result = sc_take_result;
	s->access.abort = sc_abort;
	s->access.ctx = s;
}

/**
 * sdo_client_io_poll - 推进一次 SDO 通道
 * @s: 通道对象
 * @now_ms: 当前单调毫秒时刻
 *
 * 仅 I/O 线程调用；取消、协议完成和协议失败都会生成结果。
 */
void sdo_client_io_poll(struct sdo_client *s, uint32_t now_ms)
{
	uint32_t now_us = now_ms * 1000u;
	uint32_t dt_us;
	CO_SDO_abortCode_t abort_code = CO_SDO_AB_NONE;
	CO_SDO_return_t r = CO_SDO_RT_waitingResponse;
	int failed = 0;
	int complete = 0;

	pthread_mutex_lock(&s->lock);
	dt_us = (s->last_us == 0u) ? 0u : (uint32_t)(now_us - s->last_us);
	s->last_us = now_us;

	if (s->cancel != 0u && (s->state == SDO_CLIENT_QUEUED || s->state == SDO_CLIENT_RUNNING)) {
		s->cancel = 0u;
		CO_SDOclientClose(s->co);
		s->result.status = -1;
		s->result.abort_code = 0u;
		s->state = SDO_CLIENT_DONE;
		pthread_mutex_unlock(&s->lock);
		return;
	}

	if (s->state == SDO_CLIENT_QUEUED) {
		if (s->dir == SDO_DIR_WRITE) {
			uint8_t buf[4];
			uint8_t i;

			r = CO_SDOclientDownloadInitiate(s->co, s->index, s->sub, (size_t)s->size,
							 s->txn_ms, false);
			for (i = 0u; i < s->size && i < 4u; i++) {
				buf[i] = (uint8_t)((s->value >> (8u * i)) & 0xFFu);
			}
			if (r >= 0) {
				(void)CO_SDOclientDownloadBufWrite(s->co, buf, (size_t)s->size);
			}
		} else {
			r = CO_SDOclientUploadInitiate(s->co, s->index, s->sub, s->txn_ms, false);
		}
		if (r < 0) {
			failed = 1;
		} else {
			s->state = SDO_CLIENT_RUNNING;
		}
	}

	if (failed == 0 && s->state == SDO_CLIENT_RUNNING) {
		if (s->dir == SDO_DIR_WRITE) {
			r = CO_SDOclientDownload(s->co, dt_us, false, false, &abort_code, NULL, NULL);
		} else {
			r = CO_SDOclientUpload(s->co, dt_us, false, &abort_code, NULL, NULL, NULL);
		}
		if (r == CO_SDO_RT_ok_communicationEnd) {
			if (s->dir == SDO_DIR_READ) {
				uint8_t data[4] = {0u, 0u, 0u, 0u};
				size_t n = CO_SDOclientUploadBufRead(s->co, data, sizeof(data));

				s->result.value = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
						  ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
				s->result.len = (uint8_t)n;
			}
			complete = 1;
		} else if (r < 0) {
			failed = 1;
		}
	}

	if (complete != 0 || failed != 0) {
		CO_SDOclientClose(s->co);
		s->result.status = (complete != 0) ? 0 : -1;
		s->result.abort_code = (complete != 0) ? 0u : (uint32_t)abort_code;
		s->state = SDO_CLIENT_DONE;
	}
	pthread_mutex_unlock(&s->lock);
}

/**
 * sdo_client_abort - 请求取消当前事务
 * @s: 通道对象；NULL 安全
 *
 * 不等待协议栈；取消结果由后续 I/O 节拍生成。
 */
void sdo_client_abort(struct sdo_client *s)
{
	if (s == NULL) {
		return;
	}
	pthread_mutex_lock(&s->lock);
	if (s->state == SDO_CLIENT_QUEUED || s->state == SDO_CLIENT_RUNNING) {
		s->cancel = 1u;
	}
	pthread_mutex_unlock(&s->lock);
}

/**
 * sdo_client_access - 获取通道的异步访问接口
 * @s: 通道对象；NULL 安全
 *
 * Return: 与通道同生命周期的接口指针；NULL 入参返回 NULL。
 */
const struct sdo_access *sdo_client_access(const struct sdo_client *s)
{
	return (s == NULL) ? NULL : &s->access;
}

static int sc_start(void *ctx, uint8_t dir, uint16_t index, uint8_t sub, uint32_t value,
		    uint8_t size, uint32_t timeout_ms)
{
	struct sdo_client *s = ctx;

	if (s == NULL || dir > SDO_DIR_WRITE || timeout_ms > 0xFFFFu ||
	    (dir == SDO_DIR_READ && size != 0u) ||
	    (dir == SDO_DIR_WRITE && (size == 0u || size > 4u))) {
		return -1;
	}

	pthread_mutex_lock(&s->lock);
	if (s->co == NULL || s->state != SDO_CLIENT_IDLE) {
		pthread_mutex_unlock(&s->lock);
		return -1;
	}
	s->dir = dir;
	s->index = index;
	s->sub = sub;
	s->value = value;
	s->size = size;
	s->txn_ms = (uint16_t)timeout_ms;
	s->result.status = -1;
	s->result.len = 0u;
	s->result.value = 0u;
	s->result.abort_code = 0u;
	s->state = SDO_CLIENT_QUEUED;
	pthread_mutex_unlock(&s->lock);
	return 0;
}

static int sc_take_result(void *ctx, struct sdo_result *result)
{
	struct sdo_client *s = ctx;

	if (s == NULL || result == NULL) {
		return -1;
	}
	pthread_mutex_lock(&s->lock);
	if (s->state != SDO_CLIENT_DONE) {
		pthread_mutex_unlock(&s->lock);
		return 1;
	}
	*result = s->result;
	s->state = SDO_CLIENT_IDLE;
	pthread_mutex_unlock(&s->lock);
	return 0;
}

static void sc_abort(void *ctx)
{
	sdo_client_abort((struct sdo_client *)ctx);
}
