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

static int axis_submit(struct cats_axis *a, const struct cia402_request *req);

/* mm 到 pulse：double 计算、远离零取整并饱和到 int32，机械参数见 cats_mech.h */
static int32_t scale_mm(float mm);

/* pulse 到 mm：使用与 scale_mm 相反的 double 换算 */
static float unscale_mm(int32_t pulse);

/**
 * axis_init - 初始化轴与节点的映射
 * @a: 轴对象，其设备实例须已装配
 * @node_id: CANopen 节点号
 */
void axis_init(struct cats_axis *a, uint8_t node_id)
{
	a->node_id = node_id;
}

/**
 * axis_poll - 推进一次设备状态机并刷新本层缓存
 * @a: 轴对象；NULL 安全
 * @now_ms: 当前单调毫秒时刻
 */
void axis_poll(struct cats_axis *a, uint32_t now_ms)
{
	if (a == NULL) {
		return;
	}
	{
		struct cia402_status st;

		cia402_poll(&a->dev, now_ms, &st);
		/* 结论是事件：只在有新结论时覆盖缓存。结论只保持到下次受理，
		 * 每拍都覆盖的话，上层采样一慢就会整条错过。 */
		if (st.last.result != CIA402_OP_NONE) {
			a->status.last.result = st.last.result;
			a->status.last.fail = st.last.fail;
			a->status.last.abort_code = st.last.abort_code;
		}
		a->status.phase = st.phase;
		a->status.configured = st.configured;
		a->status.enabled = st.enabled;
		a->status.failed = st.failed;
		cia402_get_feedback(&a->dev, &a->feedback);
	}
}

/**
 * axis_move_to - 提交绝对位置运动
 * @a: 轴对象；NULL 时拒绝
 * @mm: 目标位置
 * @mm_s: 轮廓速度
 * @mm_s2: 轮廓加速度和减速度
 * Return: 0 已受理；1 设备忙；-1 轴为空、参数含 NaN 或设备拒绝。
 */
int axis_move_to(struct cats_axis *a, float mm, float mm_s, float mm_s2)
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
int axis_halt(struct cats_axis *a)
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
uint32_t axis_abort_code(const struct cats_axis *a)
{
	return (a == NULL) ? 0U : a->status.last.abort_code;
}

/**
 * axis_sample - 读取最近一次反馈并换算为业务单位
 * @a: 轴对象；NULL 时输出全 0
 * @out: 输出样本；NULL 时直接返回
 *
 * 不触发总线传输，只读设备层缓存的反馈。
 */
void axis_sample(const struct cats_axis *a, struct axis_sample *out)
{
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

	out->position_mm = unscale_mm(a->feedback.position);
	out->velocity_mm_s = unscale_mm(a->feedback.velocity);
	out->statusword = a->feedback.statusword;
	out->error_code = a->feedback.error_code;
}

/**
 * axis_take_motion - 取走轴运动结论并查询在途状态
 * @a: 轴对象；NULL 安全
 *
 * Return: DONE、FAILED、BUSY 或 NONE；@a 为 NULL 时返回 NONE。
 */
enum axis_motion axis_take_motion(struct cats_axis *a)
{
	if (a == NULL) {
		return AXIS_MOTION_NONE;
	}
	if (a->status.last.result == CIA402_OP_DONE) {
		a->status.last.result = CIA402_OP_NONE; /* 取走即清：一次结论只报一次 */
		return AXIS_MOTION_DONE;
	}
	if (a->status.last.result == CIA402_OP_FAILED) {
		a->status.last.result = CIA402_OP_NONE;
		return AXIS_MOTION_FAILED;
	}
	return (a->status.phase != CIA402_PHASE_IDLE) ? AXIS_MOTION_BUSY : AXIS_MOTION_NONE;
}

/**
 * axis_config_all - 逐轴调用配置请求
 * @axes: 轴数组
 * @count: 轴数量
 *
 * Return: 0 数组有效，不表示每轴均受理；-1 数组为空或数量为 0。
 */
int axis_config_all(struct cats_axis *axes, uint8_t count)
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
int axis_enable_all(struct cats_axis *axes, uint8_t count)
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
void axis_quick_stop_all(struct cats_axis *axes, uint8_t count)
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
int axis_all_configured(const struct cats_axis *axes, uint8_t count)
{
	uint8_t i;

	if (axes == NULL || count == 0U) {
		return 0;
	}
	for (i = 0; i < count; i++) {
		if (axes[i].status.configured == 0u) {
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
int axis_all_enabled(const struct cats_axis *axes, uint8_t count)
{
	uint8_t i;

	if (axes == NULL || count == 0U) {
		return 0;
	}
	for (i = 0; i < count; i++) {
		if (axes[i].status.enabled == 0u) {
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
int axis_any_failed(const struct cats_axis *axes, uint8_t count)
{
	uint8_t i;

	if (axes == NULL) {
		return 0;
	}
	for (i = 0; i < count; i++) {
		if (axes[i].status.failed != 0u) {
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

static int axis_submit(struct cats_axis *a, const struct cia402_request *req)
{
	if (cia402_request(&a->dev, req) != CIA402_ACCEPTED) {
		return 1;
	}
	/* 新动作开始：上一条的结论作废，别让 MOVING 第一拍读到旧 DONE */
	a->status.last.result = CIA402_OP_NONE;
	a->status.last.fail = CIA402_OP_FAIL_NONE;
	return 0;
}
