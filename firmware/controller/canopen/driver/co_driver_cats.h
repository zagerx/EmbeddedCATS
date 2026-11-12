/*
 * CANopenNode 移植层：同步发送，接收帧按 ident/mask 分派。
 * 协议栈只由 I/O 线程推进。
 */

#ifndef CO_DRIVER_CATS_H
#define CO_DRIVER_CATS_H

#include "301/CO_driver.h"

#include <bus/can_io.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CO_CANinit 时作为 CANptr 传进去 */
struct co_cats_can {
	struct can_io *bus;
	/* 最近一次收发是否出错（只用于 CO_CANmodule_process 报状态） */
	uint8_t tx_error;
};

void co_cats_can_init(struct co_cats_can *c, struct can_io *bus);

/* I/O 线程每拍调用：把一帧交给栈里匹配的接收缓冲 */
void co_cats_can_receive(CO_CANmodule_t *module, const struct can_io_frame *frame);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CO_DRIVER_CATS_H */
