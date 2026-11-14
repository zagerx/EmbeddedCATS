/*
 * main.c — SDO 仲裁器的纯逻辑自测
 *
 * 用假底层通道验证控制优先、反馈取消、结果归属和排队请求启动。
 * 不依赖 CAN 硬件或 CANopenNode。
 *
 * Author: zager
 */

#include "sdo_arbiter_internal.h"

#include <stdio.h>
#include <string.h>

struct fake_client {
	struct sdo_client client;
	unsigned aborts;
};

static int failures;

#define CHECK(cond)                                                                                \
	do {                                                                                       \
		if (!(cond)) {                                                                     \
			printf("FAIL %d: %s\n", __LINE__, #cond);                                  \
			failures++;                                                                \
		}                                                                                  \
	} while (0)

static int fake_start(void *ctx, uint8_t dir, uint16_t index, uint8_t sub, uint32_t value,
		      uint8_t size, uint32_t timeout_ms);
static int fake_take(void *ctx, struct sdo_result *result);
static void fake_abort(void *ctx);
static void fake_complete(struct sdo_arbiter *a, int status, uint32_t value);
static void test_control_preempts_feedback(void);
static void test_control_result_isolated(void);

int main(void)
{
	test_control_preempts_feedback();
	test_control_result_isolated();
	if (failures != 0) {
		printf("sdo_arbiter_selftest: %d 项失败\n", failures);
		return 1;
	}
	puts("sdo_arbiter_selftest: 全部通过");
	return 0;
}

static int fake_start(void *ctx, uint8_t dir, uint16_t index, uint8_t sub, uint32_t value,
		      uint8_t size, uint32_t timeout_ms)
{
	struct sdo_client *client = ctx;

	(void)sub;
	(void)timeout_ms;
	(void)index;
	if (client->state != SDO_CLIENT_IDLE || (dir == SDO_DIR_READ && size != 0u) ||
	    (dir == SDO_DIR_WRITE && size == 0u)) {
		return -1;
	}
	client->dir = dir;
	client->result = (struct sdo_result){.status = -1, .value = value, .abort_code = size};
	client->state = SDO_CLIENT_RUNNING;
	return 0;
}

static int fake_take(void *ctx, struct sdo_result *result)
{
	struct sdo_client *client = ctx;

	if (client->state != SDO_CLIENT_DONE) {
		return 1;
	}
	*result = client->result;
	client->state = SDO_CLIENT_IDLE;
	return 0;
}

static void fake_abort(void *ctx)
{
	struct fake_client *fake = (struct fake_client *)ctx;

	fake->aborts++;
}

static void fake_complete(struct sdo_arbiter *a, int status, uint32_t value)
{
	a->client->result = (struct sdo_result){.status = (int8_t)status, .value = value};
	a->client->state = SDO_CLIENT_DONE;
	sdo_arbiter_step(a);
}

static void test_control_preempts_feedback(void)
{
	struct fake_client fake;
	struct sdo_arbiter arbiter;
	const struct sdo_access *control;
	const struct sdo_access *feedback;
	struct sdo_result result;

	memset(&fake, 0, sizeof(fake));
	fake.client.access.start = fake_start;
	fake.client.access.take_result = fake_take;
	fake.client.access.abort = fake_abort;
	fake.client.access.ctx = &fake.client;
	sdo_arbiter_init(&arbiter, &fake.client);
	control = sdo_arbiter_control(&arbiter);
	feedback = sdo_arbiter_feedback(&arbiter);

	CHECK(feedback->start(feedback->ctx, SDO_DIR_READ, 0x6041u, 0u, 0u, 0u, 100u) == 0);
	CHECK(control->start(control->ctx, SDO_DIR_WRITE, 0x6060u, 0u, 1u, 1u, 200u) == 0);
	CHECK(fake.aborts == 1u);
	sdo_arbiter_step(&arbiter);
	CHECK(fake.aborts == 1u);
	fake_complete(&arbiter, -1, 0u);
	CHECK(arbiter.owner == SDO_ARBITER_CONTROL);
	CHECK(feedback->take_result(feedback->ctx, &result) == 0);
	CHECK(result.status == -1);
	fake_complete(&arbiter, 0, 0u);
	CHECK(control->take_result(control->ctx, &result) == 0);
	CHECK(result.status == 0);
}

static void test_control_result_isolated(void)
{
	struct fake_client fake;
	struct sdo_arbiter arbiter;
	const struct sdo_access *control;
	const struct sdo_access *feedback;
	struct sdo_result result;

	memset(&fake, 0, sizeof(fake));
	fake.client.access.start = fake_start;
	fake.client.access.take_result = fake_take;
	fake.client.access.abort = fake_abort;
	fake.client.access.ctx = &fake.client;
	sdo_arbiter_init(&arbiter, &fake.client);
	control = sdo_arbiter_control(&arbiter);
	feedback = sdo_arbiter_feedback(&arbiter);

	CHECK(control->start(control->ctx, SDO_DIR_WRITE, 0x6060u, 0u, 1u, 1u, 200u) == 0);
	fake_complete(&arbiter, 0, 0u);
	CHECK(feedback->take_result(feedback->ctx, &result) == 1);
	CHECK(control->take_result(control->ctx, &result) == 0);
	CHECK(result.status == 0);
	CHECK(feedback->start(feedback->ctx, SDO_DIR_READ, 0x6041u, 0u, 0u, 0u, 100u) == 0);
}
