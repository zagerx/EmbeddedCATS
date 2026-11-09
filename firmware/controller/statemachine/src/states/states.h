/*
 * states.h — 状态函数的集中声明 + 共享工具
 *
 * 每个状态一个 .c 文件（state_*.c），这里只放它们的声明，
 * 以及"从 hsm 指针找回 app 上下文"的 container_of 小工具。
 *
 * 层次（父状态在各自的 return SM_SUPER(&父) 里，或见下方注释）：
 *   sm_top               框架提供的顶层（返回 IGNORED，冒泡终点）
 *   └── st_root          公共规则：置了故障码 → FAULT；ESTOP 事件 → ESTOP
 *       ├── st_preparing 等心跳（此时心跳还没有是正常的，所以不挂 ONLINE）
 *       ├── st_online    运行期父状态：心跳丢失 → 置码，由 ROOT 迁 FAULT
 *       │   ├── st_configuring / st_enabling / st_holding / st_moving / st_halting
 *       ├── st_fault     （直接挂 root：安全态不继承运行期动作）
 *       └── st_estop
 *
 * 约定：运行期子状态的 TICK 处理完自己的事后返回 SM_SUPER(&st_online)，
 *       由 st_online 补做公共判定（心跳），再由 st_root 判故障码并决定归属。
 *
 * 内核协议：任何状态收到 SM_SIG_EMPTY 都必须 return SM_SUPER(&父状态)，
 *           内核靠它反查父状态算 LCA。本工程统一把 default 分支写成
 *           return SM_SUPER(&父)，因此 EMPTY / INIT 自然走 default。
 */

#ifndef CONTROL_STATES_H
#define CONTROL_STATES_H

#include "../statemachine_internal.h"

#include <stddef.h>

/* 从 hsm 成员指针反推 app 上下文：各状态函数用它拿到应用上下文 */
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

/* 顶层初始转移：sm_hsm_init() 带着 SM_SIG_INIT 调它，必须返回 SM_TRAN */
sm_ret_t initial(struct sm_hsm *const me, const struct sm_event *const e);

/* 一个状态一个函数，外部链接（跨文件引用 / 内核反查父状态） */
sm_ret_t st_root(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_online(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_preparing(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_configuring(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_enabling(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_holding(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_moving(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_halting(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_fault(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_estop(struct sm_hsm *const me, const struct sm_event *const e);

#endif /* CONTROL_STATES_H */
