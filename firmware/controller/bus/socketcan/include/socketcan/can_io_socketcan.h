/* Linux SocketCAN 后端；调用方提供通道与句柄存储。 */

#ifndef SOCKETCAN_CAN_IO_SOCKETCAN_H
#define SOCKETCAN_CAN_IO_SOCKETCAN_H

#include <bus/can_io.h>

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
