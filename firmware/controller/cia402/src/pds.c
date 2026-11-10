/*
 * pds.c — 解析 CiA 402 PDS 状态并提供控制字序列
 *
 * 本文件只包含标准知识，不访问总线、时钟或线程。
 * 使能与断使能序列按输出能力单调排列，供调用方做起点快进。
 *
 * Author: zager
 */

#include <cia402/pds.h>

#include <stddef.h>

#define SW_FAULT_MASK 0x004Fu     /* bit0..3 + bit6 */
#define SW_OPERATION_MASK 0x006Fu /* bit0..3 + bit5 + bit6 */
#define SW_SWITCH_ON_DISABLED_MASK 0x004Fu

static const struct cia402_seq_step k_seq_enable[3];
static const struct cia402_seq_step k_seq_disable[1];

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

/**
 * cia402_state_name - 获取 PDS 状态名称
 * @s: PDS 状态枚举
 *
 * Return: 静态名称字符串；未知状态返回 "UNKNOWN"。
 */
const char *cia402_state_name(enum cia402_pds_state s)
{
	switch (s) {
	case CIA402_NOT_READY:
		return "NOT_READY";
	case CIA402_SWITCH_ON_DISABLED:
		return "SWITCH_ON_DISABLED";
	case CIA402_READY:
		return "READY";
	case CIA402_SWITCHED_ON:
		return "SWITCHED_ON";
	case CIA402_OP_ENABLED:
		return "OP_ENABLED";
	case CIA402_FAULT:
		return "FAULT";
	default:
		return "UNKNOWN";
	}
}

/**
 * cia402_seq_enable - 获取使能控制字序列
 * @count: 输出序列长度；NULL 安全
 *
 * Return: 静态序列表指针。
 */
const struct cia402_seq_step *cia402_seq_enable(uint8_t *count)
{
	if (count != NULL) {
		*count = (uint8_t)(sizeof(k_seq_enable) / sizeof(k_seq_enable[0]));
	}
	return k_seq_enable;
}

/**
 * cia402_seq_disable - 获取断使能控制字序列
 * @count: 输出序列长度；NULL 安全
 *
 * Return: 静态序列表指针。
 */
const struct cia402_seq_step *cia402_seq_disable(uint8_t *count)
{
	if (count != NULL) {
		*count = (uint8_t)(sizeof(k_seq_disable) / sizeof(k_seq_disable[0]));
	}
	return k_seq_disable;
}

/**
 * cia402_seq_skip - 找出控制字序列中首个尚未完成的步骤
 * @statusword: 当前 6041 状态字
 * @seq: 控制字序列表
 * @count: 序列长度
 *
 * Return: 下一步下标；全部完成返回 @count；空序列或 NULL 返回 0。
 */
uint8_t cia402_seq_skip(uint16_t statusword, const struct cia402_seq_step *seq, uint8_t count)
{
	int j;

	if (seq == NULL || count == 0u) {
		return 0u;
	}
	/* 从后往前找"已经做到的最后一步"：它之后的那些步才是要发的 */
	for (j = (int)count - 1; j >= 0; j--) {
		if ((statusword & seq[j].exp_mask) == seq[j].exp_value) {
			return (uint8_t)(j + 1);
		}
	}
	return 0u;
}

static const struct cia402_seq_step k_seq_enable[] = {
	{CIA402_CW_SHUTDOWN, SW_OPERATION_MASK, 0x0021u},  /* 06 → Ready to switch on */
	{CIA402_CW_SWITCH_ON, SW_OPERATION_MASK, 0x0023u}, /* 07 → Switched on */
	{CIA402_CW_ENABLE_OP, SW_OPERATION_MASK, 0x0027u}, /* 0F → Operation enabled */
};

static const struct cia402_seq_step k_seq_disable[] = {
	{CIA402_CW_DISABLE, SW_SWITCH_ON_DISABLED_MASK, 0x0040u}, /* 00 → Switch on disabled */
};
