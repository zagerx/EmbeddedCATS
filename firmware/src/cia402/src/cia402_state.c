/*
 * cia402_state.c — 状态字解析（纯函数，不碰总线）
 *
 * DS 402 用几个固定位表示状态机所处状态（掩码见 DS 402 状态字定义）：
 *
 *   bit3 故障           bit6 禁止启动        bit2 可操作
 *   bit1 启动           bit0 准备启动
 *
 * 判据（手册 5.3 与 DS 402 一致）：
 *   故障             xxxx xxxx x0xx 1000   → 0x08（含 x0xx 1111 故障反应中）
 *   禁止启动         xxxx xxxx x1xx 0000   → 0x40
 *   未准备好         xxxx xxxx x0xx 0000   → 0x00
 *   准备启动         xxxx xxxx x01x 0001   → 0x21
 *   已启动           xxxx xxxx x01x 0011   → 0x23
 *   可操作           xxxx xxxx x01x 0111   → 0x27
 */

#include <cia402/cia402.h>

#include "cia402_internal.h"

#define SW_FAULT_MASK 0x004Fu     /* bit0..3 + bit6 */
#define SW_OPERATION_MASK 0x006Fu /* bit0..3 + bit5 + bit6 */

void cia402_take_statusword(struct cia402_node *n)
{
	if (n->data_len > 0U) {
		n->statusword = (uint16_t)(n->data[0] |
					   (uint16_t)(n->data_len > 1U ? n->data[1] : 0U) << 8);
	}
	n->state = cia402_parse(n->statusword);
}

enum cia402_state cia402_parse(uint16_t statusword)
{
	if ((statusword & SW_FAULT_MASK) == 0x0008u || (statusword & SW_FAULT_MASK) == 0x000Fu) {
		return CIA402_FAULT;
	}
	if ((statusword & SW_FAULT_MASK) == 0x0040u) {
		return CIA402_SWITCH_ON_DISABLED;
	}
	if ((statusword & SW_FAULT_MASK) == 0x0000u) {
		return CIA402_NOT_READY;
	}
	if ((statusword & SW_OPERATION_MASK) == 0x0021u) {
		return CIA402_READY;
	}
	if ((statusword & SW_OPERATION_MASK) == 0x0023u) {
		return CIA402_SWITCHED_ON;
	}
	if ((statusword & SW_OPERATION_MASK) == 0x0027u) {
		return CIA402_OP_ENABLED;
	}
	return CIA402_STATE_UNKNOWN;
}
