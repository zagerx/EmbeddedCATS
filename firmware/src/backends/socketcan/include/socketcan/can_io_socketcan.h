/*
 * can_io_socketcan.h — can_io.h 的 Linux SocketCAN 实现（边界层）
 *
 * 这是**唯一允许出现 linux/can.h 的模块**。内核只认识 can_io.h，
 * 装配时由集成层调用 socketcan_open() 把实现塞进 struct can_io。
 *
 * 用法（存储由调用方提供，全程不用 malloc）：
 *
 *     static struct socketcan_dev dev;
 *     struct can_io io;
 *     if (socketcan_open(&io, &dev, "can0") != CAN_IO_OK) { ... }
 *     ... 把 &io 交给内核 ...
 *     can_io_close(&io);
 *
 * 编译注意：需要 -D_POSIX_C_SOURCE=200809L（clock_gettime / poll）。
 */

#ifndef SOCKETCAN_CAN_IO_SOCKETCAN_H
#define SOCKETCAN_CAN_IO_SOCKETCAN_H

#include <canopen/can_io.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SocketCAN 通道句柄。字段只为让调用方分配存储，不当公共接口使用 */
struct socketcan_dev {
	int fd;
};

/* 打开并绑定 ifname（如 "can0"）；成功时填好 io->ops 与 io->ctx */
enum can_io_status socketcan_open(struct can_io *io, struct socketcan_dev *dev, const char *ifname);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SOCKETCAN_CAN_IO_SOCKETCAN_H */
