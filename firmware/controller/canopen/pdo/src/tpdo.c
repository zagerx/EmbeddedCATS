/*
 * tpdo.c — 在本机闲置的 RPDO 接收槽上接住从站的 TPDO
 *
 * 本机对象字典里 4 路 RPDO（1400h）全部是禁用状态，CO_RPDO_init 仍会为它们
 * 注册接收缓冲，但 COB-ID 落在 0x000，实际收不到任何帧。这里把其中两个槽
 * 改成监听从站 TPDO1/TPDO2 的 COB-ID 并装上自己的回调：
 *   - 不新增对象字典条目，因此不需要 CANopenEditor；
 *   - 不修改 CANopenNode；
 *   - 占用标记就是接收缓冲的 object 指针：一旦改成自己的上下文，下次
 *     查找就找不到它了，于是天然不会重复占用同一个槽。
 *
 * 协议栈重建（NMT 复位）会把接收槽全部清零，因此每次重建后必须重新 attach。
 *
 * Author: zager
 */

#include <pdo/tpdo.h>

#include "OD.h"

#include <stddef.h>
#include <string.h>

/* 装载一台从站时要占用的接收槽数量：TPDO1 + TPDO2 */
#define TPDO_CHANNELS 2u

/* 接收回调：把一帧载荷解进节点的最新值 */
static void tpdo_on_frame(void *object, void *message);

/* 找一个本机闲置的 RPDO 接收槽；成功返回 0 并写入下标 */
static int tpdo_claim_slot(struct master *m, uint32_t *slot);

/* 在指定槽上注册对某个 COB-ID 的接收 */
static int tpdo_register(struct master *m, uint32_t slot, uint16_t cob_id, struct tpdo_rx *ctx);

/* 从载荷里按小端取 32 位 */
static int32_t tpdo_read_s32(const uint8_t *data);

/**
 * tpdo_attach - 在已建好的协议栈上占用两路接收槽
 * @t: 节点对象，生命周期内地址不变
 * @m: 已完成通信初始化的主站
 * @node_id: 从站站号
 *
 * Return: 0 成功；-1 参数无效、没有空闲接收槽或注册失败。
 */
int tpdo_attach(struct tpdo_node *t, struct master *m, uint8_t node_id)
{
	uint8_t channel;

	if (t == NULL || m == NULL || m->co == NULL || node_id == 0u) {
		return -1;
	}
	memset(t, 0, sizeof(*t));
	t->master = m;
	t->node_id = node_id;
	for (channel = 0u; channel < TPDO_CHANNELS; channel++) {
		const uint16_t cob_id = (channel == 0u) ? (uint16_t)(0x180u + node_id)
							: (uint16_t)(0x280u + node_id);

		t->rx[channel].node = t;
		t->rx[channel].channel = channel;
		if (tpdo_claim_slot(m, &t->slot[channel]) != 0) {
			return -1; /* 槽位不足：保持 attached = 0，上层据此判定收不到 */
		}
		if (tpdo_register(m, t->slot[channel], cob_id, &t->rx[channel]) != 0) {
			return -1;
		}
	}
	t->attached = 1u;
	return 0;
}

/**
 * tpdo_frames - 某一路 TPDO 的累计帧数
 * @t: 节点对象；NULL 安全
 * @channel: 0 = TPDO1，1 = TPDO2
 *
 * Return: 累计帧数；入参无效返回 0。
 */
uint32_t tpdo_frames(const struct tpdo_node *t, uint8_t channel)
{
	if (t == NULL || channel >= TPDO_CHANNELS) {
		return 0u;
	}
	return t->frames[channel];
}

/**
 * tpdo_values - 读取最新解出的值
 * @t: 节点对象；NULL 安全
 *
 * Return: 与节点同生命周期的快照指针；NULL 入参返回 NULL。
 */
const struct tpdo_frame *tpdo_values(const struct tpdo_node *t)
{
	return (t == NULL) ? NULL : &t->frame;
}

static int tpdo_claim_slot(struct master *m, uint32_t *slot)
{
	CO_CANmodule_t *can = m->co->CANmodule;
	uint16_t i;

	for (i = 0u; i < can->rxSize; i++) {
		uint16_t rpdo;

		/* 只认同 CO_RPDO_init 留下的私有标记：object 指向本机某个 RPDO */
		for (rpdo = 0u; rpdo < (uint16_t)OD_CNT_RPDO; rpdo++) {
			if (can->rxArray[i].object != (void *)&m->co->RPDO[rpdo]) {
				continue;
			}
			if (can->rxArray[i].ident != 0u) {
				continue; /* 本机 RPDO 正在用这个槽，不能占 */
			}
			*slot = (uint32_t)i;
			return 0;
		}
	}
	return -1;
}

static int tpdo_register(struct master *m, uint32_t slot, uint16_t cob_id, struct tpdo_rx *ctx)
{
	return (CO_CANrxBufferInit(m->co->CANmodule, (uint16_t)slot, cob_id, 0x7FFu, false, ctx,
				   tpdo_on_frame) == CO_ERROR_NO)
		       ? 0
		       : -1;
}

static void tpdo_on_frame(void *object, void *message)
{
	struct tpdo_rx *rx = object;
	struct tpdo_node *t = rx->node;
	const uint8_t *data = CO_CANrxMsg_readData(message);
	const uint8_t length = CO_CANrxMsg_readDLC(message);

	if (rx->channel == 0u) {
		if (length < TPDO1_LENGTH) {
			return; /* 布局与 drive_setup 写的不一致：丢弃，不污染旧值 */
		}
		t->frame.statusword = (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
		t->frame.error_code = (uint16_t)((uint16_t)data[2] | ((uint16_t)data[3] << 8));
		t->frame.mode = data[4];
	} else {
		if (length < TPDO2_LENGTH) {
			return;
		}
		t->frame.position = tpdo_read_s32(&data[0]);
		t->frame.velocity = tpdo_read_s32(&data[4]);
	}
	t->frames[rx->channel]++;
}

static int32_t tpdo_read_s32(const uint8_t *data)
{
	const uint32_t raw = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
			     ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);

	return (int32_t)raw;
}
