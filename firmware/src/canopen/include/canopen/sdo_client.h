/*
 * sdo_client.h — SDO 客户端（内核层，DS 301 用到的两条 COB-ID：0x600+id /
 * 0x580+id）
 *
 * 本阶段只做"加速传输的读"：一帧请求、一帧应答，值不超过 4 字节。
 * 分段传输（>4 字节、要 toggle 位）暂不支持，遇到会返回 CO_SDO_ERR_PROTOCOL。
 *
 * 本层只认识 struct can_io，不认识 socket、不认识 linux/can —— 平台差异全在边界层。
 *
 * 用法（不用 typedef，显式标签）：
 *     struct co_sdo_client sdo;
 *     struct co_sdo_value v;
 *     co_sdo_init(&sdo, &io, 100);                 // 100 ms 应答超时
 *     if (co_sdo_read(&sdo, 1, 0x1000, 0, &v) == CO_SDO_OK) { ... v.value ... }
 */

#ifndef CANOPEN_SDO_CLIENT_H
#define CANOPEN_SDO_CLIENT_H

#include <canopen/can_io.h>

#ifdef __cplusplus
extern "C" {
#endif

enum co_sdo_result {
	CO_SDO_OK = 0,
	CO_SDO_TIMEOUT,     /* 期限内没等到应答 */
	CO_SDO_ABORT,       /* 从站回了 abort，码在 client->last_abort 里 */
	CO_SDO_ERR_IO,      /* 底层收发失败（总线错误、接口挂了） */
	CO_SDO_ERR_PROTOCOL /* 应答形状不对：分段传输、索引不符、非上传响应… */
};

struct co_sdo_client {
	struct can_io *io;   /* 边界层注入的 CAN 收发能力 */
	uint32_t timeout_ms; /* 一次请求等应答的总时长 */
	uint32_t last_abort; /* 上一次 CO_SDO_ABORT 的 abort 码；0 表示没有 */
};

struct co_sdo_value {
	uint32_t value; /* 读回的值，小端拼装好；高位按 size 无效 */
	uint8_t size;   /* 有效字节数 1..4 */
};

/* c 与 out 由调用方提供且不得为空（本层不做防御性判空，保持逻辑简单） */
void co_sdo_init(struct co_sdo_client *c, struct can_io *io, uint32_t timeout_ms);

enum co_sdo_result co_sdo_read(struct co_sdo_client *c, uint8_t node_id, uint16_t index,
			       uint8_t sub, struct co_sdo_value *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CANOPEN_SDO_CLIENT_H */
