/*
 * nmt.h — NMT 命令（DS 301 的 0x000 广播帧）
 *
 * 只提供"命令 → 帧"的映射：命令字是标准，帧长永远是 2。
 * 什么时候发、发给谁，由主站决定。
 */

#ifndef CANOPEN_NMT_H
#define CANOPEN_NMT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 只列工程真正用到的两个命令；要用 stop / pre-op / reset node 时再加 */
enum co_nmt_cmd {
	CO_NMT_START = 0x01,               /* Start Remote Node */
	CO_NMT_RESET_COMMUNICATION = 0x82, /* Reset Communication */
};

/* 站号 0 = 总线上所有节点 */
#define CO_NMT_ALL_NODES 0u

/* 组一帧 NMT：写满 out[0..1] 并返回 2；参数不合法返回 0 */
uint8_t co_nmt_frame(uint8_t out[2], enum co_nmt_cmd cmd, uint8_t node_id);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CANOPEN_NMT_H */
