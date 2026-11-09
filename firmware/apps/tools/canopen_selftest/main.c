/*
 * canopen_selftest — SDO 报文编解码的自测（纯逻辑，不碰 CAN）
 *
 * 用断言核对字节布局：读请求 / 写请求（1..4 字节）/ 读应答 / 写确认 / abort /
 * 分段应答的分类。上真机前先保证这里全绿。
 */

#include <internal.h>

#include <stdio.h>
#include <string.h>
static int s_fail;

#define CHECK(cond)                                                                                \
	do {                                                                                       \
		if (!(cond)) {                                                                     \
			printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                   \
			s_fail++;                                                                  \
		}                                                                                  \
	} while (0)

static void expect_bytes(const uint8_t *got, const uint8_t *want, size_t n)
{
	if (memcmp(got, want, n) != 0) {
		size_t i;

		printf("  FAIL bytes:");
		for (i = 0; i < n; i++) {
			printf(" %02X", got[i]);
		}
		printf(" (want:");
		for (i = 0; i < n; i++) {
			printf(" %02X", want[i]);
		}
		printf(")\n");
		s_fail++;
	}
}

static void test_upload_req(void)
{
	uint8_t buf[8];
	const uint8_t want[8] = {0x40u, 0x41u, 0x60u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};

	co_sdo_frame_upload_req(buf, 0x6041u, 0u);
	expect_bytes(buf, want, sizeof want);
}

static void test_download_req(void)
{
	uint8_t buf[8];

	/* 6040 = 0x0006（2 字节） */
	{
		const uint8_t want[8] = {0x2Bu, 0x40u, 0x60u, 0x00u, 0x06u, 0x00u, 0x00u, 0x00u};

		co_sdo_frame_download_req(buf, 0x6040u, 0u, 0x0006u, 2u);
		expect_bytes(buf, want, sizeof want);
	}
	/* 607A = 2500（4 字节） */
	{
		const uint8_t want[8] = {0x23u, 0x7Au, 0x60u, 0x00u, 0xC4u, 0x09u, 0x00u, 0x00u};

		co_sdo_frame_download_req(buf, 0x607Au, 0u, 2500u, 4u);
		expect_bytes(buf, want, sizeof want);
	}
	/* 1 字节 / 3 字节的命令字 */
	co_sdo_frame_download_req(buf, 0x6060u, 0u, 1u, 1u);
	CHECK(buf[0] == 0x2Fu);
	co_sdo_frame_download_req(buf, 0x2000u, 0u, 0x123456u, 3u);
	CHECK(buf[0] == 0x27u);
}

static void test_parse_data(void)
{
	struct co_sdo_response rsp;
	const uint8_t frame[8] = {0x4Bu, 0x41u, 0x60u, 0x00u, 0x27u, 0x04u, 0x00u, 0x00u};

	CHECK(co_sdo_frame_parse(frame, &rsp) == CO_SDO_RSP_DATA);
	CHECK(rsp.index == 0x6041u);
	CHECK(rsp.sub == 0u);
	CHECK(rsp.len == 2u);
	CHECK(rsp.value == 0x0427u);
}

static void test_parse_ack(void)
{
	struct co_sdo_response rsp;
	const uint8_t frame[8] = {0x60u, 0x40u, 0x60u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};

	CHECK(co_sdo_frame_parse(frame, &rsp) == CO_SDO_RSP_ACK);
	CHECK(rsp.index == 0x6040u);
}

static void test_parse_abort(void)
{
	struct co_sdo_response rsp;
	const uint8_t frame[8] = {0x80u, 0x00u, 0x60u, 0x00u, 0x00u, 0x00u, 0x02u, 0x06u};

	CHECK(co_sdo_frame_parse(frame, &rsp) == CO_SDO_RSP_ABORT);
	CHECK(rsp.index == 0x6000u);
	CHECK(rsp.abort_code == 0x06020000u); /* 对象不存在 */
}

static void test_parse_segmented(void)
{
	struct co_sdo_response rsp;
	const uint8_t frame[8] = {0x41u, 0x00u, 0x10u, 0x00u, 0x04u, 0x00u, 0x00u, 0x00u};

	CHECK(co_sdo_frame_parse(frame, &rsp) == CO_SDO_RSP_SEGMENTED);
}

static void test_parse_unknown(void)
{
	struct co_sdo_response rsp;
	const uint8_t frame[8] = {0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};

	CHECK(co_sdo_frame_parse(frame, &rsp) == CO_SDO_RSP_IGNORE);
}

/* ---- 事务槽状态机（不碰总线：帧与时间都由参数给） ---- */

static void test_frame_abort(void)
{
	uint8_t buf[8];
	const uint8_t want[8] = {0x80u, 0x41u, 0x60u, 0x00u, 0x00u, 0x00u, 0x04u, 0x05u};

	co_sdo_frame_abort(buf, 0x6041u, 0u, 0x05040000u);
	expect_bytes(buf, want, sizeof want);
}

static void test_ch_write_ok(void)
{
	struct co_sdo_ch ch;
	uint8_t out[8];
	const uint8_t ack[8] = {0x60u, 0x40u, 0x60u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};

	co_sdo_ch_init(&ch);
	CHECK(co_sdo_ch_submit(&ch, 1, 0x6040u, 0u, 0x0006u, 2u, 200u) == 0);
	CHECK(co_sdo_ch_advance(&ch, 1000u, out) == CO_SDO_CH_ACT_SEND_REQ);
	CHECK(out[0] == 0x2Bu && out[1] == 0x40u && out[2] == 0x60u);
	CHECK(ch.state == CO_SDO_CH_WAIT_RSP);
	CHECK(co_sdo_ch_advance(&ch, 1001u, out) == CO_SDO_CH_ACT_NONE);
	co_sdo_ch_on_frame(&ch, ack, 1002u);
	CHECK(co_sdo_ch_take(&ch, NULL, NULL, NULL) == 1);
	CHECK(ch.state == CO_SDO_CH_IDLE);
}

static void test_ch_read_ok(void)
{
	struct co_sdo_ch ch;
	uint8_t out[8];
	uint8_t buf[4] = {0};
	uint8_t len = 0u;
	const uint8_t data[8] = {0x4Bu, 0x41u, 0x60u, 0x00u, 0x27u, 0x04u, 0x00u, 0x00u};

	co_sdo_ch_init(&ch);
	CHECK(co_sdo_ch_submit(&ch, 0, 0x6041u, 0u, 0u, 0u, 200u) == 0);
	CHECK(co_sdo_ch_advance(&ch, 0u, out) == CO_SDO_CH_ACT_SEND_REQ);
	CHECK(out[0] == 0x40u);
	co_sdo_ch_on_frame(&ch, data, 1u);
	CHECK(co_sdo_ch_take(&ch, buf, &len, NULL) == 1);
	CHECK(len == 2u);
	CHECK(buf[0] == 0x27u && buf[1] == 0x04u);
}

static void test_ch_busy(void)
{
	struct co_sdo_ch ch;
	uint8_t out[8];

	co_sdo_ch_init(&ch);
	CHECK(co_sdo_ch_submit(&ch, 0, 0x1000u, 0u, 0u, 0u, 200u) == 0);
	CHECK(co_sdo_ch_submit(&ch, 0, 0x1001u, 0u, 0u, 0u, 200u) == 1); /* 排队中：忙 */
	CHECK(co_sdo_ch_advance(&ch, 0u, out) == CO_SDO_CH_ACT_SEND_REQ);
	CHECK(co_sdo_ch_submit(&ch, 0, 0x1001u, 0u, 0u, 0u, 200u) == 1); /* 等应答：忙 */
}

/* 超时：必须发 abort 帧，结论是 TIMEOUT */
static void test_ch_timeout_sends_abort(void)
{
	struct co_sdo_ch ch;
	uint8_t out[8];
	uint32_t code = 0u;

	co_sdo_ch_init(&ch);
	CHECK(co_sdo_ch_submit(&ch, 0, 0x6041u, 0u, 0u, 0u, 10u) == 0);
	CHECK(co_sdo_ch_advance(&ch, 100u, out) == CO_SDO_CH_ACT_SEND_REQ);
	CHECK(co_sdo_ch_advance(&ch, 109u, out) == CO_SDO_CH_ACT_NONE);
	CHECK(co_sdo_ch_advance(&ch, 111u, out) == CO_SDO_CH_ACT_SEND_ABORT);
	CHECK(out[0] == 0x80u);
	CHECK(co_sdo_ch_take(&ch, NULL, NULL, &code) == -1);
	CHECK(code == 0x05040000u);
}

/* 静默期：被作废事务的迟到应答必须丢掉，不能污染空闲通道 */
static void test_ch_stale_dropped(void)
{
	struct co_sdo_ch ch;
	uint8_t out[8];
	const uint8_t late[8] = {0x4Bu, 0x41u, 0x60u, 0x00u, 0x27u, 0x04u, 0x00u, 0x00u};

	co_sdo_ch_init(&ch);
	(void)co_sdo_ch_submit(&ch, 0, 0x6041u, 0u, 0u, 0u, 10u);
	(void)co_sdo_ch_advance(&ch, 0u, out);
	(void)co_sdo_ch_advance(&ch, 20u, out); /* 超时 → 发 abort */
	(void)co_sdo_ch_take(&ch, NULL, NULL, NULL);
	CHECK(ch.state == CO_SDO_CH_IDLE);

	co_sdo_ch_on_frame(&ch, late, 30u); /* 窗口内：必须丢 */
	CHECK(co_sdo_ch_take(&ch, NULL, NULL, NULL) == 0);
	CHECK(ch.state == CO_SDO_CH_IDLE);
}

/* 取消：已发出的要补 abort，结论是失败（code = 0 表示"被取消"） */
static void test_ch_cancel_sends_abort(void)
{
	struct co_sdo_ch ch;
	uint8_t out[8];
	uint32_t code = 0xFFFFFFFFu;

	co_sdo_ch_init(&ch);
	(void)co_sdo_ch_submit(&ch, 1, 0x6040u, 0u, 0x0000u, 2u, 200u);
	(void)co_sdo_ch_advance(&ch, 0u, out);
	co_sdo_ch_cancel(&ch);
	CHECK(co_sdo_ch_take(&ch, NULL, NULL, &code) == -1);
	CHECK(code == 0u);
	CHECK(co_sdo_ch_advance(&ch, 1u, out) == CO_SDO_CH_ACT_SEND_ABORT);
	CHECK(out[0] == 0x80u);
}

/* 未发出的取消：不需要 abort 帧 */
static void test_ch_cancel_before_send(void)
{
	struct co_sdo_ch ch;
	uint8_t out[8];

	co_sdo_ch_init(&ch);
	(void)co_sdo_ch_submit(&ch, 1, 0x6040u, 0u, 0x0000u, 2u, 200u);
	co_sdo_ch_cancel(&ch);
	CHECK(co_sdo_ch_take(&ch, NULL, NULL, NULL) == -1);
	CHECK(co_sdo_ch_advance(&ch, 1u, out) == CO_SDO_CH_ACT_NONE);
}

int main(void)
{
	test_upload_req();
	test_download_req();
	test_parse_data();
	test_parse_ack();
	test_parse_abort();
	test_parse_segmented();
	test_parse_unknown();
	test_frame_abort();
	test_ch_write_ok();
	test_ch_read_ok();
	test_ch_busy();
	test_ch_timeout_sends_abort();
	test_ch_stale_dropped();
	test_ch_cancel_sends_abort();
	test_ch_cancel_before_send();

	if (s_fail == 0) {
		printf("canopen_selftest: 全部通过\n");
		return 0;
	}
	printf("canopen_selftest: %d 项失败\n", s_fail);
	return 1;
}
