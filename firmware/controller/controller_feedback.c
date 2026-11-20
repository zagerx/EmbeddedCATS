/*
 * controller_feedback.c — 调度各轴反馈读取、单位换算并保存样本
 *
 * 反馈是控制器的观察行为，不属于 402 操作流程。每个轴复用自己的
 * SDO 通道：忙就跳过，不排队、不抢占；成功才更新样本，失败保留旧值。
 * 样本以 mm / mm·s⁻¹ 直接保存在 controller_info，写入时持 info_lock。
 * 每轴四项依次轮转，多个轴在同一轮中各尝试一笔空闲通道读取。
 *
 * Author: zager
 */

#include "controller_internal.h"

#include <cia402/od.h>
#include <cats_mech.h>

#define CONTROLLER_FEEDBACK_TIMEOUT_MS 30u

/*
 * 临时开关（2026-11-20）：真机抓总线期间关闭反馈轮询，让总线上只剩控制流程的帧。
 * 关闭时 controller_info.feedback 保持全 0（由 controller_init 清零后无人写入）。
 * 恢复：把这个值改成 1 即可，轮询逻辑原样保留。
 */
#define CONTROLLER_FEEDBACK_ENABLED 0u

/* pulse / pulse·s⁻¹ 转为 mm / mm·s⁻¹ */
static float feedback_mm(int32_t pulse);

/* 保存一项读取结果；旧值只在成功时替换 */
static void feedback_store(struct controller_feedback *feedback, uint8_t item,
			   const struct sdo_result *result);

static uint8_t next_item[CONTROLLER_AXIS_COUNT];

/**
 * controller_feedback_reset - 复位各轴反馈轮询游标
 *
 * 控制器重新初始化时调用，避免沿用上一次运行的轮询位置。
 */
void controller_feedback_reset(void)
{
	uint8_t i;

	for (i = 0u; i < CONTROLLER_AXIS_COUNT; i++) {
		next_item[i] = 0u;
	}
}

/**
 * controller_feedback_poll - 每轴尝试推进一项反馈读取
 * @ctl: 控制上下文；NULL 安全
 *
 * 402 流程优先使用通道；反馈读取由下一轮 sdo_client_step() 推进。
 */
void controller_feedback_poll(struct controller *ctl)
{
	static const uint16_t index[4] = {CIA402_OD_STATUSWORD, CIA402_OD_POSITION_ACTUAL,
					  CIA402_OD_VELOCITY_ACTUAL, CIA402_OD_ERROR_CODE};
	struct sdo_result result;
	uint8_t i;

	if (CONTROLLER_FEEDBACK_ENABLED == 0u) {
		(void)ctl; /* 反馈轮询已临时关闭：不发帧、不改上报值 */
		return;
	}
	if (ctl == NULL) {
		return;
	}
	for (i = 0u; i < ctl->axis_count; i++) {
		struct axis *axis = &ctl->axes[i];
		struct controller_feedback *feedback = &ctl->info.feedback[i];
		const struct sdo_access *access = sdo_arbiter_feedback(&axis->arb);
		uint8_t item;

		if (access == NULL) {
			continue;
		}
		item = next_item[i];
		if (access->poll(access->ctx, &result) == SDO_POLL_DONE) {
			if (result.status == SDO_STATUS_OK) {
				pthread_mutex_lock(&ctl->info_lock);
				feedback_store(feedback, item, &result);
				pthread_mutex_unlock(&ctl->info_lock);
			}
			/* 先推进项号再发起，下一次读的就是下一项 */
			item = (uint8_t)((item + 1u) % 4u);
			next_item[i] = item;
		}
		(void)access->start(access->ctx, SDO_DIR_READ, index[item], 0u, 0u, 0u,
				    CONTROLLER_FEEDBACK_TIMEOUT_MS);
	}
}

static void feedback_store(struct controller_feedback *feedback, uint8_t item,
			   const struct sdo_result *result)
{
	switch (item) {
	case 0u:
		feedback->statusword = (uint16_t)result->value;
		break;
	case 1u:
		feedback->position = feedback_mm((int32_t)result->value);
		break;
	case 2u:
		feedback->velocity = feedback_mm((int32_t)result->value);
		break;
	default:
		feedback->error_code = (uint16_t)result->value;
		break;
	}
}

static float feedback_mm(int32_t pulse)
{
	return (float)(((double)pulse * (double)CATS_AXIS_MM_PER_REV) /
		       (double)CATS_AXIS_PULSES_PER_REV);
}
