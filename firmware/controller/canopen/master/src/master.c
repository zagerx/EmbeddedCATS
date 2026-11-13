/*
 * master.c — 管理 CANopenLinux 主站的生命周期与节点表
 *
 * CANopenLinux 持有 SocketCAN、epoll、timerfd 和 eventfd。协议对象动态分配，
 * 初始化失败逆序回收；所有协议调用只来自同一个 CANopen 工作线程。
 * 关闭 CAN 错误自动处理，避免协议库自行执行 ip link 重启接口。
 *
 * Author: zager
 */

#include <master/master.h>

#include "OD.h"

#include <errno.h>
#include <net/if.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#define MASTER_INTERVAL_US 1000u
#define MASTER_NMT_CONTROL 0u
#define MASTER_FIRST_HB_MS 500u
#define MASTER_SDO_SRV_TIMEOUT_MS 1000u
#define MASTER_SDO_CLI_TIMEOUT_MS 500u

/* 建立 CAN 与协议服务；调用前 CAN 必须处于配置状态 */
static int master_communication_init(struct master *m);

/* 按节点号查登记表，未登记返回 -1 */
static int master_index(const struct master *m, uint8_t node_id);

/**
 * master_init - 创建 CANopenLinux 主站
 * @m: 主站对象
 * @ifname: SocketCAN 接口名，例如 can0
 *
 * 接口和定时器由本对象接管；失败时已释放所有已建资源。
 *
 * Return: 0 成功；-1 参数、接口、内存或协议初始化失败。
 */
int master_init(struct master *m, const char *ifname)
{
	if (m == NULL || ifname == NULL || ifname[0] == '\0' || m->co != NULL) {
		return -1;
	}
	memset(m, 0, sizeof(*m));
	m->ep.epoll_fd = -1;
	m->ep.event_fd = -1;
	m->ep.timer_fd = -1;
	if (CO_epoll_create(&m->ep, MASTER_INTERVAL_US) != CO_ERROR_NO) {
		goto fail;
	}
	m->can.can_ifindex = (int)if_nametoindex(ifname);
	m->can.epoll_fd = m->ep.epoll_fd;
	if (m->can.can_ifindex == 0) {
		goto fail;
	}
	m->co = CO_new(NULL, NULL);
	if (m->co == NULL || master_communication_init(m) != 0) {
		goto fail;
	}
	return 0;

fail:
	master_deinit(m);
	return -1;
}

/**
 * master_deinit - 释放主站全部运行资源
 * @m: 主站对象；NULL 安全
 *
 * 工作线程必须已经退出。静态互斥锁不属于这里，重复调用不会重复关 fd。
 */
void master_deinit(struct master *m)
{
	if (m == NULL) {
		return;
	}
	if (m->co != NULL) {
		CO_CANsetConfigurationMode(&m->can);
		CO_CANmodule_disable(m->co->CANmodule);
		CO_delete(m->co);
		m->co = NULL;
	}
	CO_epoll_close(&m->ep);
	m->node_count = 0u;
	memset(m->node_id, 0, sizeof(m->node_id));
}

/**
 * master_reset_communication - 重建通信服务
 * @m: 正在运行的主站
 *
 * CO_t 及其 SDO 客户端地址不变，已装配的轴不需要重新绑定。
 *
 * Return: 0 成功；-1 主站未初始化或协议重建失败。
 */
int master_reset_communication(struct master *m)
{
	if (m == NULL || m->co == NULL) {
		return -1;
	}
	CO_CANsetConfigurationMode(&m->can);
	CO_CANmodule_disable(m->co->CANmodule);
	return master_communication_init(m);
}

/**
 * master_notify - 唤醒事件循环
 * @m: 主站对象；NULL 安全
 *
 * eventfd 已满表示已有唤醒待处理，不视为失败。
 */
void master_notify(struct master *m)
{
	uint64_t value = 1u;

	if (m == NULL || m->ep.event_fd < 0) {
		return;
	}
	while (write(m->ep.event_fd, &value, sizeof(value)) < 0 && errno == EINTR) {
	}
}

/**
 * master_add_node - 登记从站及其 SDO 通道
 * @m: 主站对象
 * @node_id: 从站站号，范围为 1 至 127
 *
 * Return: 1 登记成功；0 参数无效、重复登记或通道已满。
 */
int master_add_node(struct master *m, uint8_t node_id)
{
	if (m == NULL || m->co == NULL || node_id == 0u || node_id > 0x7Fu) {
		return 0;
	}
	if (m->node_count >= MASTER_MAX_NODES || m->node_count >= (uint8_t)OD_CNT_SDO_CLI) {
		return 0;
	}
	if (master_index(m, node_id) >= 0) {
		return 0;
	}
	m->node_id[m->node_count] = node_id;
	m->node_count++;
	return 1;
}

/**
 * master_node_count - 获取从站数量
 * @m: 主站对象；NULL 安全
 *
 * Return: 已登记数量；NULL 返回 0。
 */
uint8_t master_node_count(const struct master *m)
{
	return (m == NULL) ? 0u : m->node_count;
}

/**
 * master_node_id - 按下标读取从站站号
 * @m: 主站对象
 * @idx: 登记下标
 *
 * Return: 站号；主站为空或下标越界返回 0。
 */
uint8_t master_node_id(const struct master *m, uint8_t idx)
{
	if (m == NULL || idx >= m->node_count) {
		return 0u;
	}
	return m->node_id[idx];
}

/**
 * master_sdo - 获取从站的 SDO 客户端
 * @m: 主站对象
 * @node_id: 已登记的从站站号
 *
 * Return: 客户端指针；未登记或主站未初始化返回 NULL。
 */
CO_SDOclient_t *master_sdo(struct master *m, uint8_t node_id)
{
	int idx = master_index(m, node_id);

	if (idx < 0 || m->co == NULL) {
		return NULL;
	}
	return &m->co->SDOclient[idx];
}

/**
 * master_request_nmt - 发送一条 NMT 命令
 * @m: 主站对象
 * @node_id: 目标站号，0 表示所有从站
 * @cmd: NMT 命令
 *
 * Return: 0 已提交；-1 未初始化、参数错误或发送缓冲忙。
 */
int master_request_nmt(struct master *m, uint8_t node_id, enum master_nmt_cmd cmd)
{
	if (m == NULL || m->co == NULL || m->co->NMT == NULL) {
		return -1;
	}
	return (CO_NMT_sendCommand(m->co->NMT, (CO_NMT_command_t)cmd, node_id) == CO_ERROR_NO) ? 0 : -1;
}

/**
 * master_node_alive - 检查从站心跳
 * @m: 主站对象
 * @node_id: 从站站号
 *
 * Return: 0 ACTIVE；非 0 未登记、未配置、未知或超时。
 */
int master_node_alive(const struct master *m, uint8_t node_id)
{
	int8_t hb_idx;
	CO_HBconsumer_state_t state;

	if (m == NULL || m->co == NULL || m->co->HBcons == NULL) {
		return 1;
	}
	hb_idx = CO_HBconsumer_getIdxByNodeId(m->co->HBcons, node_id);
	if (hb_idx < 0) {
		return 1;
	}
	state = CO_HBconsumer_getState(m->co->HBcons, (uint8_t)hb_idx);
	return (state == CO_HBconsumer_ACTIVE) ? 0 : 1;
}

/**
 * log_printf - 输出 CANopenLinux 的警告与错误
 * @priority: syslog 优先级
 * @format: printf 格式串
 */
void log_printf(int priority, const char *format, ...)
{
	va_list ap;

	if (priority > LOG_WARNING) {
		return;
	}
	va_start(ap, format);
	(void)vfprintf(stderr, format, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
}

static int master_communication_init(struct master *m)
{
	CO_ReturnError_t err;
	uint32_t err_info = 0u;

	err = CO_CANinit(m->co, &m->can, 0u);
	if (err == CO_ERROR_NO) {
		err = CO_CANopenInit(m->co, NULL, NULL, OD, NULL, MASTER_NMT_CONTROL, MASTER_FIRST_HB_MS,
				     MASTER_SDO_SRV_TIMEOUT_MS, MASTER_SDO_CLI_TIMEOUT_MS, false,
				     MASTER_NODE_ID, &err_info);
	}
	if (err == CO_ERROR_NO) {
		CO_epoll_initCANopenMain(&m->ep, m->co);
		err = CO_CANopenInitPDO(m->co, m->co->em, OD, MASTER_NODE_ID, &err_info);
	}
	if (err != CO_ERROR_NO) {
		log_printf(LOG_ERR, "CANopen init failed: %d, OD info 0x%X", (int)err,
			   (unsigned)err_info);
		return -1;
	}
	CO_CANsetNormalMode(m->co->CANmodule);
	return m->co->CANmodule->CANnormal ? 0 : -1;
}

static int master_index(const struct master *m, uint8_t node_id)
{
	uint8_t i;

	if (m == NULL || node_id == 0u) {
		return -1;
	}
	for (i = 0u; i < m->node_count; i++) {
		if (m->node_id[i] == node_id) {
			return (int)i;
		}
	}
	return -1;
}
