/*
 * main.c — 验证控制层未执行与执行失败的边界
 *
 * 只测内存中的状态机与回执，不打开 CAN、不驱动电机。
 *
 * Author: zager
 */

#include "controller_log.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void test_not_executed(void);
static void test_cancelled(void);
static void test_multiple_failures(void);
static void test_operation_failure_only(void);
static void test_queue_overwrite(void);

#define CHECK(cond)                                                                                \
	do {                                                                                       \
		if (!(cond)) {                                                                     \
			fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);             \
			failures++;                                                                \
		}                                                                                  \
	} while (0)

int main(void)
{
	test_not_executed();
	test_cancelled();
	test_multiple_failures();
	test_operation_failure_only();
	test_queue_overwrite();
	if (failures == 0) {
		puts("controller_fault_selftest: 全部通过");
		return 0;
	}
	fprintf(stderr, "controller_fault_selftest: %d 处失败\n", failures);
	return 1;
}

static void test_not_executed(void)
{
	struct controller ctl = {.info_lock = PTHREAD_MUTEX_INITIALIZER};

	controller_log_not_executed(&ctl, CONTROLLER_SIG_MOVE, CONTROLLER_FAULT_NO_AXIS,
				    CONTROLLER_NOT_EXECUTED_INVALID_STATE);
	CHECK(ctl.info.fault.reason == CONTROLLER_FAULT_REASON_NONE);
	controller_set_axes_fault(&ctl, CONTROLLER_STATE_CONFIGURING, NULL);
	CHECK(ctl.info.fault.reason == CONTROLLER_FAULT_REASON_NONE);
}

static void test_cancelled(void)
{
	struct controller ctl = {0};
	struct controller_axes_failure failure;
	uint8_t done = 1u;

	cia402_init(&ctl.axes[0].dev, NULL);
	/* 已受理但尚未出邮箱不能视为取消完成 */
	ctl.axes[0].dev.mailbox.pending = 1u;
	CHECK(controller_axes_collect(ctl.axes, 1u, &done, &failure) == CONTROLLER_AXES_WAITING);
	CHECK(done == 1u);

	/* 已取消流程没有操作结局，结束等待但不产生失败 */
	ctl.axes[0].dev.mailbox.pending = 0u;
	CHECK(controller_axes_collect(ctl.axes, 1u, &done, &failure) == CONTROLLER_AXES_DONE);
	CHECK(done == 0u && failure.mask == 0u);
	controller_set_axes_fault(&ctl, CONTROLLER_STATE_CONFIGURING, &failure);
	CHECK(ctl.info.fault.reason == CONTROLLER_FAULT_REASON_NONE);
}

static void test_multiple_failures(void)
{
	struct controller ctl = {.info_lock = PTHREAD_MUTEX_INITIALIZER};
	struct controller_axes_failure failure;
	uint8_t done = 3u;

	cia402_init(&ctl.axes[0].dev, NULL);
	cia402_init(&ctl.axes[1].dev, NULL);
	ctl.axes[0].dev.outbox = CIA402_OP_RESULT_FAILED;
	ctl.axes[0].dev.failure = CIA402_FAIL_DRIVER;
	ctl.axes[1].dev.outbox = CIA402_OP_RESULT_FAILED;
	ctl.axes[1].dev.failure = CIA402_FAIL_TIMEOUT;
	CHECK(controller_axes_collect(ctl.axes, 2u, &done, &failure) == CONTROLLER_AXES_FAILED);
	CHECK(failure.mask == 3u);
	controller_set_axes_fault(&ctl, CONTROLLER_STATE_CONFIGURING, &failure);
	CHECK(ctl.info.fault.axis_mask == 3u);
	CHECK(ctl.info.fault.first_axis == 0u);
	CHECK(ctl.info.fault.reason == CONTROLLER_FAULT_REASON_DRIVER);

	/* 失败优先于同拍另一轴的取消结束，不会在下一拍吞掉失败 */
	done = 3u;
	CHECK(controller_axes_collect(ctl.axes, 2u, &done, &failure) == CONTROLLER_AXES_DONE);

	/* 未参与本次集合的旧回执不能被误取 */
	ctl.axes[0].dev.outbox = CIA402_OP_RESULT_DONE;
	done = 2u;
	CHECK(controller_axes_collect(ctl.axes, 2u, &done, &failure) == CONTROLLER_AXES_DONE);
	CHECK(cia402_take_result(&ctl.axes[0].dev) == CIA402_OP_RESULT_DONE);
}

/* 操作没成功不等于设备故障：只记日志，不锁存 FAULT */
static void test_operation_failure_only(void)
{
	struct controller ctl = {.info_lock = PTHREAD_MUTEX_INITIALIZER};
	struct controller_axes_failure failure;
	uint8_t done = 3u;

	cia402_init(&ctl.axes[0].dev, NULL);
	cia402_init(&ctl.axes[1].dev, NULL);
	ctl.axes[0].dev.outbox = CIA402_OP_RESULT_FAILED;
	ctl.axes[0].dev.failure = CIA402_FAIL_TIMEOUT;
	ctl.axes[1].dev.outbox = CIA402_OP_RESULT_FAILED;
	ctl.axes[1].dev.failure = CIA402_FAIL_HOMING;
	CHECK(controller_axes_collect(ctl.axes, 2u, &done, &failure) == CONTROLLER_AXES_FAILED);
	CHECK(failure.mask == 3u);
	CHECK(failure.driver_mask == 0u);
	CHECK(controller_set_axes_fault(&ctl, CONTROLLER_STATE_MOVING, &failure) == 0);
	CHECK(ctl.info.fault.reason == CONTROLLER_FAULT_REASON_NONE);
	CHECK(ctl.info.fault.axis_mask == 0u);

	/* 归类映射：未执行不等于超时或传输失败；只有驱动器故障是致命的 */
	CHECK(controller_fault_reason_of(CIA402_FAIL_NOT_STARTED) ==
	      CONTROLLER_FAULT_REASON_NOT_EXECUTED);
	CHECK(controller_fault_is_fatal(CIA402_FAIL_DRIVER) != 0);
	CHECK(controller_fault_is_fatal(CIA402_FAIL_TIMEOUT) == 0);
	CHECK(controller_fault_is_fatal(CIA402_FAIL_NOT_STARTED) == 0);
}

static void test_queue_overwrite(void)
{
	struct controller ctl = {.evtq.lock = PTHREAD_MUTEX_INITIALIZER,
				 .info_lock = PTHREAD_MUTEX_INITIALIZER};
	struct controller_evt evt = {.super = {.sig = CONTROLLER_SIG_ENABLE}};
	struct controller_evt urgent = {.super = {.sig = CONTROLLER_SIG_HALT}};
	struct controller_evt replaced = {.super = {.sig = CONTROLLER_SIG_TICK}};
	uint8_t i;

	controller_evtq_init(&ctl.evtq);
	for (i = 0u; i < CONTROLLER_EVTQ_LEN; i++) {
		CHECK(controller_evtq_push(&ctl.evtq, &evt));
	}
	controller_evtq_push_front(&ctl.evtq, &urgent, &replaced);
	CHECK(replaced.super.sig == CONTROLLER_SIG_ENABLE);
	controller_log_not_executed(&ctl, (enum controller_sig)replaced.super.sig,
				    CONTROLLER_FAULT_NO_AXIS, CONTROLLER_NOT_EXECUTED_QUEUE_FULL);
	CHECK(ctl.info.fault.reason == CONTROLLER_FAULT_REASON_NONE);
}
