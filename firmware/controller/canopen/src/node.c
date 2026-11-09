/*
 * node.c — 节点句柄：把纯状态机（sdo_channel.c）接到总线上
 *
 * 只做三件事：
 *   1. 就绪检查（有主站、站号非 0、参数合法）；
 *   2. 受理 / 取消事务（"忙"由状态机判定，本文件不另立规则）；
 *   3. 每拍把状态机要求的帧发出去（请求帧 / abort 帧）。
 *
 * 组帧、超时、陈旧帧过滤都在 sdo_channel.c，本文件不碰字节。
 */

#include "internal.h"

#include <stddef.h>
#include <string.h>

void co_node_init(struct co_node *n, struct co_master *m, uint8_t node_id)
{
	memset(n, 0, sizeof(*n));
	n->master = m;
	n->node_id = node_id;
	co_sdo_ch_init(&n->ch);
}

int co_node_ready(const struct co_node *n)
{
	return n != NULL && n->master != NULL && n->node_id != 0U;
}

int co_node_read(struct co_node *n, uint16_t index, uint8_t sub, uint16_t timeout_ms)
{
	if (!co_node_ready(n)) {
		return -1;
	}
	if (co_sdo_ch_submit(&n->ch, 0, index, sub, 0u, 0u, timeout_ms) != 0) {
		return 1; /* 忙：不覆盖在途事务 */
	}
	return 0;
}

int co_node_write(struct co_node *n, uint16_t index, uint8_t sub, uint32_t value, uint8_t size,
		  uint16_t timeout_ms)
{
	if (!co_node_ready(n) || size == 0u || size > 4u) {
		return -1;
	}
	if (co_sdo_ch_submit(&n->ch, 1, index, sub, value, size, timeout_ms) != 0) {
		return 1;
	}
	return 0;
}

int co_node_result(struct co_node *n, uint8_t *buf, uint8_t *len)
{
	uint32_t code = 0u;
	int r;

	if (n == NULL) {
		return 0;
	}
	r = co_sdo_ch_take(&n->ch, buf, len, &code);
	if (r != 0) {
		n->last_code = code; /* 结论取走后仍可查失败原因 */
	}
	return r;
}

int co_node_busy(const struct co_node *n)
{
	return n == NULL || n->ch.state != CO_SDO_CH_IDLE;
}

void co_node_close(struct co_node *n)
{
	if (n == NULL) {
		return;
	}
	/* 在途 → FAILED（必要时下一拍补 abort 帧）；未取走的结果一并丢弃 */
	co_sdo_ch_cancel(&n->ch);
	if (n->ch.state == CO_SDO_CH_DONE || n->ch.state == CO_SDO_CH_FAILED) {
		n->ch.state = CO_SDO_CH_IDLE;
	}
}

uint32_t co_node_abort_code(const struct co_node *n)
{
	return (n == NULL) ? 0U : n->last_code;
}

/* ---- 主站每拍调用：把状态机要求的帧发出去 ---- */

void co_node_advance(struct co_node *n, uint32_t now_ms)
{
	uint8_t frame[8];
	int r;

	if (!co_node_ready(n)) {
		return;
	}
	switch (co_sdo_ch_advance(&n->ch, now_ms, frame)) {
	case CO_SDO_CH_ACT_SEND_REQ:
	case CO_SDO_CH_ACT_SEND_ABORT:
		r = co_master_send(n->master, (uint16_t)(0x600u + n->node_id), frame, 8u);
		if (r != 0 && n->ch.state == CO_SDO_CH_WAIT_RSP) {
			co_sdo_ch_local_fail(&n->ch, 0u); /* 帧没出去：直接判失败 */
		}
		return;
	default:
		return;
	}
}

void co_node_on_frame(struct co_node *n, const uint8_t data[8], uint32_t now_ms)
{
	if (n == NULL) {
		return;
	}
	co_sdo_ch_on_frame(&n->ch, data, now_ms);
}
