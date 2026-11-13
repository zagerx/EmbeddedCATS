/*
 * pds.h — DS 402 的 PDS 状态知识（纯函数，没有流程）
 *
 * 只有一件事：状态字 → 状态（cia402_parse）。
 * 使能/断使能要走哪几步是流程，在 states/state_enable.c 与 state_disable.c 里。
 *
 * **这里没有循环、没有 I/O、没有超时**：谁发帧、等多久、失败了怎么办，
 * 都由 device 运行期处理，因此 PDS 可以脱离总线和时钟单独测试。
 *
 * 注意：enum cia402_pds_state 是**从站**状态的投影，主站不维护自己的迁移。
 */

#ifndef CIA402_PDS_H
#define CIA402_PDS_H

#include <cia402/types.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 状态字 → 状态；不匹配任何已知组合返回 CIA402_STATE_UNKNOWN */
enum cia402_pds_state cia402_parse(uint16_t statusword);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CIA402_PDS_H */
