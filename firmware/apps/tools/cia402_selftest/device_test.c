/*
 * device_test.c — 用假异步传输验证设备状态机
 *
 * 覆盖投递规则（受理/忙/不合法/不允许）、使能链、PP 到位、抢占、
 * 超时、以及 Quick Stop 的"失败也要走完安全链"。不依赖 CAN 硬件。
 *
 * Author: zager
 */

#include <cia402/device_instance.h>
#include <cia402/od.h>
#include <cia402/types.h>

#include <stdio.h>
#include <string.h>

sm_ret_t st_idle(struct sm_hsm *const me, const struct sm_event *const e);

struct fake_client {
	uint8_t started;
	uint8_t refuse; /* 强制发起拒绝 */
	uint8_t dir;
	uint8_t size;
	uint16_t index;
	uint32_t value;
	uint8_t result_ready;
	struct sdo_result result;
	unsigned aborts;
};

struct fx {
	struct fake_client fake;
	struct cia402_device dev;
	enum cia402_operation_result st;
	enum cia402_fail failure;
	uint32_t t;
};

static enum sdo_start_result fake_start(struct fake_client *fake, uint8_t dir, uint16_t index,
					uint8_t sub, uint32_t value, uint8_t size,
					uint32_t timeout_ms);

static void fx_init(struct fx *f);
static void fx_request(struct fx *f, enum cia402_kind kind);
static void fx_poll(struct fx *f);
static void fx_poll_at(struct fx *f, uint32_t t);
static void fx_complete(struct fx *f, int status, uint32_t value);
static void expect_started(struct fx *f, uint8_t dir, uint16_t index, uint32_t value);
/* 发一条带自定义目标的 PP 请求，返回受理结果 */
static enum cia402_response fx_move(struct fx *f, int32_t target);
/* 把一条 PP 流程推到“读状态字等到位”这一步，并留一笔读在途 */
static void fx_pp_run(struct fx *f);

static void test_request_rules(void);
static void test_enable(void);
static void test_pp(void);
static void test_pp_retarget(void);
static void test_home_bit13(void);
static void test_home_fault(void);
static void test_home_cancel(void);
static void test_home_preempt(void);
static void test_home_cleanup_preempt(void);
static void test_quick_stop_preempts(void);
static void test_quick_stop_write_failure(void);
static void test_quick_stop_clear(void);
static void test_start_rejected(void);
static void test_client_contract(void);
static void test_cancel_beats_pending_failure(void);
static void test_timeout(void);
static void test_waiting_not_timed_out(void);

static enum sdo_start_result fake_read(struct sdo_client *s, uint16_t index, uint32_t timeout_ms);
static enum sdo_start_result fake_write(struct sdo_client *s, uint16_t index, uint32_t value,
					uint8_t size, uint32_t timeout_ms);

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
	/* CHECK 只报行号：这里再补一条用例名，失败时能直接看出是哪一块 */
#define RUN(t)                                                                                     \
	do {                                                                                       \
		const int before = failures;                                                       \
		t();                                                                               \
		if (failures != before) {                                                          \
			printf("  [用例] %s\n", #t);                                               \
		}                                                                                  \
	} while (0)
	RUN(test_request_rules);
	RUN(test_enable);
	RUN(test_pp);
	RUN(test_pp_retarget);
	RUN(test_home_bit13);
	RUN(test_home_fault);
	RUN(test_home_cancel);
	RUN(test_home_preempt);
	RUN(test_home_cleanup_preempt);
	RUN(test_quick_stop_preempts);
	RUN(test_quick_stop_write_failure);
	RUN(test_quick_stop_clear);
	RUN(test_start_rejected);
	RUN(test_client_contract);
	RUN(test_cancel_beats_pending_failure);
	RUN(test_timeout);
	RUN(test_waiting_not_timed_out);
#undef RUN
	return failures;
}

/**
 * sdo_client_start - 测试用异步事务实现
 * @s: 假客户端对象
 * @req: 请求描述
 *
 * Return: 仅 ACCEPTED 产生结果；BUSY / INVALID 不占用通道。
 */
enum sdo_start_result sdo_client_start(struct sdo_client *s, const struct sdo_request *req)
{
	if (req == NULL) {
		return SDO_START_INVALID;
	}
	return fake_start((struct fake_client *)s, (uint8_t)req->dir, req->index, req->subindex,
			  req->value, req->size, req->timeout_ms);
}

/* 测试内部：读 / 写请求的简写 */
static enum sdo_start_result fake_read(struct sdo_client *s, uint16_t index, uint32_t timeout_ms)
{
	const struct sdo_request req = {
		.dir = SDO_DIR_READ,
		.index = index,
		.timeout_ms = timeout_ms,
	};

	return sdo_client_start(s, &req);
}

static enum sdo_start_result fake_write(struct sdo_client *s, uint16_t index, uint32_t value,
					uint8_t size, uint32_t timeout_ms)
{
	const struct sdo_request req = {
		.dir = SDO_DIR_WRITE,
		.index = index,
		.size = size,
		.value = value,
		.timeout_ms = timeout_ms,
	};

	return sdo_client_start(s, &req);
}

/**
 * sdo_client_poll - 取走测试事务结果
 * @s: 假客户端对象
 * @result: 结果输出
 *
 * Return: DONE 取走结果并释放通道；其他结果不改变输出。
 */
enum sdo_poll sdo_client_poll(struct sdo_client *s, struct sdo_result *result)
{
	struct fake_client *fake = (struct fake_client *)s;

	if (fake->result_ready != 0u) {
		*result = fake->result;
		fake->result_ready = 0u;
		fake->started = 0u;
		return SDO_POLL_DONE;
	}
	return (fake->started != 0u) ? SDO_POLL_WAITING : SDO_POLL_IDLE;
}

/**
 * sdo_client_abort - 登记测试事务取消
 * @s: 假客户端对象
 */
void sdo_client_abort(struct sdo_client *s)
{
	struct fake_client *fake = (struct fake_client *)s;

	if (fake->started != 0u) {
		fake->aborts++;
	}
}

static enum sdo_start_result fake_start(struct fake_client *fake, uint8_t dir, uint16_t index,
					uint8_t sub, uint32_t value, uint8_t size,
					uint32_t timeout_ms)
{
	(void)sub;
	(void)timeout_ms;
	if (fake->started != 0u) {
		return SDO_START_BUSY;
	}
	if (fake->refuse != 0u || timeout_ms == 0u ||
	    (dir == SDO_DIR_WRITE && (size == 0u || size > 4u))) {
		return SDO_START_INVALID;
	}
	fake->started = 1u;
	fake->dir = dir;
	fake->index = index;
	fake->value = value;
	fake->size = size;
	fake->result_ready = 0u;
	return SDO_START_ACCEPTED;
}

static void fx_init(struct fx *f)
{
	memset(f, 0, sizeof(*f));
	cia402_init(&f->dev, (struct sdo_client *)&f->fake);
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
	cia402_poll(&f->dev, t);
	f->st = cia402_take_result(&f->dev);
	if (f->st == CIA402_OP_RESULT_FAILED) {
		cia402_failure_of(&f->dev, &f->failure);
	}
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

static enum cia402_response fx_move(struct fx *f, int32_t target)
{
	struct cia402_request req;

	memset(&req, 0, sizeof(req));
	req.kind = CIA402_REQ_MOVE;
	req.pp.target = target;
	req.pp.velocity = 10;
	req.pp.accel = 20;
	req.pp.decel = 20;
	return cia402_request(&f->dev, &req);
}

static void fx_pp_run(struct fx *f)
{
	fx_poll(f); /* REQUEST：只设步骤 */
	fx_poll(f); /* TICK：发起第一笔事务 */
	expect_started(f, SDO_DIR_WRITE, CIA402_OD_PROFILE_VELOCITY, 10u);
	fx_complete(f, 0, 0u);
	fx_poll(f);
	expect_started(f, SDO_DIR_WRITE, CIA402_OD_PROFILE_ACCEL, 20u);
	fx_complete(f, 0, 0u);
	fx_poll(f);
	expect_started(f, SDO_DIR_WRITE, CIA402_OD_PROFILE_DECEL, 20u);
	fx_complete(f, 0, 0u);
	fx_poll(f);
	expect_started(f, SDO_DIR_WRITE, CIA402_OD_TARGET_POSITION, 100u);
	fx_complete(f, 0, 0u);
	fx_poll(f);
	expect_started(f, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD,
		       CIA402_CW_ENABLE_OP | CIA402_CW_NEW_SETPOINT);
	fx_complete(f, 0, 0u);
	fx_poll(f);
	expect_started(f, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD, CIA402_CW_ENABLE_OP);
	fx_complete(f, 0, 0u);
	fx_poll(f);
	expect_started(f, SDO_DIR_READ, CIA402_OD_STATUSWORD, 0u);
	fx_complete(f, 0, 0x0027u); /* 还没到位 */
	fx_poll(f);
	expect_started(f, SDO_DIR_READ, CIA402_OD_STATUSWORD, 0u);
}

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

	/* 前置条件由调用方负责：未配置也能受理使能，本模块只判忙 */
	memset(&req, 0, sizeof(req));
	req.kind = CIA402_REQ_ENABLE;
	CHECK(cia402_request(&f.dev, &req) == CIA402_ACCEPTED);

	/* 忙：上一条还没被工作线程取走 */
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
	fx_request(&f, CIA402_REQ_ENABLE);

	fx_poll(&f); /* REQUEST：只设步骤 */
	fx_poll(&f); /* TICK：发起读状态字 */
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

	CHECK(f.st == CIA402_OP_RESULT_DONE);
}

/* PP：写四个参数 → bit4 边沿 → 等到位 */
static void test_pp(void)
{
	struct fx f;

	fx_init(&f);
	f.dev.cw.confirmed = CIA402_CW_ENABLE_OP;
	fx_request(&f, CIA402_REQ_MOVE);

	fx_poll(&f); /* REQUEST：只设步骤 */
	fx_poll(&f); /* TICK：发起写速度 */
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

	CHECK(f.st == CIA402_OP_RESULT_DONE);
	CHECK(f.dev.cw.confirmed == CIA402_CW_ENABLE_OP);

	CHECK(cia402_take_result(&f.dev) == CIA402_OP_RESULT_NONE);
}

/* PP 运动中的新目标：走在线变位 0x103F，不停车、不报中间结论 */
static void test_pp_retarget(void)
{
	struct fx f;

	fx_init(&f);
	f.dev.cw.confirmed = CIA402_CW_ENABLE_OP;
	CHECK(fx_move(&f, 100) == CIA402_ACCEPTED);
	fx_pp_run(&f); /* 现在有一笔读状态字在途 */

	/* 运动中再来一条：受理，不是 BUSY */
	CHECK(fx_move(&f, 200) == CIA402_ACCEPTED);
	CHECK(f.st == CIA402_OP_RESULT_NONE);

	/* 这一笔读到的还是“没到位”：先把参数按新目标重跑一遍 */
	fx_complete(&f, 0, 0x0027u);
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
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_TARGET_POSITION, 200u); /* 新目标 */
	fx_complete(&f, 0, 0u);
	fx_poll(&f);
	/* 在线变位一帧搞定：没有 bit4 落沿那一步 */
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD, CIA402_CW_ONLINE_SETPOINT);
	fx_complete(&f, 0, 0u);
	fx_poll(&f);
	expect_started(&f, SDO_DIR_READ, CIA402_OD_STATUSWORD, 0u);
	/* 0x103F 是一次性指令，不得污染控制字镜像 */
	CHECK(f.dev.cw.confirmed == CIA402_CW_ENABLE_OP);
	CHECK(f.st == CIA402_OP_RESULT_NONE);

	/* 新目标优先于到位：这一次读到 bit10 = 1，也不能把后来的指令丢掉 */
	CHECK(fx_move(&f, 300) == CIA402_ACCEPTED);
	fx_complete(&f, 0, 0x0027u | CIA402_SW_TARGET_REACHED);
	fx_poll(&f);
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_PROFILE_VELOCITY, 10u);
	CHECK(f.st == CIA402_OP_RESULT_NONE);

	/* 把最后一段走到位 */
	fx_complete(&f, 0, 0u);
	fx_poll(&f);
	fx_complete(&f, 0, 0u);
	fx_poll(&f);
	fx_complete(&f, 0, 0u);
	fx_poll(&f);
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_TARGET_POSITION, 300u);
	fx_complete(&f, 0, 0u);
	fx_poll(&f);
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD, CIA402_CW_ONLINE_SETPOINT);
	fx_complete(&f, 0, 0u);
	fx_poll(&f);
	expect_started(&f, SDO_DIR_READ, CIA402_OD_STATUSWORD, 0u);
	fx_complete(&f, 0, 0x0027u | CIA402_SW_TARGET_REACHED);
	fx_poll(&f);
	CHECK(f.st == CIA402_OP_RESULT_DONE);
	CHECK(f.dev.cw.confirmed == CIA402_CW_ENABLE_OP);

	/* 只有 PP 在跑时才能改目标：别的流程仍按忙拒 */
	fx_init(&f);
	fx_request(&f, CIA402_REQ_ENABLE);
	CHECK(fx_move(&f, 100) == CIA402_BUSY);
}

/* 把一条回零流程推到“读状态字等结论”这一步，并留一笔读在途 */
static void fx_home_run(struct fx *f)
{
	fx_poll(f); /* REQUEST：只设步骤 */
	fx_poll(f); /* TICK：发起第一笔事务 */
	expect_started(f, SDO_DIR_WRITE, CIA402_OD_MODE, CIA402_MODE_HM);
	fx_complete(f, 0, 0u);
	fx_poll(f);
	expect_started(f, SDO_DIR_READ, CIA402_OD_MODE_ACTUAL, 0u);
	fx_complete(f, 0, CIA402_MODE_HM);
	fx_poll(f);
	expect_started(f, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD,
		       CIA402_CW_ENABLE_OP | CIA402_CW_HOMING_START);
	fx_complete(f, 0, 0u);
	fx_poll(f);
	expect_started(f, SDO_DIR_READ, CIA402_OD_STATUSWORD, 0u);
}

/* 回零收尾：落 bit4 与 halt → 切回 PP → 交回执 */
static void fx_home_restore(struct fx *f, uint16_t sw)
{
	fx_complete(f, 0, sw);
	fx_poll(f); /* 结论：迁到中止链（只进状态，不发事务） */
	fx_poll(f); /* TICK：发起落 bit4 的那一帧 */
	expect_started(f, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD, CIA402_CW_ENABLE_OP);
	fx_complete(f, 0, 0u);
	fx_poll(f);
	expect_started(f, SDO_DIR_WRITE, CIA402_OD_MODE, CIA402_MODE_PP);
	fx_complete(f, 0, 0u);
	fx_poll(f);
}

/* 回零出错：bit13 = 回零错误，要与 PDS 故障区分 */
static void test_home_bit13(void)
{
	struct fx f;

	fx_init(&f);
	f.dev.cw.confirmed = CIA402_CW_ENABLE_OP;
	fx_request(&f, CIA402_REQ_HOME);
	fx_home_run(&f);

	fx_home_restore(&f, 0x2637u); /* bit13：回零错误 */
	CHECK(f.st == CIA402_OP_RESULT_FAILED);
	CHECK(f.failure == CIA402_FAIL_HOMING);
	CHECK(f.dev.cw.confirmed == CIA402_CW_ENABLE_OP); /* bit4 已落，halt 也清了 */
}

/* 回零中的真 PDS 故障（bit3）仍报驱动器故障，走停机链 */
static void test_home_fault(void)
{
	struct fx f;

	fx_init(&f);
	f.dev.cw.confirmed = CIA402_CW_ENABLE_OP;
	fx_request(&f, CIA402_REQ_HOME);
	fx_home_run(&f);

	/* bit3 = 1、bit6 = 0 → cia402_parse 判 FAULT */
	fx_complete(&f, 0, 0x0008u);
	fx_poll(&f); /* 结论：迁到中止链 */
	fx_poll(&f); /* TICK：发起停机那一帧 */
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD,
		       CIA402_CW_ENABLE_OP | CIA402_CW_HALT);
	fx_complete(&f, 0, 0u);
	fx_poll(&f);
	expect_started(&f, SDO_DIR_READ, CIA402_OD_STATUSWORD, 0u);

	fx_home_restore(&f, 0x0737u | CIA402_SW_TARGET_REACHED);
	CHECK(f.st == CIA402_OP_RESULT_FAILED);
	CHECK(f.failure == CIA402_FAIL_DRIVER);
}

/* 单笔事务超时把回零打断：要报超时，不能冒充驱动器故障 */
static void test_home_cancel(void)
{
	struct fx f;

	fx_init(&f);
	f.dev.cw.confirmed = CIA402_CW_ENABLE_OP;
	fx_request(&f, CIA402_REQ_HOME);
	fx_home_run(&f);

	/* 通道判超时：先停电机，再收尾 */
	fx_complete(&f, SDO_STATUS_TIMEOUT, 0u);
	fx_poll(&f); /* 取消事件：迁到中止链 */
	fx_poll(&f); /* TICK：发起停机那一帧 */
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD,
		       CIA402_CW_ENABLE_OP | CIA402_CW_HALT);
	fx_complete(&f, 0, 0u);
	fx_poll(&f);
	expect_started(&f, SDO_DIR_READ, CIA402_OD_STATUSWORD, 0u);

	fx_home_restore(&f, 0x0737u | CIA402_SW_TARGET_REACHED);
	CHECK(f.st == CIA402_OP_RESULT_FAILED);
	CHECK(f.failure == CIA402_FAIL_TIMEOUT);
}

/* 抢占回零：完成停机与模式恢复，但取消不是回零成功或失败 */
static void test_home_preempt(void)
{
	struct fx f;
	const struct cia402_request req = {.kind = CIA402_REQ_HALT};

	fx_init(&f);
	f.dev.cw.confirmed = CIA402_CW_ENABLE_OP;
	fx_request(&f, CIA402_REQ_HOME);
	fx_home_run(&f);
	CHECK(cia402_request(&f.dev, &req) == CIA402_ACCEPTED);
	fx_poll(&f);
	fx_complete(&f, SDO_STATUS_CANCELLED, 0u);
	fx_poll(&f); /* 取消事件：迁到中止链 */
	fx_poll(&f); /* TICK：发起停机那一帧 */
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD,
		       CIA402_CW_ENABLE_OP | CIA402_CW_HALT);
	fx_complete(&f, 0, 0u);
	fx_poll(&f);
	expect_started(&f, SDO_DIR_READ, CIA402_OD_STATUSWORD, 0u);
	fx_home_restore(&f, 0x0737u | CIA402_SW_TARGET_REACHED);
	CHECK(f.st == CIA402_OP_RESULT_NONE);
	fx_poll(&f);                           /* 回空闲，取到后继 HALT */
	CHECK(cia402_is_running(&f.dev) != 0); /* HALT 是独立的新操作 */
	CHECK(cia402_take_result(&f.dev) == CIA402_OP_RESULT_NONE);
}

/* 回零清理链再被抢占：旧失败不得在后继取消之后补交 */
static void test_home_cleanup_preempt(void)
{
	struct fx f;

	fx_init(&f);
	f.dev.cw.confirmed = CIA402_CW_ENABLE_OP;
	fx_request(&f, CIA402_REQ_HOME);
	fx_home_run(&f);
	fx_complete(&f, 0, (1u << 3));
	fx_poll(&f); /* 结论：迁到中止链 */
	fx_poll(&f); /* TICK：发起停机那一帧 */
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD,
		       CIA402_CW_ENABLE_OP | CIA402_CW_HALT);
	fx_complete(&f, 0, 0u);
	fx_poll(&f);
	expect_started(&f, SDO_DIR_READ, CIA402_OD_STATUSWORD, 0u);

	CHECK(cia402_request(&f.dev, &(const struct cia402_request){.kind = CIA402_REQ_HALT}) ==
	      CIA402_ACCEPTED);
	fx_poll(&f);
	fx_complete(&f, SDO_STATUS_CANCELLED, 0u);
	fx_poll(&f); /* 取消事件：迁到中止链 */
	fx_poll(&f); /* TICK：发起停机那一帧 */
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD,
		       CIA402_CW_ENABLE_OP | CIA402_CW_HALT);
	fx_complete(&f, 0, 0u);
	fx_poll(&f);
	expect_started(&f, SDO_DIR_READ, CIA402_OD_STATUSWORD, 0u);
	fx_home_restore(&f, CIA402_SW_TARGET_REACHED);
	CHECK(f.st == CIA402_OP_RESULT_NONE);
}

/* Quick Stop 抢占在途 PP */
static void test_quick_stop_preempts(void)
{
	struct fx f;

	fx_init(&f);
	fx_request(&f, CIA402_REQ_MOVE);
	fx_poll(&f); /* REQUEST：只设步骤 */
	fx_poll(&f); /* TICK：发起写速度 */
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_PROFILE_VELOCITY, 10u);

	{
		const struct cia402_request req = {.kind = CIA402_REQ_QUICK_STOP};

		CHECK(cia402_request(&f.dev, &req) == CIA402_ACCEPTED);
	}
	fx_poll(&f);
	CHECK(f.fake.aborts == 1u);
	fx_complete(&f, SDO_STATUS_CANCELLED, 0u);
	fx_poll(&f);
	/* 旧流程作废：不能留下失败结论 */
	CHECK(f.st == CIA402_OP_RESULT_NONE);
	fx_poll(&f); /* 受理急停：只设步骤 */
	fx_poll(&f); /* TICK：写控制字 */
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD, CIA402_CW_QUICK_STOP);

	fx_complete(&f, 0, 0u);
	fx_poll(&f);
	CHECK(f.st == CIA402_OP_RESULT_DONE);

	/* 写完就停手：急停整条链路只有那一帧 */
	f.fake.started = 0u;
	fx_poll(&f);
	CHECK(f.fake.started == 0u);
}

/* 急停写失败：只报错，不重试、不断使能 */
static void test_quick_stop_write_failure(void)
{
	struct fx f;

	fx_init(&f);
	fx_request(&f, CIA402_REQ_MOVE);
	fx_poll(&f); /* REQUEST：只设步骤 */
	fx_poll(&f); /* TICK：发起写速度 */
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_PROFILE_VELOCITY, 10u);

	{
		const struct cia402_request req = {.kind = CIA402_REQ_QUICK_STOP};

		CHECK(cia402_request(&f.dev, &req) == CIA402_ACCEPTED);
	}
	fx_poll(&f);
	fx_complete(&f, SDO_STATUS_CANCELLED, 0u);
	fx_poll(&f); /* 取消事件：退出旧流程 */
	fx_poll(&f); /* 受理急停：只设步骤 */
	fx_poll(&f); /* TICK：写控制字 */
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD, CIA402_CW_QUICK_STOP);

	/* qstop 写超时：报失败就结束，不读状态字、不写断使能 */
	fx_poll_at(&f, f.t + 300u);
	fx_complete(&f, SDO_STATUS_TIMEOUT, 0u);
	fx_poll(&f);
	CHECK(f.st == CIA402_OP_RESULT_FAILED);
	CHECK(f.failure == CIA402_FAIL_TIMEOUT);

	/* 失败也不补发、不补断使能 */
	f.fake.started = 0u;
	fx_poll(&f);
	CHECK(f.fake.started == 0u);
}

/* 急停只写一帧（驱动器自行断使能）；终态只认解除请求，解除不写帧 */
static void test_quick_stop_clear(void)
{
	struct fx f;

	fx_init(&f);
	fx_request(&f, CIA402_REQ_QUICK_STOP);
	fx_poll(&f); /* 抢占：先结清取消事件 */
	fx_poll(&f); /* 受理请求 */
	fx_poll(&f); /* TICK：写控制字 */
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD, CIA402_CW_QUICK_STOP);
	fx_complete(&f, 0, 0u);
	fx_poll(&f);
	CHECK(f.st == CIA402_OP_RESULT_DONE);

	/* 终态期间：普通请求被丢弃，不发帧 */
	fx_request(&f, CIA402_REQ_ENABLE);
	f.fake.started = 0u;
	fx_poll(&f);
	CHECK(f.fake.started == 0u);

	/* 解除急停：不写任何控制字（断电与抱闸已在急停那一帧由驱动器完成） */
	f.fake.aborts = 0u;
	f.fake.started = 0u;
	fx_request(&f, CIA402_REQ_ESTOP_CLEAR);
	fx_poll(&f); /* 受理请求 → 直接交回执 */
	CHECK(f.fake.started == 0u);
	CHECK(f.st == CIA402_OP_RESULT_DONE);

	/* 恢复受理：使能请求能被接纳并发起读状态字 */
	f.fake.started = 0u;
	fx_request(&f, CIA402_REQ_ENABLE);
	fx_poll(&f); /* REQUEST：只设步骤 */
	fx_poll(&f); /* TICK：发起读状态字 */
	expect_started(&f, SDO_DIR_READ, CIA402_OD_STATUSWORD, 0u);
}

/* 事务根本没发起：取消流程，不产生执行失败或虚假超时 */
static void test_start_rejected(void)
{
	struct fx f;

	fx_init(&f);
	f.fake.refuse = 1u; /* 先置拒绝：让第一笔事务发起失败 */
	fx_request(&f, CIA402_REQ_MOVE);

	fx_poll(&f); /* REQUEST：只设步骤，还没有事务 */
	CHECK(f.st == CIA402_OP_RESULT_NONE);

	fx_poll(&f); /* TICK：发起失败 → 当场判失败并回到空闲 */
	CHECK(f.st == CIA402_OP_RESULT_FAILED);
	CHECK(f.failure == CIA402_FAIL_NOT_STARTED);
	CHECK(f.dev.operation.running == 0u);
	CHECK(sm_hsm_state(&f.dev.hsm) == &st_idle);

	f.fake.refuse = 0u;
	fx_request(&f, CIA402_REQ_MOVE);
	fx_poll(&f); /* REQUEST */
	fx_poll(&f); /* TICK */
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_PROFILE_VELOCITY, 10u);
}

/* 显式函数契约：拒绝无结果，已受理必有结果，取消不是402失败 */
static void test_client_contract(void)
{
	struct fx f;
	struct sdo_client *client = (struct sdo_client *)&f.fake;
	struct sdo_result result;

	fx_init(&f);
	fx_complete(&f, SDO_STATUS_FAIL, 0xDEADu);
	CHECK(fake_write(client, CIA402_OD_CONTROLWORD, 0u, 0u, 200u) == SDO_START_INVALID);
	CHECK(f.fake.started == 0u);
	CHECK(f.fake.result.value == 0xDEADu);
	CHECK(sdo_client_poll(client, &result) == SDO_POLL_DONE); /* 清理上一笔外部结果 */

	CHECK(fake_read(client, CIA402_OD_STATUSWORD, 200u) == SDO_START_ACCEPTED);
	CHECK(fake_read(client, CIA402_OD_STATUSWORD, 200u) == SDO_START_BUSY);
	fx_complete(&f, SDO_STATUS_CANCELLED, 0u);
	CHECK(sdo_client_poll(client, &result) == SDO_POLL_DONE);
	CHECK(result.status == SDO_STATUS_CANCELLED);
	CHECK(sdo_client_poll(client, &result) == SDO_POLL_IDLE);

	fx_request(&f, CIA402_REQ_ENABLE);
	fx_poll(&f); /* REQUEST：只设步骤 */
	fx_poll(&f); /* TICK：发起读状态字 */
	CHECK(cia402_request(&f.dev, &(const struct cia402_request){.kind = CIA402_REQ_HALT}) ==
	      CIA402_ACCEPTED);
	fx_poll(&f);
	fx_complete(&f, SDO_STATUS_CANCELLED, 0u);
	fx_poll(&f);
	CHECK(f.st == CIA402_OP_RESULT_NONE);
	fx_poll(&f); /* 受理 HALT：只设步骤 */
	fx_poll(&f); /* TICK：写控制字 */
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD, CIA402_CW_HALT);
}

/* 已被抢占的流程，其未分发的失败不能泄漏成结论 */
static void test_cancel_beats_pending_failure(void)
{
	struct fx f;

	fx_init(&f);
	f.dev.cw.confirmed = CIA402_CW_ENABLE_OP;
	fx_request(&f, CIA402_REQ_MOVE);
	fx_poll(&f); /* REQUEST：只设步骤 */
	fx_poll(&f); /* TICK：发起写速度 */
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_PROFILE_VELOCITY, 10u);

	/* 结果已经到达但还没被取走时被 HALT 抢占：旧失败不得冒充结论 */
	fx_complete(&f, SDO_STATUS_FAIL, 0u);
	{
		const struct cia402_request req = {.kind = CIA402_REQ_HALT};

		CHECK(cia402_request(&f.dev, &req) == CIA402_ACCEPTED);
	}
	fx_poll(&f);                          /* 取结果 → 走取消，不产生结局 */
	CHECK(f.st == CIA402_OP_RESULT_NONE); /* 旧失败作废 */

	fx_poll(&f); /* 受理 HALT：只设步骤 */
	fx_poll(&f); /* TICK：写控制字 */
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD,
		       CIA402_CW_ENABLE_OP | CIA402_CW_HALT);
}

/* 通道交付超时：报失败并带上原因 */
static void test_timeout(void)
{
	struct fx f;

	fx_init(&f);
	fx_request(&f, CIA402_REQ_ENABLE);

	fx_poll_at(&f, 0u);
	fx_poll_at(&f, 300u); /* 402 不自己计时：由通道判定 */
	CHECK(f.fake.aborts == 0u);
	CHECK(f.st == CIA402_OP_RESULT_NONE);

	fx_complete(&f, SDO_STATUS_TIMEOUT, 0u);
	fx_poll(&f);

	CHECK(f.st == CIA402_OP_RESULT_FAILED);
	CHECK(f.failure == CIA402_FAIL_TIMEOUT);
}

/* 通道还在等结果时，402 不得自作主张判超时 */
static void test_waiting_not_timed_out(void)
{
	struct fx f;

	fx_init(&f);
	fx_request(&f, CIA402_REQ_ENABLE);

	fx_poll_at(&f, 0u); /* 受理请求：只设步骤 */
	fx_poll_at(&f, 1u); /* TICK：发起读写 */
	expect_started(&f, SDO_DIR_READ, CIA402_OD_STATUSWORD, 0u);
	fx_poll_at(&f, 1000u);
	fx_poll_at(&f, 5000u);
	CHECK(f.fake.aborts == 0u);
	CHECK(f.st == CIA402_OP_RESULT_NONE);

	fx_complete(&f, 0, 0x0040u); /* 通道终于开始并完成传输 */
	fx_poll_at(&f, 5000u);
	expect_started(&f, SDO_DIR_WRITE, CIA402_OD_CONTROLWORD, CIA402_CW_SHUTDOWN);
}
