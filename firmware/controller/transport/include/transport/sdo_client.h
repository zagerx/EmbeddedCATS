/*
 * sdo_client.h — sdo_access 的 SDO 实现（用 CANopenNode 的 SDO 客户端）
 *
 * 两个上下文各做一半：
 *   I/O 线程：sdo_client_io_poll() —— 调 CO_SDOclient_* 推进事务、收结果、唤醒等待者
 *   服务线程：xfer（阻塞）—— 投递请求 + 等条件变量
 *
 * CANopenNode 的 SDO 客户端是**步进式**的（`CO_SDOclientDownload()` 每次返回
 * "还在进行/已结束/失败"），等待由本文件负责：所以栈只在 I/O 线程里被调用，
 * 不需要任何锁（本文件的 lock 只保护自己的字段）。
 */

#ifndef TRANSPORT_SDO_CLIENT_H
#define TRANSPORT_SDO_CLIENT_H

#include <transport/sdo_access.h>

#include "CANopen.h" /* CO_SDOclient_t */

#include <pthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sdo_client {
	CO_SDOclient_t *co;       /* 该从站的 SDO 客户端（主站提供） */
	struct sdo_access access; /* 本通道对外的函数表（ctx = 本对象，指针恒定） */

	pthread_mutex_t lock;
	pthread_cond_t cv;

	/* 请求 / 执行状态（lock 保护） */
	uint8_t pending; /* 有请求待 I/O 线程发起 */
	uint8_t busy;    /* 已交给栈，等应答 */
	uint8_t done;    /* 结果就绪 */
	uint8_t cancel;  /* 调用方放弃：I/O 线程负责关掉在途事务 */
	uint8_t abort;   /* 外部打断：等待方立刻返回（安全动作） */
	uint8_t is_write;
	uint16_t index;
	uint8_t sub;
	uint32_t value;
	uint8_t size;
	uint32_t txn_ms;

	int status; /* 0 = 成功，-1 = 失败 */
	uint32_t data;
	uint8_t len;

	/* 最近一次失败原因（给 sdo_access.last_error 用） */
	uint32_t last_code;

	/* 上一拍的微秒时刻：CO_SDOclient_* 需要 timeDifference_us */
	uint32_t last_us;
};

void sdo_client_init(struct sdo_client *s, CO_SDOclient_t *client);

/* I/O 线程每拍调用：投递请求 / 推进事务 / 收取结果 / 处理放弃 */
void sdo_client_io_poll(struct sdo_client *s, uint32_t now_ms);

/* 任意线程：打断在途事务（等待方立刻返回 -1，I/O 线程负责关掉事务） */
void sdo_client_abort(struct sdo_client *s);

/* 本通道的访问接口（生命周期跟随对象，可直接交给设备层长期保存） */
const struct sdo_access *sdo_client_access(const struct sdo_client *s);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TRANSPORT_SDO_CLIENT_H */
