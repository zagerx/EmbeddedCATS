/*
 * can_io_socketcan.c — can_io.h 的 Linux SocketCAN 实现
 *
 * 四个动作就四件事：write 发、poll+read 收、close 关、clock_gettime 取时间。
 * 平台的"脏东西"（linux/can.h、errno、poll）全部关在本文件里，
 * 出去只有 can_io.h 定义的 4 个返回码。
 */

#include <socketcan/can_io_socketcan.h>

#include <errno.h>
#include <net/if.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <linux/can.h>
#include <linux/can/raw.h>
#include <sys/socket.h>

static enum can_io_status sc_send(void *ctx, const struct can_io_frame *frame)
{
	struct socketcan_dev *dev = ctx;
	struct can_frame cf;

	if (frame->id > 0x7FFu || frame->dlc > CAN_IO_MAX_DLC) {
		return CAN_IO_ERR_INVAL; /* CANopen 只用 11 位标准帧 */
	}

	memset(&cf, 0, sizeof cf);
	cf.can_id = frame->id;
	cf.can_dlc = frame->dlc;
	memcpy(cf.data, frame->data, frame->dlc);
	if (write(dev->fd, &cf, sizeof cf) != (ssize_t)sizeof cf) {
		return CAN_IO_ERR_BUS;
	}
	return CAN_IO_OK;
}

static enum can_io_status sc_recv(void *ctx, struct can_io_frame *frame, uint32_t timeout_ms)
{
	struct socketcan_dev *dev = ctx;
	struct can_frame cf;
	struct pollfd pfd = {.fd = dev->fd, .events = POLLIN};
	int rc;

	rc = poll(&pfd, 1, (int)timeout_ms);
	if (rc == 0) {
		return CAN_IO_TIMEOUT;
	}
	if (rc < 0) {
		return (errno == EINTR) ? CAN_IO_TIMEOUT : CAN_IO_ERR_BUS;
	}
	if (read(dev->fd, &cf, sizeof cf) != (ssize_t)sizeof cf) {
		return CAN_IO_ERR_BUS;
	}
	/* 只认 11 位标准数据帧；错误帧 / 远程帧 / 扩展帧一律丢弃，让上层当"没收到" */
	if ((cf.can_id & (CAN_ERR_FLAG | CAN_RTR_FLAG | CAN_EFF_FLAG)) != 0) {
		return CAN_IO_TIMEOUT;
	}

	frame->id = cf.can_id;
	frame->dlc = cf.can_dlc;
	memcpy(frame->data, cf.data, cf.can_dlc);
	return CAN_IO_OK;
}

static void sc_close(void *ctx)
{
	struct socketcan_dev *dev = ctx;

	if (dev->fd >= 0) {
		close(dev->fd);
		dev->fd = -1;
	}
}

static uint32_t sc_now_ms(void *ctx)
{
	struct timespec ts;

	(void)ctx;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)ts.tv_nsec / 1000000u);
}

static const struct can_io_ops SOCKETCAN_OPS = {
	.send = sc_send,
	.recv = sc_recv,
	.close = sc_close,
	.now_ms = sc_now_ms,
};

enum can_io_status socketcan_open(struct can_io *io, struct socketcan_dev *dev, const char *ifname)
{
	struct sockaddr_can addr;
	unsigned ifindex;
	int fd;

	if (io == NULL || dev == NULL || ifname == NULL) {
		return CAN_IO_ERR_INVAL;
	}

	fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
	if (fd < 0) {
		return CAN_IO_ERR_BUS;
	}

	ifindex = if_nametoindex(ifname); /* 接口不存在或未 up 时这里就失败 */
	if (ifindex == 0) {
		close(fd);
		return CAN_IO_ERR_BUS;
	}

	memset(&addr, 0, sizeof addr);
	addr.can_family = AF_CAN;
	addr.can_ifindex = (int)ifindex;
	if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		close(fd);
		return CAN_IO_ERR_BUS;
	}

	dev->fd = fd;
	io->ops = &SOCKETCAN_OPS;
	io->ctx = dev;
	return CAN_IO_OK;
}
