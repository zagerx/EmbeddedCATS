/*
 * state_machine.h — DS 402 状态机（纯函数）
 *
 * 只回答三个问题：
 *   1. 这串状态字是什么状态？        ds402_status_parse()
 *   2. 这条命令的控制字是多少？      ds402_cw_for()
 *   3. 从当前状态要去目标状态，下一条命令是什么？ ds402_step()
 *
 * 第 3 条是关键：402 的需求从来不是"按 06→07→0F 走"，而是
 * "从当前状态走到 Operation enabled"。用转移表表达，故障后复位、
 * 断使能后重新使能都自然成立，不需要为每种起点写一套序列。
 *
 * 本层不知道：什么时候该调（业务决定）、怎么发（传输层决定）、超时多少。
 */

#ifndef DS402_STATE_MACHINE_H
#define DS402_STATE_MACHINE_H

#include <ds402/types.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 一次推进建议：发哪条命令、预期进入哪个状态（预期只用于诊断） */
struct ds402_step {
	enum ds402_cmd cmd;      /* DS402_CMD_NONE = 当前状态下走不动 */
	enum ds402_state expect; /* 发出命令后应当读到的状态 */
};

enum ds402_state ds402_status_parse(uint16_t statusword);
uint16_t ds402_cw_for(enum ds402_cmd cmd);
struct ds402_step ds402_step(enum ds402_state cur, enum ds402_state target);

/* 名字表（日志/界面用；未知值返回 "UNKNOWN"） */
const char *ds402_state_name(enum ds402_state s);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DS402_STATE_MACHINE_H */
