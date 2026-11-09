/*
 * pds.h — DS 402 的 PDS 状态知识（纯函数，没有流程）
 *
 * 三样东西，全是"标准里写着"的：
 *   1. 状态字 → 状态（cia402_parse）
 *   2. 从站要走哪些步（序列表：每步写什么控制字、等什么状态字）
 *   3. 现在走到第几步（cia402_seq_skip：给的起点快进）
 *
 * **这里没有循环、没有 I/O、没有超时**：谁发帧、等多久、失败了怎么办，
 * 都由 device 运行期处理，因此 PDS 可以脱离总线和时钟单独测试。
 *
 * 注意：enum cia402_state 是**从站**状态的投影，主站不维护自己的迁移。
 */

#ifndef CIA402_PDS_H
#define CIA402_PDS_H

#include <cia402/types.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 状态字 → 状态；不匹配任何已知组合返回 CIA402_STATE_UNKNOWN */
enum cia402_state cia402_parse(uint16_t statusword);

/* 名字表（日志/界面用；未知值返回 "UNKNOWN"） */
const char *cia402_state_name(enum cia402_state s);

/*
 * 一步：写 cw 进 6040，然后等"状态字 & exp_mask == exp_value"。
 * 序列表必须从"无输出"向"可操作"单调排列（skip 依赖这个性质）。
 */
struct cia402_seq_step {
	uint16_t cw;        /* 本步写进 6040 的控制字 */
	uint16_t exp_mask;  /* 期望状态字的掩码 */
	uint16_t exp_value; /* 掩码后应等于的值 */
};

/* 使能：0x06 → Ready(0x21) → 0x07 → Switched on(0x23) → 0x0F → OP_ENABLED(0x27) */
const struct cia402_seq_step *cia402_seq_enable(uint8_t *count);

/* 断使能：0x00 → Switch on disabled(0x40)，一步到位 */
const struct cia402_seq_step *cia402_seq_disable(uint8_t *count);

/*
 * 起点快进：返回"第一件还没做到的事"的下标。
 * 已经全部满足时返回 count（调用方据此判"不用发帧，直接成功"）。
 *
 *   0x40 → 0   从 06 开始
 *   0x21 → 1   跳过 06，直接 07
 *   0x23 → 2   直接 0F
 *   0x27 → 3   已完成
 *
 * 实现是"从后往前找已满足的那一步"，所以不要求状态字依次满足前缀。
 */
uint8_t cia402_seq_skip(uint16_t statusword, const struct cia402_seq_step *seq, uint8_t count);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CIA402_PDS_H */
