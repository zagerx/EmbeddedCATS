/*
 * state_machine.c — DS 402 状态机实现（纯函数，可离线单测）
 *
 * 状态字判据（DS 402 / 手册 5.3）：
 *   故障         (sw & 0x4F) == 0x08 或 0x0F
 *   禁止启动      0x40      未准备好   0x00
 *   准备启动      0x21      已启动     0x23      可操作 0x27
 */

#include <ds402/state_machine.h>

#define SW_FAULT_MASK 0x004Fu     /* bit0..3 + bit6 */
#define SW_OPERATION_MASK 0x006Fu /* bit0..3 + bit5 + bit6 */

enum ds402_state ds402_status_parse(uint16_t statusword)
{
	if ((statusword & SW_FAULT_MASK) == 0x0008u || (statusword & SW_FAULT_MASK) == 0x000Fu) {
		return DS402_FAULT;
	}
	if ((statusword & SW_FAULT_MASK) == 0x0040u) {
		return DS402_SWITCH_ON_DISABLED;
	}
	if ((statusword & SW_FAULT_MASK) == 0x0000u) {
		return DS402_NOT_READY;
	}
	if ((statusword & SW_OPERATION_MASK) == 0x0021u) {
		return DS402_READY;
	}
	if ((statusword & SW_OPERATION_MASK) == 0x0023u) {
		return DS402_SWITCHED_ON;
	}
	if ((statusword & SW_OPERATION_MASK) == 0x0027u) {
		return DS402_OP_ENABLED;
	}
	return DS402_STATE_UNKNOWN;
}

uint16_t ds402_cw_for(enum ds402_cmd cmd)
{
	switch (cmd) {
	case DS402_CMD_SHUTDOWN:
		return DS402_CW_SHUTDOWN;
	case DS402_CMD_SWITCH_ON:
		return DS402_CW_SWITCH_ON;
	case DS402_CMD_ENABLE_OP:
		return DS402_CW_ENABLE_OP;
	case DS402_CMD_DISABLE_VOLTAGE:
		return DS402_CW_DISABLE;
	case DS402_CMD_QUICK_STOP:
		return DS402_CW_QUICK_STOP;
	case DS402_CMD_FAULT_RESET:
		return DS402_CW_FAULT_RESET;
	default:
		return DS402_CW_DISABLE;
	}
}

struct ds402_step ds402_step(enum ds402_state cur, enum ds402_state target)
{
	struct ds402_step s;

	s.cmd = DS402_CMD_NONE;
	s.expect = cur;

	/* 已到目标，或处在"只能等"的状态：没有命令可发 */
	if (cur == target || cur == DS402_STATE_UNKNOWN || cur == DS402_NOT_READY) {
		return s;
	}

	/* 故障：先复位，之后重新读状态再规划 */
	if (cur == DS402_FAULT) {
		s.cmd = DS402_CMD_FAULT_RESET;
		s.expect = DS402_SWITCH_ON_DISABLED;
		return s;
	}

	/* 目标在当前状态"下方"（例如从 OP_ENABLED 退到 SWITCH_ON_DISABLED）：
	 * 断电压一步到位，不逐级下降 */
	if ((int)cur > (int)target) {
		s.cmd = DS402_CMD_DISABLE_VOLTAGE;
		s.expect = DS402_SWITCH_ON_DISABLED;
		return s;
	}

	/* 向上走一级 */
	switch (cur) {
	case DS402_SWITCH_ON_DISABLED:
		s.cmd = DS402_CMD_SHUTDOWN;
		s.expect = DS402_READY;
		break;
	case DS402_READY:
		s.cmd = DS402_CMD_SWITCH_ON;
		s.expect = DS402_SWITCHED_ON;
		break;
	case DS402_SWITCHED_ON:
		s.cmd = DS402_CMD_ENABLE_OP;
		s.expect = DS402_OP_ENABLED;
		break;
	default:
		break;
	}
	return s;
}

const char *ds402_state_name(enum ds402_state s)
{
	switch (s) {
	case DS402_NOT_READY:
		return "NOT_READY";
	case DS402_SWITCH_ON_DISABLED:
		return "SWITCH_ON_DISABLED";
	case DS402_READY:
		return "READY";
	case DS402_SWITCHED_ON:
		return "SWITCHED_ON";
	case DS402_OP_ENABLED:
		return "OP_ENABLED";
	case DS402_FAULT:
		return "FAULT";
	default:
		return "UNKNOWN";
	}
}
