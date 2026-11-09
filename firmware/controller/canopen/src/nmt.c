/*
 * nmt.c — NMT 帧组帧（纯函数，不碰总线）
 *
 * DS 301 的 NMT 帧格式固定：data[0] = 命令字，data[1] = 站号（0 = 全站）。
 */

#include <canopen/nmt.h>

#include <canopen/cob.h>

#include <stddef.h>

uint8_t co_nmt_frame(uint8_t out[2], enum co_nmt_cmd cmd, uint8_t node_id)
{
	if (out == NULL || node_id > CO_MAX_NODE_ID) {
		return 0u;
	}
	switch (cmd) {
	case CO_NMT_START:
	case CO_NMT_RESET_COMMUNICATION:
		break;
	default:
		return 0u;
	}
	out[0] = (uint8_t)cmd;
	out[1] = node_id;
	return 2u;
}
