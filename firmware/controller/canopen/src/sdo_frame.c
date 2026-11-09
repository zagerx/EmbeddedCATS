/*
 * sdo_frame.c — SDO 报文的字节布局（纯函数）
 *
 * 只做"结构体 ↔ 8 字节"的转换，不看时间、不碰总线、没有状态。
 * 支持加速传输（1..4 字节）的读与写；分段传输只识别出"对方要求分段"就停。
 *
 * 帧布局（DS 301，客户端 → 服务端 0x600+id / 服务端 → 客户端 0x580+id）：
 *   [0] 命令字   [1..2] index 小端   [3] subindex   [4..7] 数据/abort code
 */

#include "internal.h"

#define CS_UPLOAD_REQ 0x40u
#define CS_DOWNLOAD_1 0x2Fu
#define CS_DOWNLOAD_2 0x2Bu
#define CS_DOWNLOAD_3 0x27u
#define CS_DOWNLOAD_4 0x23u
#define CS_DOWNLOAD_ACK 0x60u
#define CS_ABORT 0x80u

static void put_head(uint8_t out[8], uint8_t cs, uint16_t index, uint8_t sub)
{
	out[0] = cs;
	out[1] = (uint8_t)index;
	out[2] = (uint8_t)(index >> 8);
	out[3] = sub;
}

static void put_value(uint8_t out[8], uint32_t value)
{
	out[4] = (uint8_t)value;
	out[5] = (uint8_t)(value >> 8);
	out[6] = (uint8_t)(value >> 16);
	out[7] = (uint8_t)(value >> 24);
}

void co_sdo_frame_upload_req(uint8_t out[8], uint16_t index, uint8_t sub)
{
	put_head(out, CS_UPLOAD_REQ, index, sub);
	put_value(out, 0u);
}

void co_sdo_frame_download_req(uint8_t out[8], uint16_t index, uint8_t sub, uint32_t value,
			       uint8_t size)
{
	uint8_t cs;

	switch (size) {
	case 1u:
		cs = CS_DOWNLOAD_1;
		break;
	case 2u:
		cs = CS_DOWNLOAD_2;
		break;
	case 3u:
		cs = CS_DOWNLOAD_3;
		break;
	default: /* 4 字节，也兜住 size 传错的情况 */
		cs = CS_DOWNLOAD_4;
		break;
	}
	put_head(out, cs, index, sub);
	put_value(out, value);
}

/*
 * abort 帧：超时或取消时由客户端发给服务端，把该事务在服务端作废。
 * 不发的话，服务端可能稍后才回一帧，被下一笔事务误认成自己的应答。
 */
void co_sdo_frame_abort(uint8_t out[8], uint16_t index, uint8_t sub, uint32_t code)
{
	put_head(out, CS_ABORT, index, sub);
	put_value(out, code);
}

enum co_sdo_rsp co_sdo_frame_parse(const uint8_t in[8], struct co_sdo_response *rsp)
{
	uint8_t cs = in[0];
	uint8_t len = 0u;
	enum co_sdo_rsp kind = CO_SDO_RSP_IGNORE;

	rsp->index = (uint16_t)(in[1] | ((uint16_t)in[2] << 8));
	rsp->sub = in[3];
	rsp->abort_code = 0u;
	rsp->len = 0u;

	switch (cs) {
	case CS_ABORT:
		rsp->abort_code = (uint32_t)in[4] | ((uint32_t)in[5] << 8) |
				  ((uint32_t)in[6] << 16) | ((uint32_t)in[7] << 24);
		kind = CO_SDO_RSP_ABORT;
		break;
	case CS_DOWNLOAD_ACK:
		kind = CO_SDO_RSP_ACK;
		break;
	case 0x4Fu:
		len = 1u;
		kind = CO_SDO_RSP_DATA;
		break;
	case 0x4Bu:
		len = 2u;
		kind = CO_SDO_RSP_DATA;
		break;
	case 0x47u:
		len = 3u;
		kind = CO_SDO_RSP_DATA;
		break;
	case 0x43u:
		len = 4u;
		kind = CO_SDO_RSP_DATA;
		break;
	case 0x41u: /* 有长度指示、分段传输 */
	case 0x40u: /* 无长度指示、分段传输 */
	case 0x42u: /* 块传输 */
		kind = CO_SDO_RSP_SEGMENTED;
		break;
	default:
		kind = CO_SDO_RSP_IGNORE;
		break;
	}

	rsp->len = len;
	if (kind == CO_SDO_RSP_DATA) {
		uint32_t v = 0u;
		uint8_t i;

		for (i = 0u; i < len; i++) {
			v |= (uint32_t)in[4u + i] << (8u * i);
		}
		rsp->value = v;
	}
	return kind;
}
