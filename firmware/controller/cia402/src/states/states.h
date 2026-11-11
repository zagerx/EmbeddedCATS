/*
 * states.h — 状态函数的公用工具
 *
 * 状态函数一律手写完整签名（与 hsm 内核约定一致），本头只提供两件小事：
 * 从 hsm 成员反推设备上下文、给未处理事件统一上抛父状态的宏。
 *
 * Author: zager
 */

#ifndef CIA402_STATES_H
#define CIA402_STATES_H

#include "../device_internal.h"

#include <stddef.h>

/* 从 hsm 成员指针反推设备对象（hsm 是 struct cia402_device 的成员） */
#define DEV_OF(me_) ((struct cia402_device *)((char *)(me_) - offsetof(struct cia402_device, hsm)))

/* 本层全部状态都挂在 sm_top 下，未处理事件直接上抛内核顶层 */
#define DEV_IGNORE() return SM_SUPER(&sm_top)

/* SIG_DEV_DONE 事件里的传输结果 */
#define DEV_DONE_EVT(e_) (&SM_EVT_CAST(struct dev_done_evt, (e_))->result)

#endif /* CIA402_STATES_H */
