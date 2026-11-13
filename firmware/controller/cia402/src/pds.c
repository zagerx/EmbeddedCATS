/*
 * pds.c — 解析 CiA 402 PDS 状态
 *
 * 本文件只包含标准知识，不访问总线、时钟或线程。
 *
 * Author: zager
 */

#include <cia402/pds.h>

#define SW_FAULT_MASK 0x004Fu     /* bit0..3 + bit6 */
#define SW_OPERATION_MASK 0x006Fu /* bit0..3 + bit5 + bit6 */

/**
 * cia402_parse - 将状态字解析为 PDS 状态
 * @statusword: 驱动器 6041 状态字
 *
 * Return: 匹配的 PDS 状态；未知位组合返回 CIA402_STATE_UNKNOWN。
 */
enum cia402_pds_state cia402_parse(uint16_t statusword)
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
