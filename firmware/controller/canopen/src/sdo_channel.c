/*
 * sdo_channel.c — 一次 SDO 事务的纯状态机
 *
 * 这里没有总线、没有锁、没有时钟：帧从参数进出，时间由调用方给。
 * 于是"协议对不对"可以完全离线验证（见 apps/tools/canopen_selftest）。
 */

#include "internal.h"

#include <stddef.h>
#include <string.h>

/* 记录"被作废事务"的键：静默窗口内收到同键帧一律当迟到应答丢掉 */
static void stale_set(struct co_sdo_ch *ch, uint32_t now_ms)
{
	ch->stale_index = ch->index;
	ch->stale_sub = ch->sub;
	ch->stale_valid = 1u;
	ch->stale_until_ms = now_ms + CO_SDO_CH_STALE_MS;
}

static int stale_hit(const struct co_sdo_ch *ch, const struct co_sdo_response *rsp, uint32_t now_ms)
{
	if (ch->stale_valid == 0u) {
		return 0;
	}
	if ((int32_t)(now_ms - ch->stale_until_ms) >= 0) {
		return 0; /* 静默期已过 */
	}
	if (rsp->index != ch->stale_index || rsp->sub != ch->stale_sub) {
		return 0;
	}
	/* 当前事务恰好也在访问同一个对象时无法区分（SDO 报文没有事务号），
	 * 只能按"当前事务优先"处理——这是协议固有限制，不是实现偷懒。 */
	if (ch->state == CO_SDO_CH_WAIT_RSP && rsp->index == ch->index && rsp->sub == ch->sub) {
		return 0;
	}
	return 1;
}

void co_sdo_ch_init(struct co_sdo_ch *ch)
{
	if (ch == NULL) {
		return;
	}
	memset(ch, 0, sizeof(*ch));
	ch->state = CO_SDO_CH_IDLE;
}

int co_sdo_ch_submit(struct co_sdo_ch *ch, int is_write, uint16_t index, uint8_t sub,
		     uint32_t value, uint8_t size, uint32_t txn_timeout_ms)
{
	if (ch == NULL) {
		return 1;
	}
	if (ch->state != CO_SDO_CH_IDLE) {
		return 1; /* 单事务：绝不覆盖在途/未取走的结果 */
	}
	if (is_write && (size == 0u || size > 4u)) {
		return 1; /* 本层只做加速传输 */
	}

	ch->is_write = (uint8_t)(is_write ? 1 : 0);
	ch->index = index;
	ch->sub = sub;
	ch->value = value;
	ch->size = size;
	ch->data[0] = ch->data[1] = ch->data[2] = ch->data[3] = 0u;
	ch->len = 0u;
	ch->code = 0u;
	ch->abort_pending = 0u;
	ch->sent_ms = 0u;
	ch->txn_timeout_ms = (txn_timeout_ms == 0u) ? 200u : txn_timeout_ms;
	ch->state = CO_SDO_CH_QUEUED; /* 下一拍 advance 才真正发帧 */
	return 0;
}

void co_sdo_ch_cancel(struct co_sdo_ch *ch)
{
	if (ch == NULL) {
		return;
	}
	switch (ch->state) {
	case CO_SDO_CH_QUEUED:
		/* 还没上总线：直接作废，不需要 abort 帧 */
		ch->code = 0u;
		ch->state = CO_SDO_CH_FAILED;
		return;
	case CO_SDO_CH_WAIT_RSP:
		/* 已发出：必须补发 abort，否则服务端的迟到应答会污染下一笔 */
		ch->code = 0u;
		ch->abort_pending = 1u;
		ch->state = CO_SDO_CH_FAILED;
		return;
	default:
		return;
	}
}

void co_sdo_ch_local_fail(struct co_sdo_ch *ch, uint32_t code)
{
	if (ch == NULL) {
		return;
	}
	if (ch->state == CO_SDO_CH_IDLE || ch->state == CO_SDO_CH_DONE ||
	    ch->state == CO_SDO_CH_FAILED) {
		return; /* 已经有结论，不覆盖 */
	}
	ch->code = code;
	ch->state = CO_SDO_CH_FAILED;
}

enum co_sdo_ch_action co_sdo_ch_advance(struct co_sdo_ch *ch, uint32_t now_ms, uint8_t out[8])
{
	if (ch == NULL || out == NULL) {
		return CO_SDO_CH_ACT_NONE;
	}

	/* 1. 有待补发的 abort（取消时置位） */
	if (ch->abort_pending != 0u) {
		ch->abort_pending = 0u;
		co_sdo_frame_abort(out, ch->index, ch->sub, ch->code);
		stale_set(ch, now_ms);
		return CO_SDO_CH_ACT_SEND_ABORT;
	}

	switch (ch->state) {
	case CO_SDO_CH_QUEUED:
		if (ch->is_write != 0u) {
			co_sdo_frame_download_req(out, ch->index, ch->sub, ch->value, ch->size);
		} else {
			co_sdo_frame_upload_req(out, ch->index, ch->sub);
		}
		ch->sent_ms = now_ms;
		ch->state = CO_SDO_CH_WAIT_RSP;
		return CO_SDO_CH_ACT_SEND_REQ;

	case CO_SDO_CH_WAIT_RSP:
		if ((uint32_t)(now_ms - ch->sent_ms) < ch->txn_timeout_ms) {
			return CO_SDO_CH_ACT_NONE;
		}
		/* 超时：判失败 + 补发 abort + 记陈旧键（迟到的应答不会再被认领） */
		ch->code = CO_SDO_ABORT_TIMEOUT;
		ch->state = CO_SDO_CH_FAILED;
		co_sdo_frame_abort(out, ch->index, ch->sub, ch->code);
		stale_set(ch, now_ms);
		return CO_SDO_CH_ACT_SEND_ABORT;

	default:
		return CO_SDO_CH_ACT_NONE;
	}
}

void co_sdo_ch_on_frame(struct co_sdo_ch *ch, const uint8_t in[8], uint32_t now_ms)
{
	struct co_sdo_response rsp;
	enum co_sdo_rsp kind;

	if (ch == NULL || in == NULL) {
		return;
	}

	kind = co_sdo_frame_parse(in, &rsp);

	if (stale_hit(ch, &rsp, now_ms) != 0) {
		return; /* 被作废事务的迟到应答：丢掉，不算错误 */
	}
	if (ch->state != CO_SDO_CH_WAIT_RSP) {
		return; /* 没有在途事务：一律丢 */
	}
	if (rsp.index != ch->index || rsp.sub != ch->sub) {
		return; /* 不是本事务的应答 */
	}

	switch (kind) {
	case CO_SDO_RSP_ABORT:
		ch->code = rsp.abort_code;
		ch->state = CO_SDO_CH_FAILED;
		return;
	case CO_SDO_RSP_DATA:
		if (ch->is_write != 0u) {
			return; /* 写事务收到数据帧：不是给我们的 */
		}
		ch->data[0] = (uint8_t)(rsp.value);
		ch->data[1] = (uint8_t)(rsp.value >> 8);
		ch->data[2] = (uint8_t)(rsp.value >> 16);
		ch->data[3] = (uint8_t)(rsp.value >> 24);
		ch->len = rsp.len;
		ch->state = CO_SDO_CH_DONE;
		return;
	case CO_SDO_RSP_ACK:
		if (ch->is_write == 0u) {
			return;
		}
		ch->state = CO_SDO_CH_DONE;
		return;
	case CO_SDO_RSP_SEGMENTED:
		/* 只支持加速传输：对方要求分段就是本层的限制，如实上报 */
		ch->code = CO_SDO_ABORT_UNSUPPORTED;
		ch->state = CO_SDO_CH_FAILED;
		return;
	default:
		return; /* 认不出的帧：忽略，让超时兜底 */
	}
}

int co_sdo_ch_take(struct co_sdo_ch *ch, uint8_t *data, uint8_t *len, uint32_t *code)
{
	if (ch == NULL) {
		return 0;
	}
	if (code != NULL) {
		*code = ch->code;
	}
	if (ch->state == CO_SDO_CH_DONE) {
		if (data != NULL) {
			memcpy(data, ch->data, sizeof(ch->data));
		}
		if (len != NULL) {
			*len = ch->len;
		}
		ch->state = CO_SDO_CH_IDLE;
		return 1;
	}
	if (ch->state == CO_SDO_CH_FAILED) {
		ch->state = CO_SDO_CH_IDLE;
		return -1;
	}
	return 0;
}
