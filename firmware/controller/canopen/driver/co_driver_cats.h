/*
 * CANopenNode 的 SocketCAN 移植层：同步发送，接收帧按 ident/mask 分派。
 * 文件描述符由宿主打开和关闭，本层只借用。
 */

#ifndef CO_DRIVER_CATS_H
#define CO_DRIVER_CATS_H

#include "301/CO_driver.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum co_cats_io_result {
	CO_CATS_IO_ERROR = -1,
	CO_CATS_IO_OK = 0,
	CO_CATS_IO_TIMEOUT = 1,
};

/* CO_CANinit 时作为 CANptr 传进去 */
struct co_cats_can {
	int fd;
	uint8_t tx_error;
};

/* 打开并绑定一个 SocketCAN 接口；成功返回 fd，失败返回 -1 */
int controller_socketcan_open(const char *ifname);

/* 关闭宿主打开的 fd；负值忽略 */
void controller_socketcan_close(int fd);

/* 绑定一个借用 fd，不复制或接管其生命周期 */
void co_cats_can_init(struct co_cats_can *c, int fd);

/* 非阻塞或有界等待读取一帧，转换为 CANopenNode 的消息 */
enum co_cats_io_result co_cats_can_read(struct co_cats_can *c, CO_CANrxMsg_t *msg,
					uint32_t timeout_ms);

/* I/O 线程每拍调用：把一帧交给栈里匹配的接收缓冲 */
void co_cats_can_receive(CO_CANmodule_t *module, const CO_CANrxMsg_t *frame);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CO_DRIVER_CATS_H */
