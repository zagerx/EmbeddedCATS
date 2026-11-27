/*
 * sdo_client_selftest — 真实 SDO 客户端的 CANopenNode 协议桩回归
 *
 * 不创建 CAN 接口；只验证真实客户端的事务所有权与有界收尾。
 *
 * Author: zager
 */

#include <sdo/sdo_client_instance.h>

#include <stdio.h>
#include <string.h>

static int failures;

/* 取走并核对一笔事务结果 */
static void finish(struct sdo_client *client, int status);

/* 测试内部：读 / 写请求的简写 */
static bool start_read(struct sdo_client *c, uint16_t index, uint32_t timeout_ms);
static bool start_write(struct sdo_client *c, uint16_t index, uint32_t value, uint8_t size,
			uint32_t timeout_ms);

#define CHECK(cond)                                                                                \
	do {                                                                                       \
		if (!(cond)) {                                                                     \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                     \
			failures++;                                                                \
		}                                                                                  \
	} while (0)

int main(void)
{
	CO_SDOclient_t co;
	struct sdo_client client;
	struct sdo_result result;

	memset(&co, 0, sizeof(co));
	sdo_client_init(&client, &co);
	co.initiate = CO_SDO_RT_waitingResponse;

	/* 非法参数与底层拒绝均不生成结果 */
	CHECK(!start_write(&client, 0x6040u, 0x0Fu, 0u, 100u));
	CHECK(sdo_client_get_state(&client) == SDO_CLIENT_IDLE);
	co.initiate = CO_SDO_RT_abort;
	CHECK(!start_read(&client, 0x6041u, 100u));
	CHECK(sdo_client_get_state(&client) == SDO_CLIENT_IDLE);
	co.initiate = CO_SDO_RT_waitingResponse;

	/* 正常读完成：小端值解码，取走结果后才释放通道 */
	co.next = CO_SDO_RT_ok_communicationEnd;
	co.read_value = 0x12345678u;
	CHECK(start_read(&client, 0x6041u, 100u));
	CHECK(!start_write(&client, 0x6040u, 0x0Fu, 2u, 100u));
	sdo_client_step(&client, 1000u, NULL);
	CHECK(sdo_client_get_state(&client) == SDO_CLIENT_DONE);
	sdo_client_take_result(&client, &result);
	CHECK(result.status == SDO_STATUS_OK);
	CHECK(result.value == 0x12345678u);
	CHECK(sdo_client_get_state(&client) == SDO_CLIENT_IDLE);

	/* 取消与正常完成同拍发生：尊重已完成事务，不倒改为取消 */
	co.next = CO_SDO_RT_ok_communicationEnd;
	co.read_value = 0x89ABCDEFu;
	CHECK(start_read(&client, 0x6041u, 100u));
	sdo_client_abort(&client);
	sdo_client_step(&client, 1000u, NULL);
	CHECK(sdo_client_get_state(&client) == SDO_CLIENT_DONE);
	sdo_client_take_result(&client, &result);
	CHECK(result.status == SDO_STATUS_OK);
	CHECK(result.value == 0x89ABCDEFu);

	/* 显式取消：协议 abort 结束后交付 CANCELLED，不伪造 abort 码 */
	co.next = CO_SDO_RT_waitingResponse;
	co.abort_code = CO_SDO_AB_TIMEOUT;
	co.closes = 0u;
	CHECK(start_write(&client, 0x6040u, 0x0Fu, 2u, 100u));
	sdo_client_abort(&client);
	sdo_client_step(&client, 1000u, NULL);
	CHECK(sdo_client_get_state(&client) == SDO_CLIENT_RUNNING);
	co.next = CO_SDO_RT_endedWithClientAbort;
	sdo_client_step(&client, 1000u, NULL);
	finish(&client, SDO_STATUS_CANCELLED);
	CHECK(co.closes == 1u);

	/* 自身期限：即使发送缓冲一直满，也有界结束为 TIMEOUT */
	co.next = CO_SDO_RT_transmittBufferFull;
	co.closes = 0u;
	CHECK(start_read(&client, 0x6041u, 2u));
	sdo_client_step(&client, 1000u, NULL);
	CHECK(sdo_client_get_state(&client) == SDO_CLIENT_RUNNING);
	sdo_client_step(&client, 1000u, NULL);
	sdo_client_step(&client, 1000u, NULL);
	sdo_client_step(&client, 1000u, NULL);
	finish(&client, SDO_STATUS_TIMEOUT);
	CHECK(co.closes == 1u);

	/* 主动取消后发送缓冲一直满：本地 Close 释放通道，不宣称协议 abort 成功 */
	co.next = CO_SDO_RT_transmittBufferFull;
	co.closes = 0u;
	CHECK(start_write(&client, 0x6040u, 0x0Fu, 2u, 3u));
	sdo_client_abort(&client);
	sdo_client_step(&client, 1000u, NULL);
	sdo_client_step(&client, 1000u, NULL);
	CHECK(sdo_client_get_state(&client) == SDO_CLIENT_RUNNING);
	sdo_client_step(&client, 1000u, NULL);
	finish(&client, SDO_STATUS_CANCELLED);
	CHECK(co.closes == 1u);

	if (failures == 0) {
		puts("sdo_client_selftest: 全部通过");
	}
	return failures == 0 ? 0 : 1;
}

static bool start_read(struct sdo_client *c, uint16_t index, uint32_t timeout_ms)
{
	const struct sdo_request req = {
		.dir = SDO_DIR_READ,
		.index = index,
		.timeout_ms = timeout_ms,
	};

	return sdo_client_start(c, &req);
}

static bool start_write(struct sdo_client *c, uint16_t index, uint32_t value, uint8_t size,
			uint32_t timeout_ms)
{
	const struct sdo_request req = {
		.dir = SDO_DIR_WRITE,
		.index = index,
		.size = size,
		.value = value,
		.timeout_ms = timeout_ms,
	};

	return sdo_client_start(c, &req);
}

/** CO_SDOclientDownloadInitiate - 返回测试设定的写发起结果 */
CO_SDO_return_t CO_SDOclientDownloadInitiate(CO_SDOclient_t *client, uint16_t index,
					     uint8_t subindex, size_t size, uint16_t timeout_ms,
					     bool block)
{
	(void)index;
	(void)subindex;
	(void)size;
	(void)timeout_ms;
	(void)block;
	return client->initiate;
}

/** CO_SDOclientUploadInitiate - 返回测试设定的读发起结果 */
CO_SDO_return_t CO_SDOclientUploadInitiate(CO_SDOclient_t *client, uint16_t index, uint8_t subindex,
					   uint16_t timeout_ms, bool block)
{
	(void)index;
	(void)subindex;
	(void)timeout_ms;
	(void)block;
	return client->initiate;
}

/** CO_SDOclientDownload - 返回测试设定的写推进结果 */
CO_SDO_return_t CO_SDOclientDownload(CO_SDOclient_t *client, uint32_t dt_us, bool abort, bool block,
				     CO_SDO_abortCode_t *abort_code, size_t *size,
				     uint32_t *timer_next_us)
{
	(void)dt_us;
	(void)abort;
	(void)block;
	(void)size;
	(void)timer_next_us;
	*abort_code = client->abort_code;
	return client->next;
}

/** CO_SDOclientUpload - 返回测试设定的读推进结果 */
CO_SDO_return_t CO_SDOclientUpload(CO_SDOclient_t *client, uint32_t dt_us, bool abort,
				   CO_SDO_abortCode_t *abort_code, size_t *size, size_t *read_size,
				   uint32_t *timer_next_us)
{
	(void)dt_us;
	(void)abort;
	(void)size;
	(void)read_size;
	(void)timer_next_us;
	*abort_code = client->abort_code;
	return client->next;
}

/** CO_SDOclientUploadBufRead - 提供测试设定的小端读取值 */
size_t CO_SDOclientUploadBufRead(CO_SDOclient_t *client, uint8_t *data, size_t size)
{
	uint8_t value[4];
	size_t i;

	for (i = 0u; i < sizeof(value); i++) {
		value[i] = (uint8_t)(client->read_value >> (8u * (uint32_t)i));
	}
	for (i = 0u; i < size; i++) {
		data[i] = value[i];
	}
	return size;
}

/** CO_SDOclientDownloadBufWrite - 接受测试写入的本地缓冲 */
size_t CO_SDOclientDownloadBufWrite(CO_SDOclient_t *client, const uint8_t *data, size_t size)
{
	(void)client;
	(void)data;
	return size;
}

/** CO_SDOclientClose - 记录真实客户端执行本地收尾的次数 */
void CO_SDOclientClose(CO_SDOclient_t *client)
{
	client->closes++;
}

static void finish(struct sdo_client *client, int status)
{
	struct sdo_result result;

	memset(&result, 0xA5, sizeof(result));
	CHECK(sdo_client_get_state(client) == SDO_CLIENT_DONE);
	sdo_client_take_result(client, &result);
	CHECK(result.status == status);
	CHECK(sdo_client_get_state(client) == SDO_CLIENT_IDLE);
}
