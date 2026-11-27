/*
 * sdo_client.c — 实现工作线程内的异步 SDO 对象访问
 *
 * read / write 直接发起事务，step 使用事件循环时间差推进协议。
 * 只有上游受理才进入在途；拒绝不占用通道，也不制造结果。
 * 取消和超时先推进协议 abort；持续发送阻塞时有界本地收尾。
 * 取走结果才释放通道。
 *
 * Author: zager
 */

#include <sdo/sdo_client_instance.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* 取消协议收尾的期限；零期限使用一毫秒，确保每拍至少尝试协议 abort */
static uint32_t sc_abort_timeout(const struct sdo_transaction *t);

/* 饱和累加，避免长期发送阻塞时计时回绕 */
static uint32_t sc_sat_add(uint32_t value, uint32_t delta);

/**
 * sdo_client_init - 初始化一条异步 SDO 通道
 * @s: 通道对象，生命周期内地址不变
 * @client: 主站分配的 CANopenNode 客户端
 */
void sdo_client_init(struct sdo_client *s, CO_SDOclient_t *client)
{
	memset(s, 0, sizeof(*s));
	s->co = client;
	s->txn.result.status = SDO_STATUS_FAIL;
}

/**
 * sdo_client_start - 启动一笔异步事务
 * @s: 通道对象；NULL 视为未受理
 * @req: 请求描述；NULL 视为未受理
 *
 * 读忽略 size / value，固定读取 4 字节。
 *
 * Return: true 已受理，必须取走结果；false 未受理，不生成结果。
 */
bool sdo_client_start(struct sdo_client *s, const struct sdo_request *req)
{
	struct sdo_transaction *t;
	CO_SDO_return_t r;

	if (s == NULL || s->co == NULL || req == NULL || req->timeout_ms > 0xFFFFu ||
	    (req->dir == SDO_DIR_WRITE && (req->size == 0u || req->size > 4u))) {
		fprintf(stderr, "[sdo] 未受理：参数无效或通道未装配\n");
		return false;
	}
	t = &s->txn;
	if (t->state != SDO_CLIENT_IDLE) {
		fprintf(stderr, "[sdo] 未受理：通道有事务在途或结果未取走\n");
		return false;
	}
	if (req->dir == SDO_DIR_WRITE) {
		uint8_t buf[4];
		uint8_t i;

		r = CO_SDOclientDownloadInitiate(s->co, req->index, req->subindex,
						 (size_t)req->size, (uint16_t)req->timeout_ms,
						 false);
		for (i = 0u; i < req->size; i++) {
			buf[i] = (uint8_t)((req->value >> (8u * i)) & 0xFFu);
		}
		if (r >= 0) {
			(void)CO_SDOclientDownloadBufWrite(s->co, buf, (size_t)req->size);
		}
	} else {
		r = CO_SDOclientUploadInitiate(s->co, req->index, req->subindex,
					       (uint16_t)req->timeout_ms, false);
	}
	if (r < 0) {
		fprintf(stderr, "[sdo] 未受理：上游客户端拒绝发起 r=%d\n", (int)r);
		return false;
	}
	t->dir = (uint8_t)req->dir;
	t->state = SDO_CLIENT_RUNNING;
	t->abort_cause = SDO_ABORT_NONE;
	t->timeout_us = req->timeout_ms * 1000u;
	t->elapsed_us = 0u;
	t->abort_elapsed_us = 0u;
	t->result = (struct sdo_result){.status = SDO_STATUS_FAIL};
	return true;
}

/**
 * sdo_client_get_state - 查询通道状态
 * @s: 通道对象；NULL 视为 IDLE
 *
 * Return: IDLE 空闲；RUNNING 在途；DONE 结果待取走。
 */
enum sdo_client_state sdo_client_get_state(struct sdo_client *s)
{
	return (s == NULL) ? SDO_CLIENT_IDLE : (enum sdo_client_state)s->txn.state;
}

/**
 * sdo_client_take_result - 取走一笔已受理事务的结果
 * @s: 通道对象；NULL 安全
 * @result: 结果输出；NULL 安全，非 DONE 时不修改
 *
 * 仅 DONE 时生效：拷出结果并释放通道。
 */
void sdo_client_take_result(struct sdo_client *s, struct sdo_result *result)
{
	if (s == NULL || result == NULL || s->txn.state != SDO_CLIENT_DONE) {
		return;
	}
	*result = s->txn.result;
	s->txn.state = SDO_CLIENT_IDLE;
}

/**
 * sdo_client_abort - 请求取消当前事务
 * @s: 通道对象；NULL 安全
 *
 * 只登记取消；协议收尾或本地期限回退前通道仍被占用，随后交付 CANCELLED。
 */
void sdo_client_abort(struct sdo_client *s)
{
	if (s == NULL || s->txn.state != SDO_CLIENT_RUNNING ||
	    s->txn.abort_cause != SDO_ABORT_NONE) {
		return;
	}
	s->txn.abort_cause = SDO_ABORT_CALLER;
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
	struct sdo_transaction *t;
	CO_SDO_return_t r;
	bool_t send_abort;
	bool abort_expired;

	if (s == NULL) {
		return;
	}
	t = &s->txn;
	if (t->state != SDO_CLIENT_RUNNING) {
		return;
	}
	if (dt_us >= t->timeout_us - t->elapsed_us) {
		t->elapsed_us = t->timeout_us;
	} else {
		t->elapsed_us += dt_us;
	}
	if (t->abort_cause == SDO_ABORT_NONE && t->elapsed_us >= t->timeout_us) {
		t->abort_cause = SDO_ABORT_TIMEOUT;
	}
	send_abort = t->abort_cause != SDO_ABORT_NONE;
	if (t->dir == SDO_DIR_WRITE) {
		r = CO_SDOclientDownload(s->co, dt_us, send_abort, false, &abort_code, NULL,
					 timer_next_us);
	} else {
		r = CO_SDOclientUpload(s->co, dt_us, send_abort, &abort_code, NULL, NULL,
				       timer_next_us);
	}
	if (send_abort) {
		t->abort_elapsed_us = sc_sat_add(t->abort_elapsed_us, dt_us);
	}
	if (r == CO_SDO_RT_ok_communicationEnd && t->dir == SDO_DIR_READ &&
	    t->abort_cause != SDO_ABORT_TIMEOUT) {
		uint8_t data[4] = {0u, 0u, 0u, 0u};

		(void)CO_SDOclientUploadBufRead(s->co, data, sizeof(data));
		t->result.value = (uint32_t)data[0] | ((uint32_t)data[1] << 8u) |
				  ((uint32_t)data[2] << 16u) | ((uint32_t)data[3] << 24u);
	}
	abort_expired = send_abort && t->abort_elapsed_us >= sc_abort_timeout(t);
	if (r == CO_SDO_RT_ok_communicationEnd || r < 0 || abort_expired) {
		CO_SDOclientClose(s->co);
		if (r == CO_SDO_RT_ok_communicationEnd && t->abort_cause != SDO_ABORT_TIMEOUT) {
			/* 本拍收到正常完成，调用方取消不能倒改已经完成的协议事务 */
			t->result.status = SDO_STATUS_OK;
		} else if (t->abort_cause == SDO_ABORT_TIMEOUT) {
			t->result.status = SDO_STATUS_TIMEOUT;
		} else if (t->abort_cause == SDO_ABORT_CALLER) {
			t->result.status = SDO_STATUS_CANCELLED;
		} else {
			t->result.status = SDO_STATUS_FAIL;
		}
		t->result.abort_code = (t->result.status == SDO_STATUS_OK ||
					t->result.status == SDO_STATUS_CANCELLED)
					       ? 0u
					       : (uint32_t)abort_code;
		if (t->result.status != SDO_STATUS_OK) {
			t->result.value = 0u;
		}
		t->state = SDO_CLIENT_DONE;
		t->abort_cause = SDO_ABORT_NONE;
	}
}

static uint32_t sc_abort_timeout(const struct sdo_transaction *t)
{
	return (t->timeout_us != 0u) ? t->timeout_us : 1000u;
}

static uint32_t sc_sat_add(uint32_t value, uint32_t delta)
{
	return (UINT32_MAX - value < delta) ? UINT32_MAX : value + delta;
}
