/*
 * controller_feedback.c — 调度各轴反馈读取并保存样本
 *
 * 反馈是控制器的观察行为，不属于 402 操作流程。每个轴复用自己的
 * SDO 通道：忙就跳过，不排队、不抢占；成功才更新样本，失败保留旧值。
 * 每轴四项依次轮转，多个轴在同一轮中各尝试一笔空闲通道读取。
 *
 * Author: zager
 */

#include "controller_internal.h"

#include <cia402/od.h>

#define CONTROLLER_FEEDBACK_TIMEOUT_MS 100u

/* 保存一项读取结果；旧值只在成功时替换 */
static void feedback_store(struct controller_axis_feedback *feedback, uint8_t item,
			   const struct sdo_result *result);

/**
 * controller_feedback_poll - 每轴尝试推进一项反馈读取
 * @ctl: 控制上下文；NULL 安全
 * @now_ms: 当前单调毫秒时刻
 *
 * 402 流程优先使用通道；反馈读取由下一轮 sdo_client_step() 推进。
 */
void controller_feedback_poll(struct controller *ctl, uint32_t now_ms)
{
	static const uint16_t index[4] = {CIA402_OD_STATUSWORD, CIA402_OD_POSITION_ACTUAL,
					  CIA402_OD_VELOCITY_ACTUAL, CIA402_OD_ERROR_CODE};
	struct sdo_result result;
	uint8_t i;

	if (ctl == NULL) {
		return;
	}
	for (i = 0u; i < ctl->axis_count; i++) {
		struct axis *axis = &ctl->axes[i];
		struct controller_axis_feedback *feedback = &ctl->feedback[i];
		const struct sdo_access *access = sdo_arbiter_feedback(&axis->arb);
		uint8_t item = feedback->next;

		if (access == NULL) {
			continue;
		}
		if (access->take_result(access->ctx, &result) == 0) {
			if (result.status == 0) {
				feedback_store(feedback, item, &result);
				feedback->updated_ms[item] = now_ms;
			}
			feedback->next = (uint8_t)((item + 1u) % 4u);
		}
		if (access->start(access->ctx, SDO_DIR_READ, index[item], 0u, 0u, 0u,
				  CONTROLLER_FEEDBACK_TIMEOUT_MS) != 0) {
			continue;
		}
	}
}

static void feedback_store(struct controller_axis_feedback *feedback, uint8_t item,
			   const struct sdo_result *result)
{
	switch (item) {
	case 0u:
		feedback->statusword = (uint16_t)result->value;
		break;
	case 1u:
		feedback->position = (int32_t)result->value;
		break;
	case 2u:
		feedback->velocity = (int32_t)result->value;
		break;
	default:
		feedback->error_code = (uint16_t)result->value;
		break;
	}
	feedback->valid_mask |= (uint8_t)(1u << item);
}
