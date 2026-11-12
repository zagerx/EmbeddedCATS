/*
 * master.c — 管理主站节点表并推进 CANopenNode 收发
 *
 * 节点登记顺序对应对象字典中的 SDO 客户端通道。
 * 协议栈与总线只能由 I/O 线程推进，本层不认识 CiA 402 或业务状态。
 *
 * Author: zager
 */

#include <master/master.h>

#include "OD.h" /* 本机的对象字典（心跳表 0x1016、SDO 客户端 0x1280/0x1281） */

#include <stddef.h>
#include <string.h>

/* 每拍最多抽多少帧：防止总线上灌满无关帧时把状态机饿死 */
#define MASTER_RX_BUDGET 64u

/* CO_CANopenInit 的参数：本机选择 */
#define MASTER_NMT_CONTROL 0u   /* 不自动进 operational，由业务状态机下令 */
#define MASTER_FIRST_HB_MS 500u /* 心跳生产者的首帧延时（本机不生产心跳，占位） */
#define MASTER_SDO_SRV_TIMEOUT_MS 1000u
#define MASTER_SDO_CLI_TIMEOUT_MS 500u

/* 将站号映射到节点表下标，未登记时返回 -1 */
static int master_index(const struct master *m, uint8_t node_id);

/* 立即发送一条 NMT 命令；会直接发帧，只能由 I/O 线程调用 */
static int master_nmt_send(struct master *m, uint8_t node_id, enum master_nmt_cmd cmd);

/**
 * master_init - 绑定总线并初始化 CANopenNode
 * @m: 主站对象
 * @bus: CAN 通道，由调用方持有
 *
 * 初始化过程不主动发送 NMT 帧；@m 为 NULL 时直接返回。
 */
void master_init(struct master *m, struct can_io *bus)
{
	if (m == NULL) {
		return;
	}
	memset(m, 0, sizeof(*m));
	m->bus = bus;
	co_cats_can_init(&m->can, bus);
	pthread_mutex_init(&m->nmt_lock, NULL);

	/* CO_USE_GLOBALS：返回的是静态实例，不动态分配内存 */
	m->co = CO_new(NULL, NULL);
	if (m->co == NULL) {
		return;
	}
	(void)CO_CANinit(m->co, &m->can, 0u); /* 波特率由平台侧配置 */
	(void)CO_CANopenInit(m->co, NULL, NULL, OD, NULL, MASTER_NMT_CONTROL, MASTER_FIRST_HB_MS,
			     MASTER_SDO_SRV_TIMEOUT_MS, MASTER_SDO_CLI_TIMEOUT_MS, false,
			     MASTER_NODE_ID, NULL);
}

/**
 * master_add_node - 登记一个从站并分配对应 SDO 通道
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
		return 0; /* 没有多余的 SDO 通道 */
	}
	if (master_index(m, node_id) >= 0) {
		return 0; /* 重复登记 */
	}
	m->node_id[m->node_count] = node_id;
	m->node_count++;
	return 1;
}

/**
 * master_node_count - 获取已登记从站数量
 * @m: 主站对象；NULL 安全
 *
 * Return: 从站数量；@m 为 NULL 时返回 0。
 */
uint8_t master_node_count(const struct master *m)
{
	return (m == NULL) ? 0u : m->node_count;
}

/**
 * master_node_id - 按登记下标读取从站站号
 * @m: 主站对象；NULL 安全
 * @idx: 登记下标
 *
 * Return: 站号；主站为空或下标越界时返回 0。
 */
uint8_t master_node_id(const struct master *m, uint8_t idx)
{
	if (m == NULL || idx >= m->node_count) {
		return 0u;
	}
	return m->node_id[idx];
}

/**
 * master_sdo - 获取从站对应的 SDO 客户端
 * @m: 主站对象
 * @node_id: 已登记的从站站号
 *
 * Return: SDO 客户端指针；未登记或主站未初始化时返回 NULL。
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
 * master_bus - 获取主站绑定的 CAN 通道
 * @m: 主站对象；NULL 安全
 *
 * Return: 借用通道指针；@m 为 NULL 时返回 NULL。
 */
struct can_io *master_bus(struct master *m)
{
	return (m == NULL) ? NULL : m->bus;
}

/**
 * master_nmt_send - 立即发送一条 NMT 命令
 * @m: 主站对象
 * @node_id: 目标站号，0 表示所有从站
 * @cmd: NMT 命令
 *
 * 经 CO_NMT_sendCommand 直接发帧（不排队），因此只能在 I/O 线程里调用。
 *
 * Return: 0 已交给协议栈发送；-1 主站未初始化或发送失败。
 */
static int master_nmt_send(struct master *m, uint8_t node_id, enum master_nmt_cmd cmd)
{
	if (m == NULL || m->co == NULL || m->co->NMT == NULL) {
		return -1;
	}
	if (CO_NMT_sendCommand(m->co->NMT, (CO_NMT_command_t)cmd, node_id) != CO_ERROR_NO) {
		return -1;
	}
	return 0;
}

/**
 * master_request_nmt - 登记一条待发 NMT 命令
 * @m: 主站对象
 * @node_id: 目标站号，0 表示所有从站
 * @cmd: NMT 命令
 *
 * 任意线程可调。只登记不发送：帧由 I/O 线程在下一拍 master_send_step()
 * 开头发出，保证帧只从 I/O 线程出去。同一拍只保留最后一条登记。
 *
 * Return: 0 已登记；-1 主站未初始化。
 */
int master_request_nmt(struct master *m, uint8_t node_id, enum master_nmt_cmd cmd)
{
	if (m == NULL || m->co == NULL || m->co->NMT == NULL) {
		return -1;
	}
	pthread_mutex_lock(&m->nmt_lock);
	m->nmt_node_id = node_id;
	m->nmt_cmd = cmd;
	m->nmt_pending = 1u;
	pthread_mutex_unlock(&m->nmt_lock);
	return 0;
}

/**
 * master_recv_step - 非阻塞抽取本拍 CAN 帧并交给协议栈
 * @m: 主站对象
 * @now_ms: 本拍单调毫秒时刻
 *
 * 只能由 I/O 线程调用，每拍最多处理 MASTER_RX_BUDGET 帧。
 */
void master_recv_step(struct master *m, uint32_t now_ms)
{
	struct can_io_frame frame;
	uint8_t budget = (uint8_t)MASTER_RX_BUDGET;

	if (m == NULL || m->bus == NULL || m->co == NULL) {
		return;
	}
	m->now_ms = now_ms;

	/* 把总线上已到的帧抽干（不阻塞），逐帧交给栈（软件匹配 ident/mask） */
	while (budget > 0u) {
		if (can_io_recv(m->bus, &frame, 0u) != CAN_IO_OK) {
			break; /* 本拍的帧已经抽完 */
		}
		budget--;
		co_cats_can_receive(m->co->CANmodule, &frame);
	}
}

/**
 * master_send_step - 推进协议栈并发送本拍待发帧
 * @m: 主站对象
 * @now_ms: 本拍单调毫秒时刻
 *
 * 只能由 I/O 线程调用，并与 master_recv_step 保持固定顺序。
 */
void master_send_step(struct master *m, uint32_t now_ms)
{
	uint32_t now_us;
	uint32_t dt_us;
	uint8_t nmt_pending;
	uint8_t nmt_node_id;
	enum master_nmt_cmd nmt_cmd;

	if (m == NULL || m->co == NULL) {
		return;
	}
	m->now_ms = now_ms;
	now_us = now_ms * 1000u;
	dt_us = (m->last_us == 0u) ? 0u : (uint32_t)(now_us - m->last_us);
	m->last_us = now_us;

	/* 先发别线程登记的 NMT：它没有应答，先于本拍的 SDO 帧出去即可 */
	pthread_mutex_lock(&m->nmt_lock);
	nmt_pending = m->nmt_pending;
	nmt_node_id = m->nmt_node_id;
	nmt_cmd = m->nmt_cmd;
	m->nmt_pending = 0u;
	pthread_mutex_unlock(&m->nmt_lock);
	if (nmt_pending != 0u) {
		(void)master_nmt_send(m, nmt_node_id, nmt_cmd);
	}

	/* 栈的统一入口：心跳消费超时、SDO 超时、NMT 处理、发帧都在里面 */
	(void)CO_process(m->co, false, dt_us, NULL);
}

/**
 * master_node_alive - 检查从站心跳是否处于 ACTIVE
 * @m: 主站对象
 * @node_id: 从站站号
 *
 * Return: 0 心跳正常；非 0 未登记、未配置监控、未知或已超时。
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
		return 1; /* 没在 0x1016 里配置：监控不了 → 判不正常 */
	}
	state = CO_HBconsumer_getState(m->co->HBcons, (uint8_t)hb_idx);
	return (state == CO_HBconsumer_ACTIVE) ? 0 : 1;
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
