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

#include <transport/sdo_client_instance.h>

#include <stddef.h>
#include <string.h>

/* 校验并由上游立即发起一笔事务；未受理时不修改事务状态 */
static enum sdo_start_result sc_start(struct sdo_client *s, uint8_t dir, uint16_t index,
				      uint8_t sub, uint32_t value, uint8_t size,
				      uint32_t timeout_ms);

/* 取消协议收尾的期限；零期限使用一毫秒，确保每拍至少尝试协议 abort */
static uint32_t sc_abort_timeout(const struct sdo_client *s);

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
	s->result.status = SDO_STATUS_FAIL;
}

/**
 * sdo_client_read - 发起一笔异步读
 * @s: 通道对象；NULL 返回 INVALID
 * @index: 对象索引
 * @subindex: 子索引
 * @timeout_ms: 从事务受理起算的期限
 *
 * Return: ACCEPTED 后必须取走结果；其余返回均不生成结果。
 */
enum sdo_start_result sdo_client_read(struct sdo_client *s, uint16_t index, uint8_t subindex,
				      uint32_t timeout_ms)
{
	return sc_start(s, SDO_DIR_READ, index, subindex, 0u, 0u, timeout_ms);
}

/**
 * sdo_client_write - 发起一笔异步写
 * @s: 通道对象；NULL 返回 INVALID
 * @index: 对象索引
 * @subindex: 子索引
 * @value: 待写值
 * @size: 数据长度，1～4 字节
 * @timeout_ms: 从事务受理起算的期限
 *
 * Return: ACCEPTED 后必须取走结果；其余返回均不生成结果。
 */
enum sdo_start_result sdo_client_write(struct sdo_client *s, uint16_t index, uint8_t subindex,
				       uint32_t value, uint8_t size, uint32_t timeout_ms)
{
	return sc_start(s, SDO_DIR_WRITE, index, subindex, value, size, timeout_ms);
}

/**
 * sdo_client_poll - 查询并取走一笔已受理事务的结果
 * @s: 通道对象；NULL 安全
 * @result: 结果输出；NULL 安全，非 DONE 时不修改
 *
 * Return: DONE 表示结果已取走并释放通道；WAITING 表示尚不能发起新事务。
 */
enum sdo_poll sdo_client_poll(struct sdo_client *s, struct sdo_result *result)
{
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

/**
 * sdo_client_abort - 请求取消当前事务
 * @s: 通道对象；NULL 安全
 *
 * 只登记取消；协议收尾或本地期限回退前通道仍被占用，随后交付 CANCELLED。
 */
void sdo_client_abort(struct sdo_client *s)
{
	if (s == NULL || s->state != SDO_CLIENT_RUNNING) {
		return;
	}
	s->cancel = 1u;
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
	if (dt_us >= s->timeout_us - s->elapsed_us) {
		s->elapsed_us = s->timeout_us;
	} else {
		s->elapsed_us += dt_us;
	}
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
	if (s->cancel != 0u) {
		s->abort_elapsed_us = sc_sat_add(s->abort_elapsed_us, dt_us);
	}
	if (r == CO_SDO_RT_ok_communicationEnd && s->dir == SDO_DIR_READ && s->timed_out == 0u) {
		uint8_t data[4] = {0u, 0u, 0u, 0u};

		(void)CO_SDOclientUploadBufRead(s->co, data, sizeof(data));
		s->result.value = (uint32_t)data[0] | ((uint32_t)data[1] << 8u) |
				  ((uint32_t)data[2] << 16u) | ((uint32_t)data[3] << 24u);
	}
	if (s->cancel != 0u && s->abort_elapsed_us >= sc_abort_timeout(s)) {
		s->abort_timed_out = 1u;
	}
	if (r == CO_SDO_RT_ok_communicationEnd || r < 0 || s->abort_timed_out != 0u) {
		CO_SDOclientClose(s->co);
		if (r == CO_SDO_RT_ok_communicationEnd && s->timed_out == 0u) {
			/* 本拍收到正常完成，取消不能倒改已经完成的协议事务 */
			s->result.status = SDO_STATUS_OK;
		} else if (s->timed_out != 0u) {
			s->result.status = SDO_STATUS_TIMEOUT;
		} else if (s->cancel != 0u) {
			s->result.status = SDO_STATUS_CANCELLED;
		} else {
			s->result.status = SDO_STATUS_FAIL;
		}
		s->result.abort_code = (s->result.status == SDO_STATUS_OK ||
					s->result.status == SDO_STATUS_CANCELLED)
					       ? 0u
					       : (uint32_t)abort_code;
		if (s->result.status != SDO_STATUS_OK) {
			s->result.value = 0u;
		}
		s->state = SDO_CLIENT_DONE;
		s->cancel = 0u;
	}
}

static enum sdo_start_result sc_start(struct sdo_client *s, uint8_t dir, uint16_t index,
				      uint8_t sub, uint32_t value, uint8_t size,
				      uint32_t timeout_ms)
{
	CO_SDO_return_t r;

	if (s == NULL || s->co == NULL || timeout_ms > 0xFFFFu ||
	    (dir == SDO_DIR_WRITE && (size == 0u || size > 4u))) {
		return SDO_START_INVALID;
	}
	if (s->state != SDO_CLIENT_IDLE) {
		return SDO_START_BUSY;
	}
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
	if (r < 0) {
		return SDO_START_INVALID;
	}
	s->dir = dir;
	s->state = SDO_CLIENT_RUNNING;
	s->cancel = 0u;
	s->timed_out = 0u;
	s->timeout_us = timeout_ms * 1000u;
	s->elapsed_us = 0u;
	s->abort_elapsed_us = 0u;
	s->abort_timed_out = 0u;
	s->result = (struct sdo_result){.status = SDO_STATUS_FAIL};
	return SDO_START_ACCEPTED;
}

static uint32_t sc_abort_timeout(const struct sdo_client *s)
{
	return (s->timeout_us != 0u) ? s->timeout_us : 1000u;
}

static uint32_t sc_sat_add(uint32_t value, uint32_t delta)
{
	return (UINT32_MAX - value < delta) ? UINT32_MAX : value + delta;
}
