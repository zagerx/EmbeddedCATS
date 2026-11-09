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

#include <cats_mech.h>

#include <stddef.h>

void axis_init(struct cats_axis *a, uint8_t node_id)
{
	a->node_id = node_id;
}

/*
 * mm → pulse。全链路只有这一处取整：
 *   1. 先升到 double 算（float 乘法会丢位，且 double 在 x86-64/aarch64 上
 *      都是同一套 IEEE 双精度运算，结果可复现）；
 *   2. 四舍五入、远离零（加 0.5 再截断，不依赖 libm）；
 *   3. 饱和到 int32，防越界。
 *
 * 机械参数是编译期常量，见 controller/config/cats_mech.h
 */
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

int axis_move_to(struct cats_axis *a, float mm, float mm_s, float mm_s2)
{
	if (a == NULL) {
		return -1;
	}
	if (mm != mm || mm_s != mm_s || mm_s2 != mm_s2) {
		return -1; /* NaN：进到整数换算就是未定义行为，这里就拦住 */
	}
	return drive_pp_start(&a->drv, scale_mm(mm), scale_mm(mm_s), scale_mm(mm_s2),
			       scale_mm(mm_s2));
}

int axis_halt(struct cats_axis *a)
{
	return (a == NULL) ? -1 : drive_halt(&a->drv);
}

uint32_t axis_abort_code(const struct cats_axis *a)
{
	return (a == NULL) ? 0U : drive_abort_code(&a->drv);
}

/* pulse → mm，与 scale_mm() 方向相反；同样用 double 算，结果可复现 */
static float unscale_mm(int32_t pulse)
{
	return (float)(((double)pulse * (double)CATS_AXIS_MM_PER_REV) /
		       (double)CATS_AXIS_PULSES_PER_REV);
}

void axis_sample(const struct cats_axis *a, struct axis_sample *out)
{
	uint8_t valid;

	if (out == NULL) {
		return;
	}
	out->position_mm = 0.0f;
	out->velocity_mm_s = 0.0f;
	out->statusword = 0U;
	out->error_code = 0U;
	out->valid = 0U;
	if (a == NULL) {
		return;
	}

	valid = drive_feedback_valid(&a->drv);
	out->statusword = drive_feedback_statusword(&a->drv);
	if ((valid & DRIVE_FB_STATUSWORD) != 0U) {
		out->valid |= DRIVE_FB_STATUSWORD;
	}
	if ((valid & DRIVE_FB_POSITION) != 0U) {
		out->position_mm = unscale_mm(drive_feedback_position(&a->drv));
		out->valid |= DRIVE_FB_POSITION;
	}
	if ((valid & DRIVE_FB_VELOCITY) != 0U) {
		out->velocity_mm_s = unscale_mm(drive_feedback_velocity(&a->drv));
		out->valid |= DRIVE_FB_VELOCITY;
	}
	out->error_code = drive_feedback_error_code(&a->drv);
	if ((valid & DRIVE_FB_ERROR_CODE) != 0U) {
		out->valid |= DRIVE_FB_ERROR_CODE;
	}
}

enum axis_motion axis_take_motion(struct cats_axis *a)
{
	enum drive_result r;

	if (a == NULL) {
		return AXIS_MOTION_NONE;
	}
	r = drive_take_result(&a->drv);
	if (r == DRIVE_RESULT_DONE) {
		return AXIS_MOTION_DONE;
	}
	if (r == DRIVE_RESULT_FAILED) {
		return AXIS_MOTION_FAILED;
	}
	return drive_busy(&a->drv) ? AXIS_MOTION_BUSY : AXIS_MOTION_NONE;
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
		(void)drive_request_config(&axes[i].drv);
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
		(void)drive_request_enable(&axes[i].drv);
	}
	return 0;
}

void axis_quick_stop_all(struct cats_axis *axes, uint8_t count)
{
	uint8_t i;

	if (axes == NULL) {
		return;
	}
	for (i = 0; i < count; i++) {
		(void)drive_quick_stop(&axes[i].drv);
	}
}

int axis_all_configured(const struct cats_axis *axes, uint8_t count)
{
	uint8_t i;

	if (axes == NULL || count == 0U) {
		return 0;
	}
	for (i = 0; i < count; i++) {
		if (!drive_is_configured(&axes[i].drv)) {
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
		if (!drive_is_enabled(&axes[i].drv)) {
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
		if (drive_failed(&axes[i].drv)) {
			return 1;
		}
	}
	return 0;
}
