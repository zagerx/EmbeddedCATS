/*
 * axis.c — 轴层：单轴动作 + 批量转发
 *
 * 一个轴 = 一个节点，每个节点有一条自己的 SDO 通道（OD 的 1280h/1281h 各绑
 * 一个站号），所以轴与轴之间不需要排队：同时发起、各走各的，谁先出结论谁
 * 先上报，一个轴失败不影响另一个轴。
 *
 * 本文件因此没有"轮到谁"的状态。批量动作只是 for 循环——一组轴就是
 * "数组 + 条数"，不另立容器（调用方已经有两个事实，再包一层只是制造第二份真相）。
 */

#include <axis/axis.h>

#include <stddef.h>

void axis_init(struct cats_axis *a, struct co_master *m, uint8_t node_id)
{
	a->node_id = node_id;
	a->pulses_per_rev = 0;
	a->mm_per_rev = 0.0f;
	cia402_init(&a->drv, m, node_id);
}

void axis_set_scale(struct cats_axis *a, int32_t pulses_per_rev, float mm_per_rev)
{
	if (a == NULL) {
		return;
	}
	a->pulses_per_rev = pulses_per_rev;
	a->mm_per_rev = mm_per_rev;
}

/*
 * mm → pulse。全链路只有这一处取整：
 *   1. 先升到 double 算（float 乘法会丢位，且 double 在 x86-64/aarch64 上
 *      都是同一套 IEEE 双精度运算，结果可复现）；
 *   2. 四舍五入、远离零（加 0.5 再截断，不依赖 libm）；
 *   3. 饱和到 int32，防越界（1 m × 10000 pulse/rev 用 int32 也装不下）。
 */
static int32_t scale_mm(const struct cats_axis *a, float mm)
{
	double p = ((double)mm * (double)a->pulses_per_rev) / (double)a->mm_per_rev;

	p += (p >= 0.0) ? 0.5 : -0.5;
	if (p >= 2147483647.0) {
		return INT32_MAX;
	}
	if (p <= -2147483647.0) {
		return -INT32_MAX;
	}
	return (int32_t)p;
}

int axis_move_to(struct cats_axis *a, float mm, float mm_s, float mm_s2)
{
	if (a == NULL || a->pulses_per_rev <= 0 || a->mm_per_rev <= 0.0f) {
		return -1; /* 没配机械参数就不接指令：宁可不动，也不算错位置 */
	}
	if (mm != mm || mm_s != mm_s || mm_s2 != mm_s2) {
		return -1; /* NaN：进到整数换算就是未定义行为，这里就拦住 */
	}
	return cia402_pp_start(&a->drv, scale_mm(a, mm), scale_mm(a, mm_s), scale_mm(a, mm_s2),
			       scale_mm(a, mm_s2));
}

int axis_halt(struct cats_axis *a)
{
	return (a == NULL) ? -1 : cia402_halt(&a->drv);
}

uint32_t axis_abort_code(const struct cats_axis *a)
{
	return (a == NULL) ? 0U : cia402_abort_code(&a->drv);
}

enum axis_motion axis_take_motion(struct cats_axis *a)
{
	enum cia402_result r;

	if (a == NULL) {
		return AXIS_MOTION_NONE;
	}
	r = cia402_take_result(&a->drv);
	if (r == CIA402_RESULT_DONE) {
		return AXIS_MOTION_DONE;
	}
	if (r == CIA402_RESULT_FAILED) {
		return AXIS_MOTION_FAILED;
	}
	return cia402_busy(&a->drv) ? AXIS_MOTION_BUSY : AXIS_MOTION_NONE;
}

/* ---- 批量动作：一组轴 = 数组 + 条数 ---- */

int axis_config_all(struct cats_axis *axes, uint8_t count)
{
	uint8_t i;

	if (axes == NULL || count == 0U) {
		return -1;
	}
	for (i = 0; i < count; i++) {
		/* 1 = 已经配过（不重复发起），不算错 */
		(void)cia402_request_config(&axes[i].drv);
	}
	return 0;
}

int axis_enable_all(struct cats_axis *axes, uint8_t count)
{
	uint8_t i;

	if (axes == NULL || count == 0U) {
		return -1;
	}
	for (i = 0; i < count; i++) {
		/* 1 = 已经要求过（不重复发起），不算错 */
		(void)cia402_request_enable(&axes[i].drv);
	}
	return 0;
}

void axis_poll_all(struct cats_axis *axes, uint8_t count, uint32_t now_ms)
{
	uint8_t i;

	if (axes == NULL) {
		return;
	}
	for (i = 0; i < count; i++) {
		cia402_poll(&axes[i].drv, now_ms);
	}
}

void axis_quick_stop_all(struct cats_axis *axes, uint8_t count)
{
	uint8_t i;

	if (axes == NULL) {
		return;
	}
	for (i = 0; i < count; i++) {
		(void)cia402_quick_stop(&axes[i].drv);
	}
}

int axis_all_configured(const struct cats_axis *axes, uint8_t count)
{
	uint8_t i;

	if (axes == NULL || count == 0U) {
		return 0;
	}
	for (i = 0; i < count; i++) {
		if (!cia402_is_configured(&axes[i].drv)) {
			return 0;
		}
	}
	return 1;
}

int axis_all_enabled(const struct cats_axis *axes, uint8_t count)
{
	uint8_t i;

	if (axes == NULL || count == 0U) {
		return 0;
	}
	for (i = 0; i < count; i++) {
		if (!cia402_is_enabled(&axes[i].drv)) {
			return 0;
		}
	}
	return 1;
}

int axis_any_failed(const struct cats_axis *axes, uint8_t count)
{
	uint8_t i;

	if (axes == NULL) {
		return 0;
	}
	for (i = 0; i < count; i++) {
		if (cia402_failed(&axes[i].drv)) {
			return 1;
		}
	}
	return 0;
}
