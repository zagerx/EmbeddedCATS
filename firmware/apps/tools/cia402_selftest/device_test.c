/*
 * device_test.c — 用假异步传输验证设备状态机
 *
 * 覆盖使能、PP 到位、急停抢占和事务超时，不依赖 CAN 硬件。
 *
 * Author: zager
 */

#include <cia402/device.h>
#include <cia402/od.h>
#include <cia402/types.h>

#include <stdio.h>
#include <string.h>

struct fake_access {
	struct sdo_access access;
	uint8_t started;
	uint8_t dir;
	uint8_t size;
	uint16_t index;
	uint32_t value;
	uint8_t result_ready;
	struct sdo_result result;
	unsigned aborts;
};

static int fake_start(void *ctx, uint8_t dir, uint16_t index, uint8_t sub, uint32_t value,
		      uint8_t size, uint32_t timeout_ms);
static int fake_take_result(void *ctx, struct sdo_result *result);
static void fake_abort(void *ctx);
static void fake_complete(struct fake_access *fake, int status, uint32_t value);
static void expect_started(struct fake_access *fake, uint8_t dir, uint16_t index,
			   uint32_t value);
static void test_enable(void);
static void test_pp(void);
static void test_quick_stop_preempts(void);
static void test_timeout(void);

static int failures;

#define CHECK(cond)                                                                                \
	do {                                                                                       \
		if (!(cond)) {                                                                     \
			printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                   \
			failures++;                                                                  \
		}                                                                                  \
	} while (0)

/**
 * cia402_device_tests - 运行设备状态机测试
 *
 * Return: 失败断言数量。
 */
int cia402_device_tests(void)
{
	failures = 0;
	test_enable();
	test_pp();
	test_quick_stop_preempts();
	test_timeout();
	return failures;
}

static int fake_start(void *ctx, uint8_t dir, uint16_t index, uint8_t sub, uint32_t value,
		      uint8_t size, uint32_t timeout_ms)
{
	struct fake_access *fake = ctx;

	(void)sub;
	(void)timeout_ms;
	if (fake->started != 0u) {
		return -1;
	}
	fake->started = 1u;
	fake->dir = dir;
	fake->index = index;
	fake->value = value;
	fake->size = size;
	fake->result_ready = 0u;
	return 0;
}

static int fake_take_result(void *ctx, struct sdo_result *result)
{
	struct fake_access *fake = ctx;

	if (fake->result_ready == 0u) {
		return 1;
	}
	*result = fake->result;
	fake->result_ready = 0u;
	fake->started = 0u;
	return 0;
}

static void fake_abort(void *ctx)
{
	struct fake_access *fake = ctx;

	fake->aborts++;
}

static void fake_complete(struct fake_access *fake, int status, uint32_t value)
{
	memset(&fake->result, 0, sizeof(fake->result));
	fake->result.status = (int8_t)status;
	fake->result.value = value;
	fake->result.len = (fake->dir == SDO_DIR_READ) ? fake->size : 0u;
	fake->result_ready = 1u;
}

static void expect_started(struct fake_access *fake, uint8_t dir, uint16_t index, uint32_t value)
{
	CHECK(fake->started != 0u);
	CHECK(fake->dir == dir);
	CHECK(fake->index == index);
	if (dir == SDO_DIR_WRITE) {
		CHECK(fake->value == value);
	}
}

static void test_enable(void)
{
	struct fake_access fake = {0};
	struct cia402_device dev;

	fake.access.start = fake_start;
	fake.access.take_result = fake_take_result;
	fake.access.abort = fake_abort;
	fake.access.ctx = &fake;
	cia402_device_init(&dev, &fake.access, 1u);

	CHECK(cia402_device_request_enable(&dev) == 0);
	cia402_device_service(&dev, 0u);
	expect_started(&fake, SDO_DIR_READ, CIA402_OD_STATUSWORD, 0u);
	fake_complete(&fake, 0, 0x0040u);
	cia402_device_service(&dev, 1u);
	expect_started(&fake, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD, CIA402_CW_SHUTDOWN);
	fake_complete(&fake, 0, 0u);
	cia402_device_service(&dev, 2u);
	fake_complete(&fake, 0, 0x0021u);
	cia402_device_service(&dev, 3u);
	expect_started(&fake, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD, CIA402_CW_SWITCH_ON);
	fake_complete(&fake, 0, 0u);
	cia402_device_service(&dev, 4u);
	fake_complete(&fake, 0, 0x0023u);
	cia402_device_service(&dev, 5u);
	expect_started(&fake, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD, CIA402_CW_ENABLE_OP);
	fake_complete(&fake, 0, 0u);
	cia402_device_service(&dev, 6u);
	fake_complete(&fake, 0, 0x0027u);
	cia402_device_service(&dev, 7u);
	CHECK(cia402_device_is_enabled(&dev) != 0);
	CHECK(cia402_device_take_result(&dev) == CIA402_DEVICE_RESULT_DONE);
	CHECK(cia402_device_busy(&dev) == 0);
}

static void test_pp(void)
{
	struct fake_access fake = {0};
	struct cia402_device dev;

	fake.access.start = fake_start;
	fake.access.take_result = fake_take_result;
	fake.access.abort = fake_abort;
	fake.access.ctx = &fake;
	cia402_device_init(&dev, &fake.access, 1u);
	dev.en_state = CIA402_DEVICE_EN_ENABLED;
	dev.controlword = CIA402_CW_ENABLE_OP;

	CHECK(cia402_device_pp_start(&dev, 100, 10, 20, 20) == 0);
	cia402_device_service(&dev, 0u);
	expect_started(&fake, SDO_DIR_WRITE, CIA402_OD_PROFILE_VELOCITY, 10u);
	fake_complete(&fake, 0, 0u);
	cia402_device_service(&dev, 1u);
	expect_started(&fake, SDO_DIR_WRITE, CIA402_OD_PROFILE_ACCEL, 20u);
	fake_complete(&fake, 0, 0u);
	cia402_device_service(&dev, 2u);
	expect_started(&fake, SDO_DIR_WRITE, CIA402_OD_PROFILE_DECEL, 20u);
	fake_complete(&fake, 0, 0u);
	cia402_device_service(&dev, 3u);
	expect_started(&fake, SDO_DIR_WRITE, CIA402_OD_TARGET_POSITION, 100u);
	fake_complete(&fake, 0, 0u);
	cia402_device_service(&dev, 4u);
	expect_started(&fake, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD,
		       CIA402_CW_ENABLE_OP | CIA402_CW_NEW_SETPOINT);
	fake_complete(&fake, 0, 0u);
	cia402_device_service(&dev, 5u);
	expect_started(&fake, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD, CIA402_CW_ENABLE_OP);
	fake_complete(&fake, 0, 0u);
	cia402_device_service(&dev, 6u);
	expect_started(&fake, SDO_DIR_READ, CIA402_OD_STATUSWORD, 0u);
	fake_complete(&fake, 0, 0x0027u);
	cia402_device_service(&dev, 7u);
	fake_complete(&fake, 0, 0x0027u | CIA402_SW_TARGET_REACHED);
	cia402_device_service(&dev, 8u);
	CHECK(cia402_device_take_result(&dev) == CIA402_DEVICE_RESULT_DONE);
	CHECK(dev.controlword == CIA402_CW_ENABLE_OP);
}

static void test_quick_stop_preempts(void)
{
	struct fake_access fake = {0};
	struct cia402_device dev;

	fake.access.start = fake_start;
	fake.access.take_result = fake_take_result;
	fake.access.abort = fake_abort;
	fake.access.ctx = &fake;
	cia402_device_init(&dev, &fake.access, 1u);
	dev.en_state = CIA402_DEVICE_EN_ENABLED;
	CHECK(cia402_device_pp_start(&dev, 100, 10, 20, 20) == 0);
	cia402_device_service(&dev, 0u);
	expect_started(&fake, SDO_DIR_WRITE, CIA402_OD_PROFILE_VELOCITY, 10u);

	CHECK(cia402_device_quick_stop(&dev) == 0);
	cia402_device_service(&dev, 1u);
	CHECK(fake.aborts == 1u);
	fake_complete(&fake, -1, 0u);
	cia402_device_service(&dev, 2u);
	cia402_device_service(&dev, 3u);
	expect_started(&fake, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD, CIA402_CW_QUICK_STOP);
	fake_complete(&fake, 0, 0u);
	cia402_device_service(&dev, 4u);
	fake_complete(&fake, 0, 0x0040u);
	cia402_device_service(&dev, 5u);
	expect_started(&fake, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD, CIA402_CW_DISABLE);
	fake_complete(&fake, 0, 0u);
	cia402_device_service(&dev, 6u);
	CHECK(cia402_device_is_enabled(&dev) == 0);
	CHECK(cia402_device_take_result(&dev) == CIA402_DEVICE_RESULT_DONE);
}

static void test_timeout(void)
{
	struct fake_access fake = {0};
	struct cia402_device dev;

	fake.access.start = fake_start;
	fake.access.take_result = fake_take_result;
	fake.access.abort = fake_abort;
	fake.access.ctx = &fake;
	cia402_device_init(&dev, &fake.access, 1u);

	CHECK(cia402_device_request_enable(&dev) == 0);
	cia402_device_service(&dev, 0u);
	cia402_device_service(&dev, 200u + 1u);
	CHECK(fake.aborts == 1u);
	fake_complete(&fake, -1, 0u);
	cia402_device_service(&dev, 200u + 2u);
	CHECK(cia402_device_failed(&dev) != 0);
	CHECK(cia402_device_take_result(&dev) == CIA402_DEVICE_RESULT_FAILED);
}
