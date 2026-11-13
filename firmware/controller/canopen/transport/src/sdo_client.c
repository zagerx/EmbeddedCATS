/*
 * sdo_client.c — 实现工作线程内的异步 SDO 对象访问
 *
 * start 仅发起一笔上游事务，step 使用事件循环的真实时间差推进协议。
 * 取消由上游 abort 收尾，旧事务未结束时通道保持占用，避免迟到响应串入新事务。
 *
 * Author: zager
 */

#include <transport/sdo_client.h>

#include <stddef.h>
#include <string.h>

/* 校验并立即发起事务，成功受理返回 0 */
static int sc_start(void *ctx, uint8_t dir, uint16_t index, uint8_t sub, uint32_t value,
		    uint8_t size, uint32_t timeout_ms);

/* 取走结果并释放通道；尚无结果返回 1 */
static int sc_take_result(void *ctx, struct sdo_result *result);

/* 通过公共取消入口登记取消请求 */
static void sc_abort(void *ctx);

/**
 * sdo_client_init - 初始化一条异步 SDO 通道
 * @s: 通道对象，生命周期内地址不变
 * @client: 主站分配的 CANopenNode 客户端
 */
void sdo_client_init(struct sdo_client *s, CO_SDOclient_t *client)
{
	memset(s, 0, sizeof(*s));
	s->co = client;
	s->result.status = -1;
	s->access.start = sc_start;
	s->access.take_result = sc_take_result;
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
	send_abort = s->cancel != 0u;
	if (s->dir == SDO_DIR_WRITE) {
		r = CO_SDOclientDownload(s->co, dt_us, send_abort, false, &abort_code, NULL,
					 timer_next_us);
	} else {
		r = CO_SDOclientUpload(s->co, dt_us, send_abort, &abort_code, NULL, NULL, timer_next_us);
	}
	if (r == CO_SDO_RT_ok_communicationEnd && s->dir == SDO_DIR_READ && s->cancel == 0u) {
		uint8_t data[4] = {0u, 0u, 0u, 0u};
		size_t n = CO_SDOclientUploadBufRead(s->co, data, sizeof(data));

		s->result.value = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
				  ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
		s->result.len = (uint8_t)n;
	}
	if (r == CO_SDO_RT_ok_communicationEnd || r < 0) {
		CO_SDOclientClose(s->co);
		s->result.status = (r == CO_SDO_RT_ok_communicationEnd && s->cancel == 0u) ? 0 : -1;
		s->result.abort_code = (s->result.status == 0) ? 0u : (uint32_t)abort_code;
		if (s->cancel != 0u) {
			s->result.len = 0u;
			s->result.value = 0u;
			s->result.abort_code = 0u;
		}
		s->state = SDO_CLIENT_DONE;
		s->cancel = 0u;
	}
}

/**
 * sdo_client_abort - 请求取消当前事务
 * @s: 通道对象；NULL 安全
 *
 * 后续 step 发送协议 abort 并生成取消结果，结果取走前不能发起新事务。
 */
void sdo_client_abort(struct sdo_client *s)
{
	if (s == NULL || s->state != SDO_CLIENT_RUNNING) {
		return;
	}
	s->cancel = 1u;
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

	if (s == NULL || s->co == NULL || s->state != SDO_CLIENT_IDLE || dir > SDO_DIR_WRITE ||
	    timeout_ms > 0xFFFFu || (dir == SDO_DIR_READ && size != 0u) ||
	    (dir == SDO_DIR_WRITE && (size == 0u || size > 4u))) {
		return -1;
	}
	s->dir = dir;
	s->result.status = -1;
	s->result.len = 0u;
	s->result.value = 0u;
	s->result.abort_code = 0u;
	if (dir == SDO_DIR_WRITE) {
		uint8_t buf[4];
		uint8_t i;

		r = CO_SDOclientDownloadInitiate(s->co, index, sub, (size_t)size, (uint16_t)timeout_ms,
						 false);
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

static int sc_take_result(void *ctx, struct sdo_result *result)
{
	struct sdo_client *s = ctx;

	if (s == NULL || result == NULL) {
		return -1;
	}
	if (s->state != SDO_CLIENT_DONE) {
		return 1;
	}
	*result = s->result;
	s->state = SDO_CLIENT_IDLE;
	return 0;
}

static void sc_abort(void *ctx)
{
	sdo_client_abort((struct sdo_client *)ctx);
}
