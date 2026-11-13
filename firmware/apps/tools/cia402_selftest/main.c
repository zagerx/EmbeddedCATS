/* DS402 状态字解析的纯逻辑自测。 */

#include <cia402/pds.h>

#include <stdio.h>

int cia402_device_tests(void);

static int s_fail;

#define CHECK(cond)                                                                                \
	do {                                                                                       \
		if (!(cond)) {                                                                     \
			printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                   \
			s_fail++;                                                                  \
		}                                                                                  \
	} while (0)

static void test_parse(void)
{
	printf("parse：状态字 → 状态\n");
	CHECK(cia402_parse(0x0000u) == CIA402_NOT_READY);
	CHECK(cia402_parse(0x0040u) == CIA402_SWITCH_ON_DISABLED);
	CHECK(cia402_parse(0x0021u) == CIA402_READY);
	CHECK(cia402_parse(0x0023u) == CIA402_SWITCHED_ON);
	CHECK(cia402_parse(0x0027u) == CIA402_OP_ENABLED);
	CHECK(cia402_parse(0x0008u) == CIA402_FAULT); /* 故障 */
	CHECK(cia402_parse(0x000Fu) == CIA402_FAULT); /* 故障反应中 */
	CHECK(cia402_parse(0x1234u) == CIA402_STATE_UNKNOWN);
	/* 高位置位不影响判定（只看 bit0..3 / bit5 / bit6） */
	CHECK(cia402_parse(0x0627u) == CIA402_OP_ENABLED);
}

int main(void)
{
	test_parse();
	s_fail += cia402_device_tests();

	if (s_fail == 0) {
		printf("cia402_selftest: 全部通过\n");
		return 0;
	}
	printf("cia402_selftest: %d 处失败\n", s_fail);
	return 1;
}
