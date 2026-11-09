/*
 * cob.h — 预定义连接集里本工程用到的那几段 COB-ID
 *
 * 只有协议事实：站号 → COB-ID 的算式，以及"这个 COB-ID 属于哪类帧"的反判。
 * 不知道这台机器上登记过哪几个站（那是主站的表）。
 */

#ifndef CANOPEN_COB_H
#define CANOPEN_COB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CO_COB_NMT 0x000u
#define CO_MAX_NODE_ID 0x7Fu

/* 站号 → COB-ID（站号 1..127 有效） */
uint16_t co_cob_sdo_req(uint8_t node_id); /* 主站 → 从站：0x600 + id */
uint16_t co_cob_sdo_rsp(uint8_t node_id); /* 从站 → 主站：0x580 + id */
uint16_t co_cob_hb(uint8_t node_id);      /* 从站 → 主站：0x700 + id */

/* 反判：是这个方向就填出站号并返回 1，否则返回 0 */
int co_cob_is_sdo_rsp(uint16_t cob_id, uint8_t *node_id);
int co_cob_is_hb(uint16_t cob_id, uint8_t *node_id);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CANOPEN_COB_H */
