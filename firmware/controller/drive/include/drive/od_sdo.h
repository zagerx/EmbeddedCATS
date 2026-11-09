/*
 * od_sdo.h — od_access 的 SDO 实现（每节点一份）
 *
 * 两个上下文各做一半：
 *   I/O 线程：od_sdo_io_step() —— 把 pending 请求交给 canopen，收结果、唤醒等待者
 *   服务线程：read/write（阻塞）—— 投递请求 + 等完成条件变量
 *
 * 只有 I/O 线程碰 co_node，所以 canopen 层不需要任何锁。
 */

#ifndef DRIVE_BACKENDS_OD_SDO_H
#define DRIVE_BACKENDS_OD_SDO_H

#include <drive/od_access.h>

#include <pthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 只借用指针：本头文件不 include canopen，避免把"传输"漏到上层 */
struct co_master;
struct co_node;

struct od_sdo {
	struct co_master *master;
	struct co_node *node;

	pthread_mutex_t lock;
	pthread_cond_t cv;

	/* 请求 / 执行状态（lock 保护） */
	uint8_t pending; /* 有请求待 I/O 线程发起 */
	uint8_t busy;    /* 已交给 canopen，等应答 */
	uint8_t done;    /* 结果就绪 */
	uint8_t cancel;  /* 调用方放弃：I/O 线程负责关掉在途事务 */
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
};

void od_sdo_init(struct od_sdo *s, struct co_master *master, struct co_node *node);

/* I/O 线程每拍调用 */
void od_sdo_io_step(struct od_sdo *s, uint32_t now_ms);

/* 拿一组 od_access 函数指针（ctx = s） */
struct od_access od_sdo_iface(struct od_sdo *s);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DRIVE_BACKENDS_OD_SDO_H */
