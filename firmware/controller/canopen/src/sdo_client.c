/*
 * sdo_client.c — 把纯状态机（sdo_channel.c）接到一颗从站上
 *
 * 只做三件事：
 *   1. 就绪检查（站号非 0、有发帧回调、参数合法）；
 *   2. 受理 / 取消事务（"忙"由状态机判定，本文件不另立规则）；
 *   3. 每拍把状态机要求的帧发出去（请求帧 / abort 帧）。
 *
 * 组帧、超时、陈旧帧过滤都在 sdo_channel.c，本文件不碰字节；
 * 帧往哪条总线走由注入的 tx 决定，本文件不碰 bus。
 */

#include <canopen/sdo.h>

#include <canopen/cob.h>

#include "sdo_channel.h"

#include <stddef.h>
#include <string.h>

void co_sdo_init(struct co_sdo *s, uint8_t node_id, co_sdo_tx_fn tx, void *tx_ctx)
{
	if (s == NULL) {
		return;
	}
	memset(s, 0, sizeof(*s));
	s->node_id = node_id;
	s->tx = tx;
	s->tx_ctx = tx_ctx;
	co_sdo_ch_init(&s->ch);
}

int co_sdo_ready(const struct co_sdo *s)
{
	return s != NULL && s->tx != NULL && s->node_id != 0U;
}

int co_sdo_read(struct co_sdo *s, uint16_t index, uint8_t sub, uint16_t timeout_ms)
{
	if (!co_sdo_ready(s)) {
		return -1;
	}
	if (co_sdo_ch_submit(&s->ch, 0, index, sub, 0u, 0u, timeout_ms) != 0) {
		return 1; /* 忙：不覆盖在途事务 */
	}
	return 0;
}

int co_sdo_write(struct co_sdo *s, uint16_t index, uint8_t sub, uint32_t value, uint8_t size,
		 uint16_t timeout_ms)
{
	if (!co_sdo_ready(s) || size == 0u || size > 4u) {
		return -1;
	}
	if (co_sdo_ch_submit(&s->ch, 1, index, sub, value, size, timeout_ms) != 0) {
		return 1;
	}
	return 0;
}

int co_sdo_result(struct co_sdo *s, uint8_t *buf, uint8_t *len)
{
	uint32_t code = 0u;
	int r;

	if (s == NULL) {
		return 0;
	}
	r = co_sdo_ch_take(&s->ch, buf, len, &code);
	if (r != 0) {
		s->last_code = code; /* 结论取走后仍可查失败原因 */
	}
	return r;
}

int co_sdo_busy(const struct co_sdo *s)
{
	return s == NULL || s->ch.state != CO_SDO_CH_IDLE;
}

void co_sdo_close(struct co_sdo *s)
{
	if (s == NULL) {
		return;
	}
	/* 在途 → FAILED（必要时下一拍补 abort 帧）；未取走的结果一并丢弃 */
	co_sdo_ch_cancel(&s->ch);
	if (s->ch.state == CO_SDO_CH_DONE || s->ch.state == CO_SDO_CH_FAILED) {
		s->ch.state = CO_SDO_CH_IDLE;
	}
}

uint32_t co_sdo_abort_code(const struct co_sdo *s)
{
	return (s == NULL) ? 0U : s->last_code;
}

void co_sdo_advance(struct co_sdo *s, uint32_t now_ms)
{
	uint8_t frame[8];
	int r;

	if (!co_sdo_ready(s)) {
		return;
	}
	switch (co_sdo_ch_advance(&s->ch, now_ms, frame)) {
	case CO_SDO_CH_ACT_SEND_REQ:
	case CO_SDO_CH_ACT_SEND_ABORT:
		r = s->tx(s->tx_ctx, co_cob_sdo_req(s->node_id), frame);
		if (r != 0 && s->ch.state == CO_SDO_CH_WAIT_RSP) {
			co_sdo_ch_local_fail(&s->ch, 0u); /* 帧没出去：直接判失败 */
		}
		return;
	default:
		return;
	}
}

void co_sdo_on_frame(struct co_sdo *s, const uint8_t data[8], uint32_t now_ms)
{
	if (s == NULL) {
		return;
	}
	co_sdo_ch_on_frame(&s->ch, data, now_ms);
}
