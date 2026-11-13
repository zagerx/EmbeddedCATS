/*
 * master.h — CANopenLinux 主站对象、节点表与资源生命周期
 *
 * Author: zager
 */

#ifndef MASTER_MASTER_H
#define MASTER_MASTER_H

#include "CANopen.h"
#include "CO_epoll_interface.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MASTER_MAX_NODES 16
#define MASTER_NODE_ID 0x7Fu

enum master_nmt_cmd {
	MASTER_NMT_START = 1,
	MASTER_NMT_RESET_COMM = 130,
};

#define MASTER_NMT_ALL_NODES 0u

struct master {
	CO_t *co;
	CO_epoll_t ep;
	CO_CANptrSocketCan_t can;
	uint8_t node_count;
	uint8_t node_id[MASTER_MAX_NODES];
};

/* 创建并启动主站；失败时释放已申请的资源。成功返回 0，失败返回 -1 */
int master_init(struct master *m, const char *ifname);

/* 停 CAN 并释放协议栈与 epoll；只能在工作线程退出后调用，允许重复调用 */
void master_deinit(struct master *m);

/* 通信复位后重建协议服务，保留动态对象、epoll 和节点表 */
int master_reset_communication(struct master *m);

/* 用 eventfd 唤醒等待中的工作线程；任意线程可调用 */
void master_notify(struct master *m);

/* 第 i 个登记节点绑定第 i 条对象字典 SDO 客户端通道 */
int master_add_node(struct master *m, uint8_t node_id);
uint8_t master_node_count(const struct master *m);
uint8_t master_node_id(const struct master *m, uint8_t idx);
CO_SDOclient_t *master_sdo(struct master *m, uint8_t node_id);

/* 立即请求 NMT，只能由 CANopen 工作线程调用 */
int master_request_nmt(struct master *m, uint8_t node_id, enum master_nmt_cmd cmd);

/* 心跳为 ACTIVE 返回 0，其余情况返回非 0 */
int master_node_alive(const struct master *m, uint8_t node_id);

#ifdef __cplusplus
}
#endif

#endif /* MASTER_MASTER_H */
