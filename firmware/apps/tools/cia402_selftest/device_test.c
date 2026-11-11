/*
 * device_test.c — 用假异步传输验证设备状态机
 *
 * 覆盖投递规则（受理/忙/不合法/不允许）、使能链、PP 到位、抢占、
 * 超时、以及 Quick Stop 的"失败也要走完安全链"。不依赖 CAN 硬件。
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

struct fx {
	struct fake_access fake;
	struct cia402_device dev;
	struct cia402_status st;
	uint32_t t;
};

static int fake_start(void *ctx, uint8_t dir, uint16_t index, uint8_t sub, uint32_t value,
		      uint8_t size, uint32_t timeout_ms);
static int fake_take_result(void *ctx, struct sdo_result *result);
static void fake_abort(void *ctx);

static void fx_init(struct fx *f);
static void fx_request(struct fx *f, enum cia402_kind kind);
static void fx_poll(struct fx *f);
static void fx_poll_at(struct fx *f, uint32_t t);
static void fx_complete(struct fx *f, int status, uint32_t value);
static void expect_started(struct fx *f, uint8_t dir, uint16_t index, uint32_t value);

static void test_request_rules(void);
static void test_enable(void);
static void test_pp(void);
static void test_quick_stop_preempts(void);
static void test_quick_stop_timeout_chain(void);
static void test_start_rejected(void);
static void test_cancel_beats_pending_failure(void);
static void test_timeout(void);

static int failures;

#define CHECK(cond)                                                                                \
	do {                                                                                       \
		if (!(cond)) {                                                                     \
			printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                   \
			failures++;                                                                \
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
	test_request_rules();
	test_enable();
	test_pp();
	test_quick_stop_preempts();
	test_quick_stop_timeout_chain();
	test_start_rejected();
	test_cancel_beats_pending_failure();
	test_timeout();
	return failures;
}

/* ---- 假传输层 ---- */

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

/* ---- 测试脚手架 ---- */

static void fx_init(struct fx *f)
{
	memset(f, 0, sizeof(*f));
	f->fake.access.start = fake_start;
	f->fake.access.take_result = fake_take_result;
	f->fake.access.abort = fake_abort;
	f->fake.access.ctx = &f->fake;
	cia402_init(&f->dev, &f->fake.access, 1u);
}

static void fx_request(struct fx *f, enum cia402_kind kind)
{
	struct cia402_request req;

	memset(&req, 0, sizeof(req));
	req.kind = kind;
	if (kind == CIA402_REQ_MOVE) {
		req.pp.target = 100;
		req.pp.velocity = 10;
		req.pp.accel = 20;
		req.pp.decel = 20;
	}
	CHECK(cia402_request(&f->dev, &req) == CIA402_ACCEPTED);
}

static void fx_poll_at(struct fx *f, uint32_t t)
{
	f->t = t;
	cia402_poll(&f->dev, t, &f->st);
}

static void fx_poll(struct fx *f)
{
	fx_poll_at(f, f->t + 1u);
}

static void fx_complete(struct fx *f, int status, uint32_t value)
{
	memset(&f->fake.result, 0, sizeof(f->fake.result));
	f->fake.result.status = (int8_t)status;
	f->fake.result.value = value;
	f->fake.result.len = (f->fake.dir == SDO_DIR_READ) ? f->fake.size : 0u;
	f->fake.result_ready = 1u;
}

static void expect_started(struct fx *f, uint8_t dir, uint16_t index, uint32_t value)
{
	CHECK(f->fake.started != 0u);
	CHECK(f->fake.dir == dir);
	CHECK(f->fake.index == index);
	if (dir == SDO_DIR_WRITE) {
		CHECK(f->fake.value == value);
	}
}

/* ---- 用例 ---- */

/* 投递规则：不合法、不允许、忙 */
static void test_request_rules(void)
{
	struct fx f;
	struct cia402_request req;

	fx_init(&f);

	/* 参数不合法 */
	memset(&req, 0, sizeof(req));
	req.kind = (enum cia402_kind)99;
	CHECK(cia402_request(&f.dev, &req) == CIA402_INVALID);
	req.kind = CIA402_REQ_MOVE;
	req.pp.velocity = 0; /* 速度 0 等于不动 */
	CHECK(cia402_request(&f.dev, &req) == CIA402_INVALID);

	/* 未配置不能使能 */
	memset(&req, 0, sizeof(req));
	req.kind = CIA402_REQ_ENABLE;
	CHECK(cia402_request(&f.dev, &req) == CIA402_REFUSED);

	/* 未使能不能运动 */
	f.dev.facts.configured = 1u;
	req.kind = CIA402_REQ_MOVE;
	req.pp.velocity = 10;
	req.pp.accel = 20;
	req.pp.decel = 20;
	CHECK(cia402_request(&f.dev, &req) == CIA402_REFUSED);

	/* 忙：上一条没结束 */
	req.kind = CIA402_REQ_ENABLE;
	CHECK(cia402_request(&f.dev, &req) == CIA402_ACCEPTED);
	CHECK(cia402_request(&f.dev, &req) == CIA402_BUSY);

	/* 抢占不受忙限制 */
	req.kind = CIA402_REQ_QUICK_STOP;
	CHECK(cia402_request(&f.dev, &req) == CIA402_ACCEPTED);
}

/* 使能链：Switch on disabled → Ready → Switched on → Operation enabled */
static void test_enable(void)
{
	struct fx f;

	fx_init(&f);
	f.dev.facts.configured = 1u;
	fx_request(&f, CIA402_REQ_ENABLE);

	fx_poll(&f);
	CHECK(f.st.phase == CIA402_PHASE_ENABLE);
	expect_started(&f, SDO_DIR_READ, CIA402_OD_STATUSWORD, 0u);

	fx_complete(&f, 0, 0x0040u);
	fx_poll(&f);
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD, CIA402_CW_SHUTDOWN);

	fx_complete(&f, 0, 0u);
	fx_poll(&f);
	expect_started(&f, SDO_DIR_READ, CIA402_OD_STATUSWORD, 0u);

	fx_complete(&f, 0, 0x0021u);
	fx_poll(&f);
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD, CIA402_CW_SWITCH_ON);

	fx_complete(&f, 0, 0u);
	fx_poll(&f);
	fx_complete(&f, 0, 0x0023u);
	fx_poll(&f);
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD, CIA402_CW_ENABLE_OP);

	fx_complete(&f, 0, 0u);
	fx_poll(&f);
	fx_complete(&f, 0, 0x0027u);
	fx_poll(&f);

	CHECK(f.st.enabled != 0);
	CHECK(f.st.result == CIA402_RESULT_DONE);
	CHECK(f.st.phase == CIA402_PHASE_IDLE);
}

/* PP：写四个参数 → bit4 边沿 → 等到位 */
static void test_pp(void)
{
	struct fx f;

	fx_init(&f);
	f.dev.facts.enabled = 1u;
	f.dev.xfer.cw_ok = CIA402_CW_ENABLE_OP;
	fx_request(&f, CIA402_REQ_MOVE);

	fx_poll(&f);
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_PROFILE_VELOCITY, 10u);
	fx_complete(&f, 0, 0u);
	fx_poll(&f);
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_PROFILE_ACCEL, 20u);
	fx_complete(&f, 0, 0u);
	fx_poll(&f);
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_PROFILE_DECEL, 20u);
	fx_complete(&f, 0, 0u);
	fx_poll(&f);
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_TARGET_POSITION, 100u);
	fx_complete(&f, 0, 0u);
	fx_poll(&f);
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD,
		       CIA402_CW_ENABLE_OP | CIA402_CW_NEW_SETPOINT);
	fx_complete(&f, 0, 0u);
	fx_poll(&f);
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD, CIA402_CW_ENABLE_OP);
	fx_complete(&f, 0, 0u);
	fx_poll(&f);
	expect_started(&f, SDO_DIR_READ, CIA402_OD_STATUSWORD, 0u);
	fx_complete(&f, 0, 0x0027u); /* 还没到位 */
	fx_poll(&f);
	fx_complete(&f, 0, 0x0027u | CIA402_SW_TARGET_REACHED);
	fx_poll(&f);

	CHECK(f.st.result == CIA402_RESULT_DONE);
	CHECK(f.dev.xfer.cw_ok == CIA402_CW_ENABLE_OP);
}

/* Quick Stop 抢占在途 PP */
static void test_quick_stop_preempts(void)
{
	struct fx f;

	fx_init(&f);
	f.dev.facts.enabled = 1u;
	fx_request(&f, CIA402_REQ_MOVE);
	fx_poll(&f);
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_PROFILE_VELOCITY, 10u);

	{
		const struct cia402_request req = {.kind = CIA402_REQ_QUICK_STOP};

		CHECK(cia402_request(&f.dev, &req) == CIA402_ACCEPTED);
	}
	fx_poll(&f);
	CHECK(f.fake.aborts == 1u);
	fx_complete(&f, -1, 0u); /* 被 abort 的旧事务 */
	fx_poll(&f);
	/* 旧流程作废：不能留下失败结论 */
	CHECK(f.st.result == CIA402_RESULT_NONE);
	fx_poll(&f);
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD, CIA402_CW_QUICK_STOP);

	fx_complete(&f, 0, 0u);
	fx_poll(&f);
	fx_complete(&f, 0, 0x0040u);
	fx_poll(&f);
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD, CIA402_CW_DISABLE);

	fx_complete(&f, 0, 0u);
	fx_poll(&f);
	CHECK(f.st.enabled == 0u);
	CHECK(f.st.result == CIA402_RESULT_DONE);
}

/* 安全链：qstop 写超时仍读状态，读超时仍断使能 */
static void test_quick_stop_timeout_chain(void)
{
	struct fx f;

	fx_init(&f);
	f.dev.facts.enabled = 1u;
	fx_request(&f, CIA402_REQ_MOVE);
	fx_poll(&f);
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_PROFILE_VELOCITY, 10u);

	{
		const struct cia402_request req = {.kind = CIA402_REQ_QUICK_STOP};

		CHECK(cia402_request(&f.dev, &req) == CIA402_ACCEPTED);
	}
	fx_poll(&f);
	fx_complete(&f, -1, 0u);
	fx_poll(&f);
	fx_poll(&f);
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD, CIA402_CW_QUICK_STOP);

	/* ① qstop 写超时：仍必须去读状态字 */
	fx_poll_at(&f, f.t + 300u);
	fx_complete(&f, -1, 0u);
	fx_poll(&f);
	expect_started(&f, SDO_DIR_READ, CIA402_OD_STATUSWORD, 0u);

	/* ② 读状态超时：仍必须写断使能 */
	fx_poll_at(&f, f.t + 300u);
	fx_complete(&f, -1, 0u);
	fx_poll(&f);
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD, CIA402_CW_DISABLE);

	/* ③ 最后一步成功才收尾 */
	fx_complete(&f, 0, 0u);
	fx_poll(&f);
	CHECK(f.st.enabled == 0u);
	CHECK(f.st.result == CIA402_RESULT_DONE);
}

/* 发起被立即拒绝：下一拍给出失败结论 */
static void test_start_rejected(void)
{
	struct fx f;

	fx_init(&f);
	f.dev.facts.enabled = 1u;
	fx_request(&f, CIA402_REQ_MOVE);

	f.fake.started = 1u; /* 让 fake_start 直接拒绝 */
	fx_poll(&f);
	fx_poll(&f);

	CHECK(f.st.result == CIA402_RESULT_FAILED);
	CHECK(f.st.phase == CIA402_PHASE_IDLE);
	CHECK(f.st.failed != 0u);
}

/* 已被抢占的流程，其未分发的失败不能泄漏成结论 */
static void test_cancel_beats_pending_failure(void)
{
	struct fx f;

	fx_init(&f);
	f.dev.facts.enabled = 1u;
	f.dev.xfer.cw_ok = CIA402_CW_ENABLE_OP;
	fx_request(&f, CIA402_REQ_MOVE);

	f.fake.started = 1u; /* 第一笔写被拒：rejected 置位 */
	fx_poll(&f);
	f.fake.started = 0u;

	{
		const struct cia402_request req = {.kind = CIA402_REQ_HALT};

		CHECK(cia402_request(&f.dev, &req) == CIA402_ACCEPTED);
	}
	fx_poll(&f);
	CHECK(f.st.result == CIA402_RESULT_NONE); /* 旧失败作废 */

	fx_poll(&f);
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD,
		       CIA402_CW_ENABLE_OP | CIA402_CW_HALT);
}

/* 单笔事务超时：报失败并带上原因 */
static void test_timeout(void)
{
	struct fx f;

	fx_init(&f);
	f.dev.facts.configured = 1u;
	fx_request(&f, CIA402_REQ_ENABLE);

	fx_poll_at(&f, 0u);
	fx_poll_at(&f, 300u); /* 超过单笔事务期限 */
	CHECK(f.fake.aborts == 1u);
	fx_complete(&f, -1, 0u);
	fx_poll(&f);

	CHECK(f.st.result == CIA402_RESULT_FAILED);
	CHECK(f.st.fail == CIA402_FAIL_TIMEOUT);
	CHECK(f.st.phase == CIA402_PHASE_IDLE);
}
