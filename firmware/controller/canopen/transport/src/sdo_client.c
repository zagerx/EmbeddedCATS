/*
 * sdo_client.c — 实现工作线程内的异步 SDO 对象访问
 *
 * start 仅发起一笔上游事务，step 使用事件循环的真实时间差推进协议。
 * 取消由上游 abort 收尾，旧事务未结束时通道保持占用，避免迟到响应串入新事务。
 * 期限在这里统一掌握：start 受理即起算，到期自行 abort，结果报 TIMEOUT。
 *
 * Author: zager
 */

#include <transport/sdo_client.h>

#include <stddef.h>
#include <string.h>

/* 校验并立即发起事务，成功受理返回 0 */
static int sc_start(void *ctx, uint8_t dir, uint16_t index, uint8_t sub, uint32_t value,
		    uint8_t size, uint32_t timeout_ms);

/* 推进并交付结果：DONE / WAITING / IDLE */
static enum sdo_poll sc_poll(void *ctx, struct sdo_result *result);

/* 通过公共取消入口登记取消请求 */
static void sc_abort(void *ctx);

/* 请求取消当前事务，由后续 step 发送协议 abort */
static void sdo_client_abort(struct sdo_client *s);

/**
 * sdo_client_init - 初始化一条异步 SDO 通道
 * @s: 通道对象，生命周期内地址不变
 * @client: 主站分配的 CANopenNode 客户端
 */
void sdo_client_init(struct sdo_client *s, CO_SDOclient_t *client)
{
	memset(s, 0, sizeof(*s));
	s->co = client;
	s->result.status = SDO_STATUS_FAIL;
	s->access.start = sc_start;
	s->access.poll = sc_poll;
	s->access.abort = sc_abort;
	s->access.ctx = s;
}

/**
 * sdo_client_step - 推进一次在途 SDO 事务
 * @s: 通道对象；NULL 安全
 * @dt_us: 距上次推进的真实时间差
 * @timer_next_us: 上游建议的下次唤醒间隔，可为 NULL
 */
void sdo_client_step(struct sdo_client *s, uint32_t dt_us, uint32_t *timer_next_us)
{
	CO_SDO_abortCode_t abort_code = CO_SDO_AB_NONE;
	CO_SDO_return_t r;
	bool_t send_abort;

	if (s == NULL || s->state != SDO_CLIENT_RUNNING) {
		return;
	}
	/* 期限先于协议栈检查：到期由本层 abort，结果口径唯一 */
	if (s->cancel == 0u && s->elapsed_us >= s->timeout_us) {
		s->cancel = 1u;
		s->timed_out = 1u;
	}
	send_abort = s->cancel != 0u;
	if (s->dir == SDO_DIR_WRITE) {
		r = CO_SDOclientDownload(s->co, dt_us, send_abort, false, &abort_code, NULL,
					 timer_next_us);
	} else {
		r = CO_SDOclientUpload(s->co, dt_us, send_abort, &abort_code, NULL, NULL,
				       timer_next_us);
	}
	if (s->elapsed_us < s->timeout_us) {
		s->elapsed_us += dt_us;
	}
	if (r == CO_SDO_RT_ok_communicationEnd && s->dir == SDO_DIR_READ && s->cancel == 0u) {
		uint8_t data[4] = {0u, 0u, 0u, 0u};

		(void)CO_SDOclientUploadBufRead(s->co, data, sizeof(data));
		s->result.value = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
				  ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
	}
	if (r == CO_SDO_RT_ok_communicationEnd || r < 0) {
		CO_SDOclientClose(s->co);
		s->result.status = (r == CO_SDO_RT_ok_communicationEnd && s->cancel == 0u)
					   ? SDO_STATUS_OK
					   : SDO_STATUS_FAIL;
		s->result.abort_code =
			(s->result.status == SDO_STATUS_OK) ? 0u : (uint32_t)abort_code;
		if (s->timed_out != 0u) {
			s->result.status = SDO_STATUS_TIMEOUT;
		}
		if (s->cancel != 0u) {
			s->result.value = 0u;
			if (s->timed_out == 0u) {
				s->result.abort_code = 0u;
			}
		}
		s->state = SDO_CLIENT_DONE;
		s->cancel = 0u;
	}
}

/**
 * sdo_client_access - 获取异步访问接口
 * @s: 通道对象；NULL 安全
 *
 * Return: 与通道同生命周期的接口；NULL 入参返回 NULL。
 */
const struct sdo_access *sdo_client_access(const struct sdo_client *s)
{
	return (s == NULL) ? NULL : &s->access;
}

static int sc_start(void *ctx, uint8_t dir, uint16_t index, uint8_t sub, uint32_t value,
		    uint8_t size, uint32_t timeout_ms)
{
	struct sdo_client *s = ctx;
	CO_SDO_return_t r;

	if (s == NULL || s->co == NULL) {
		return -1;
	}
	if (s->state != SDO_CLIENT_IDLE) {
		return -1; /* 上一笔还没结束：poll 会交付那笔的结果 */
	}
	if (dir > SDO_DIR_WRITE || timeout_ms > 0xFFFFu || (dir == SDO_DIR_READ && size != 0u) ||
	    (dir == SDO_DIR_WRITE && (size == 0u || size > 4u))) {
		/* 参数错误也交一笔失败结果，调用方不会等不到事件 */
		s->result = (struct sdo_result){.status = SDO_STATUS_FAIL};
		s->state = SDO_CLIENT_DONE;
		return -1;
	}
	s->dir = dir;
	s->result = (struct sdo_result){.status = SDO_STATUS_FAIL};
	s->timeout_us = timeout_ms * 1000u;
	s->elapsed_us = 0u;
	s->timed_out = 0u;
	if (dir == SDO_DIR_WRITE) {
		uint8_t buf[4];
		uint8_t i;

		r = CO_SDOclientDownloadInitiate(s->co, index, sub, (size_t)size,
						 (uint16_t)timeout_ms, false);
		for (i = 0u; i < size; i++) {
			buf[i] = (uint8_t)((value >> (8u * i)) & 0xFFu);
		}
		if (r >= 0) {
			(void)CO_SDOclientDownloadBufWrite(s->co, buf, (size_t)size);
		}
	} else {
		r = CO_SDOclientUploadInitiate(s->co, index, sub, (uint16_t)timeout_ms, false);
	}
	s->state = (r < 0) ? SDO_CLIENT_DONE : SDO_CLIENT_RUNNING;
	if (r < 0) {
		CO_SDOclientClose(s->co);
	}
	return 0;
}

static enum sdo_poll sc_poll(void *ctx, struct sdo_result *result)
{
	struct sdo_client *s = ctx;

	if (s == NULL || result == NULL) {
		return SDO_POLL_IDLE;
	}
	if (s->state == SDO_CLIENT_DONE) {
		*result = s->result;
		s->state = SDO_CLIENT_IDLE;
		return SDO_POLL_DONE;
	}
	return (s->state == SDO_CLIENT_RUNNING) ? SDO_POLL_WAITING : SDO_POLL_IDLE;
}

static void sc_abort(void *ctx)
{
	sdo_client_abort((struct sdo_client *)ctx);
}

static void sdo_client_abort(struct sdo_client *s)
{
	if (s == NULL || s->state != SDO_CLIENT_RUNNING) {
		return;
	}
	s->cancel = 1u;
}
