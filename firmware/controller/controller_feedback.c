/*
 * controller_feedback.c — 把 TPDO 反馈搬进对外信息结构
 *
 * 反馈数据由 canopen/pdo 在接收回调里解好；本文件只做两件事：把某个轴的
 * 最新值换算成 mm，并在有帧到来时复制进 controller_info。轮询本身不再发
 * 任何总线帧，因此也不会和 402 控制流程抢 SDO 通道。
 *
 * 每轴两路 TPDO（状态字一路、位置速度一路），收到新帧才更新一次；没有新帧
 * 就保留旧值，与之前 SDO 读失败时保留旧值的口径一致。
 *
 * Author: zager
 */

#include "controller_internal.h"

#include <cats_mech.h>
#include <pdo/tpdo.h>

/*
 * 抓总线时的静音开关：置 0 可让总线上只剩控制流程的帧，此时
 * controller_info.feedback 保持全 0（由 controller_init 清零后无人写入）。
 * 正常运行必须为 1。
 */
#define CONTROLLER_FEEDBACK_ENABLED 1u

/* pulse / pulse·s⁻¹ 转为 mm / mm·s⁻¹ */
static float feedback_mm(int32_t pulse);

/* 每轴上一次见到的累计帧数；变了才说明有新数据 */
static uint32_t last_frames[CONTROLLER_AXIS_COUNT];

/**
 * controller_feedback_reset - 复位各轴的新帧游标
 *
 * 控制器重新初始化时调用，避免沿用上一次运行的计数。
 */
void controller_feedback_reset(void)
{
	uint8_t i;

	for (i = 0u; i < CONTROLLER_AXIS_COUNT; i++) {
		last_frames[i] = 0u;
	}
}

/**
 * controller_feedback_poll - 把各轴最新 TPDO 快照搬进上报结构
 * @ctl: 控制上下文；NULL 安全
 * @now_ms: 本轮统一时钟；本函数不用它，保持与其它 poll 同签名
 *
 * 只在累计帧数变化时加锁复制一次，避免每拍都动 info_lock。
 */
void controller_feedback_poll(struct controller *ctl, uint32_t now_ms)
{
	uint8_t i;

	(void)now_ms; /* 数据由接收回调推进，这里只取快照 */
	if (CONTROLLER_FEEDBACK_ENABLED == 0u) {
		(void)ctl; /* 反馈已临时关闭：不发帧、不改上报值 */
		return;
	}
	if (ctl == NULL || ctl->node_ready == 0u) {
		return; /* 驱动器还在重启：此时收到的帧不作数 */
	}
	for (i = 0u; i < ctl->axis_count; i++) {
		struct axis *axis = &ctl->axes[i];
		const struct tpdo_frame *values = tpdo_values(&axis->tpdo);
		const uint32_t frames = tpdo_frames(&axis->tpdo, 0u) + tpdo_frames(&axis->tpdo, 1u);

		if (values == NULL || frames == last_frames[i]) {
			continue; /* 还没有反馈，或这一拍没有新帧 */
		}
		last_frames[i] = frames;
		pthread_mutex_lock(&ctl->info_lock);
		ctl->info.feedback[i].statusword = values->statusword;
		ctl->info.feedback[i].error_code = values->error_code;
		ctl->info.feedback[i].position = feedback_mm(values->position);
		ctl->info.feedback[i].velocity = feedback_mm(values->velocity);
		pthread_mutex_unlock(&ctl->info_lock);
	}
}

static float feedback_mm(int32_t pulse)
{
	return (float)(((double)pulse * (double)CATS_AXIS_MM_PER_REV) /
		       (double)CATS_AXIS_PULSES_PER_REV);
}
