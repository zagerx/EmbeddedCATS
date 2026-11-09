/*
 * od_sdo.h — od_access 的 SDO 实现（用 CANopenNode 的 SDO 客户端）
 *
 * 两个上下文各做一半：
 *   I/O 线程：od_sdo_io_step() —— 调 CO_SDOclient_* 推进事务、收结果、唤醒等待者
 *   服务线程：read/write（阻塞）—— 投递请求 + 等条件变量
 *
 * CANopenNode 的 SDO 客户端是**步进式**的（`CO_SDOclientDownload()` 每次返回
 * "还在进行/已结束/失败"），等待由本文件负责：所以栈只在 I/O 线程里被调用，
 * 不需要任何锁（本文件的 lock 只保护自己的字段）。
 */

#ifndef DRIVE_BACKENDS_OD_SDO_H
#define DRIVE_BACKENDS_OD_SDO_H

#include <drive/od_access.h>

#include "CANopen.h" /* CO_SDOclient_t */

#include <pthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct od_sdo {
	CO_SDOclient_t *client; /* 该从站的 SDO 客户端（主站提供） */

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
	uint32_t code;
	uint32_t data;
	uint8_t len;

	/* 最近一次失败原因（给 od_access.last_error 用） */
	uint32_t last_code;

	/* 上一拍的微秒时刻：CO_SDOclient_* 需要 timeDifference_us */
	uint32_t last_us;
};

void od_sdo_init(struct od_sdo *s, CO_SDOclient_t *client);

/* I/O 线程每拍调用：投递请求 / 推进事务 / 收取结果 / 处理放弃 */
void od_sdo_io_step(struct od_sdo *s, uint32_t now_ms);

/* 任意线程：打断在途事务（等待方立刻返回 -1，I/O 线程负责关掉事务） */
void od_sdo_abort(struct od_sdo *s);

/* 拿一组 od_access 函数指针（ctx = s） */
struct od_access od_sdo_iface(struct od_sdo *s);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DRIVE_BACKENDS_OD_SDO_H */
