/*
 * internal.h — canopen 层的内部契约（不对外发布）
 *
 * 只给本层的 .c 用：SDO 报文编解码与事务槽之间的约定。
 * 对外只有 cob.h / nmt.h / hb.h / sdo.h 四个头。
 *
 * 依赖方向（单向，不回头）：
 *   sdo_client.c → sdo_channel.c → sdo_frame.c
 * 本层不碰总线、不看时间源、不知道有主站这回事。
 */

#ifndef CANOPEN_INTERNAL_H
#define CANOPEN_INTERNAL_H

#include <canopen/sdo.h>

#include <stdint.h>

/* 事务槽（纯状态机）的层内接口 */
#include "sdo_channel.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 本层判定的失败原因（与从站回的 abort 共用 code 字段，这里只占两个码） */
#define CO_SDO_ABORT_TIMEOUT 0x05040000u     /* 等应答超时 */
#define CO_SDO_ABORT_UNSUPPORTED 0x05040001u /* 对方要求分段传输（本层不做） */

/* ---- SDO 报文编解码（sdo_frame.c）：纯函数，不碰总线、不看时间 ---- */

enum co_sdo_rsp {
	CO_SDO_RSP_IGNORE = 0, /* 不认识 / 与本次事务无关 */
	CO_SDO_RSP_DATA,       /* 读到值（加速传输） */
	CO_SDO_RSP_ACK,        /* 写已被接受 */
	CO_SDO_RSP_ABORT,      /* 从站报错 */
	CO_SDO_RSP_SEGMENTED   /* 从站要求分段传输：本层不支持 */
};

struct co_sdo_response {
	uint16_t index;
	uint8_t sub;
	uint8_t len; /* DATA 时有效：1..4 */
	uint32_t value;
	uint32_t abort_code;
};

void co_sdo_frame_upload_req(uint8_t out[8], uint16_t index, uint8_t sub);
void co_sdo_frame_download_req(uint8_t out[8], uint16_t index, uint8_t sub, uint32_t value,
			       uint8_t size);
void co_sdo_frame_abort(uint8_t out[8], uint16_t index, uint8_t sub, uint32_t code);
enum co_sdo_rsp co_sdo_frame_parse(const uint8_t in[8], struct co_sdo_response *rsp);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CANOPEN_INTERNAL_H */
