/*
 * axis.c — 换算业务单位并转发单轴与多轴动作请求
 *
 * 一个轴对应一个 CiA 402 设备实例，业务状态由状态机管理。
 * 机械换算参数来自 cats_mech.h，编译期固定。
 *
 * Author: zager
 */

#include <axis/axis.h>

#include <cats_mech.h>

#include <stddef.h>

static int axis_submit(struct axis *a, const struct cia402_request *req);

/* mm 到 pulse：double 计算、远离零取整并饱和到 int32，机械参数见 cats_mech.h */
static int32_t scale_mm(float mm);

/* pulse 到 mm：使用与 scale_mm 相反的 double 换算 */
static float unscale_mm(int32_t pulse);

/**
 * axis_init - 初始化轴与节点的映射
 * @a: 轴对象，其设备实例须已装配
 * @node_id: CANopen 节点号
 */
void axis_init(struct axis *a, uint8_t node_id)
{
	a->node_id = node_id;
}

/**
 * axis_poll - 推进一次设备状态机
 * @a: 轴对象；NULL 安全
 * @now_ms: 当前单调毫秒时刻
 *
 * 不在轴层复制状态；调用方通过设备接口读取事实或消费结论。
 */
void axis_poll(struct axis *a, uint32_t now_ms)
{
	if (a == NULL) {
		return;
	}
	cia402_poll(&a->dev, now_ms, NULL);
}

/**
 * axis_move_to - 提交绝对位置运动
 * @a: 轴对象；NULL 时拒绝
 * @mm: 目标位置
 * @mm_s: 轮廓速度
 * @mm_s2: 轮廓加速度和减速度
 * Return: 0 已受理；1 设备忙；-1 轴为空、参数含 NaN 或设备拒绝。
 */
int axis_move_to(struct axis *a, float mm, float mm_s, float mm_s2)
{
	if (a == NULL) {
		return -1;
	}
	if (mm != mm || mm_s != mm_s || mm_s2 != mm_s2) {
		return -1; /* NaN：进到整数换算就是未定义行为，这里就拦住 */
	}
	{
		struct cia402_request req;

		req.kind = CIA402_REQ_MOVE;
		req.pp.target = scale_mm(mm);
		req.pp.velocity = scale_mm(mm_s);
		req.pp.accel = scale_mm(mm_s2);
		req.pp.decel = scale_mm(mm_s2);
		return axis_submit(a, &req);
	}
}

/**
 * axis_halt - 请求减速停车并保持使能
 * @a: 轴对象；NULL 安全
 *
 * Return: 设备请求结果；@a 为 NULL 时返回 -1。
 */
int axis_halt(struct axis *a)
{
	if (a == NULL) {
		return -1;
	}
	{
		const struct cia402_request req = {.kind = CIA402_REQ_HALT};

		return axis_submit(a, &req);
	}
}

/**
 * axis_abort_code - 读取轴最近一次 SDO abort 码
 * @a: 轴对象；NULL 安全
 *
 * Return: abort 码；无记录或 @a 为 NULL 时返回 0。
 */
uint32_t axis_abort_code(const struct axis *a)
{
	struct cia402_status status;

	if (a == NULL) {
		return 0U;
	}
	cia402_get_status(&a->dev, &status);
	return status.last.abort_code;
}

/**
 * axis_sample - 读取最近一次反馈并换算为业务单位
 * @a: 轴对象；NULL 时输出全 0
 * @out: 输出样本；NULL 时直接返回
 *
 * 不触发总线传输，只读设备层缓存的反馈。
 */
void axis_sample(const struct axis *a, struct axis_sample *out)
{
	struct cia402_feedback feedback;

	if (out == NULL) {
		return;
	}
	out->position_mm = 0.0f;
	out->velocity_mm_s = 0.0f;
	out->statusword = 0U;
	out->error_code = 0U;
	if (a == NULL) {
		return;
	}

	cia402_get_feedback(&a->dev, &feedback);
	out->position_mm = unscale_mm(feedback.position);
	out->velocity_mm_s = unscale_mm(feedback.velocity);
	out->statusword = feedback.statusword;
	out->error_code = feedback.error_code;
}

/**
 * axis_take_motion - 取走轴运动结论并查询在途状态
 * @a: 轴对象；NULL 安全
 *
 * Return: DONE、FAILED、BUSY 或 NONE；@a 为 NULL 时返回 NONE。
 */
enum axis_motion axis_take_motion(struct axis *a)
{
	enum cia402_operation_result result;
	struct cia402_status status;

	if (a == NULL) {
		return AXIS_MOTION_NONE;
	}

	result = cia402_take_result(&a->dev);
	if (result == CIA402_OP_DONE) {
		return AXIS_MOTION_DONE;
	}
	if (result == CIA402_OP_FAILED) {
		return AXIS_MOTION_FAILED;
	}

	cia402_get_status(&a->dev, &status);
	return (status.phase != CIA402_PHASE_IDLE) ? AXIS_MOTION_BUSY : AXIS_MOTION_NONE;
}

/**
 * axis_config_all - 逐轴调用配置请求
 * @axes: 轴数组
 * @count: 轴数量
 *
 * Return: 0 数组有效，不表示每轴均受理；-1 数组为空或数量为 0。
 */
int axis_config_all(struct axis *axes, uint8_t count)
{
	uint8_t i;

	if (axes == NULL || count == 0U) {
		return -1;
	}
	for (i = 0; i < count; i++) {
		const struct cia402_request req = {.kind = CIA402_REQ_CONFIG};

		/* 已配过时会被内部拒绝，不算错 */
		(void)axis_submit(&axes[i], &req);
	}
	return 0;
}

/**
 * axis_enable_all - 逐轴调用使能请求
 * @axes: 轴数组
 * @count: 轴数量
 *
 * Return: 0 数组有效，不表示每轴均受理；-1 数组为空或数量为 0。
 */
int axis_enable_all(struct axis *axes, uint8_t count)
{
	uint8_t i;

	if (axes == NULL || count == 0U) {
		return -1;
	}
	for (i = 0; i < count; i++) {
		const struct cia402_request req = {.kind = CIA402_REQ_ENABLE};

		/* 已使能时会被内部拒绝，不算错 */
		(void)axis_submit(&axes[i], &req);
	}
	return 0;
}

/**
 * axis_quick_stop_all - 请求所有轴快速停止并断使能
 * @axes: 轴数组；NULL 时直接返回
 * @count: 轴数量
 */
void axis_quick_stop_all(struct axis *axes, uint8_t count)
{
	uint8_t i;

	if (axes == NULL) {
		return;
	}
	for (i = 0; i < count; i++) {
		const struct cia402_request req = {.kind = CIA402_REQ_QUICK_STOP};

		(void)axis_submit(&axes[i], &req);
	}
}

/**
 * axis_all_configured - 检查是否所有轴均已配置
 * @axes: 轴数组
 * @count: 轴数量
 *
 * Return: 1 全部配置；0 存在未配置轴或参数无效。
 */
int axis_all_configured(const struct axis *axes, uint8_t count)
{
	struct cia402_status status;
	uint8_t i;

	if (axes == NULL || count == 0U) {
		return 0;
	}
	for (i = 0; i < count; i++) {
		cia402_get_status(&axes[i].dev, &status);
		if (status.configured == 0u) {
			return 0;
		}
	}
	return 1;
}

/**
 * axis_all_enabled - 检查是否所有轴均已使能
 * @axes: 轴数组
 * @count: 轴数量
 *
 * Return: 1 全部使能；0 存在未使能轴或参数无效。
 */
int axis_all_enabled(const struct axis *axes, uint8_t count)
{
	struct cia402_status status;
	uint8_t i;

	if (axes == NULL || count == 0U) {
		return 0;
	}
	for (i = 0; i < count; i++) {
		cia402_get_status(&axes[i].dev, &status);
		if (status.enabled == 0u) {
			return 0;
		}
	}
	return 1;
}

/**
 * axis_any_failed - 检查是否存在失败轴
 * @axes: 轴数组；NULL 安全
 * @count: 轴数量
 *
 * Return: 1 至少一轴失败；0 无失败轴或数组为空。
 */
int axis_any_failed(const struct axis *axes, uint8_t count)
{
	struct cia402_status status;
	uint8_t i;

	if (axes == NULL) {
		return 0;
	}
	for (i = 0; i < count; i++) {
		cia402_get_status(&axes[i].dev, &status);
		if (status.failed != 0u) {
			return 1;
		}
	}
	return 0;
}

static int32_t scale_mm(float mm)
{
	double p = ((double)mm * (double)CATS_AXIS_PULSES_PER_REV) / (double)CATS_AXIS_MM_PER_REV;

	p += (p >= 0.0) ? 0.5 : -0.5;
	if (p >= 2147483647.0) {
		return INT32_MAX;
	}
	if (p <= -2147483647.0) {
		return -INT32_MAX;
	}
	return (int32_t)p;
}

static float unscale_mm(int32_t pulse)
{
	return (float)(((double)pulse * (double)CATS_AXIS_MM_PER_REV) /
		       (double)CATS_AXIS_PULSES_PER_REV);
}

static int axis_submit(struct axis *a, const struct cia402_request *req)
{
	if (cia402_request(&a->dev, req) != CIA402_ACCEPTED) {
		return 1;
	}
	return 0;
}
