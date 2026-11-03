/*
 * cia402.c — 每节点一个 402 推进器（有状态，每拍推进一步）
 *
 * 使能序列（手册 6.1.4 的步骤 3~5、6，这里把"先定模式"提到最前）：
 *
 *   6060 = 1（PP） → 6040 = 06（等 0x21） → 6040 = 07（等 0x23）
 *   → 6040 = 0F（等 0x27） → 完成
 *
 * 每一步都是"写控制字 → 读状态字确认"，两步都走 SDO。SDO 用上游原语驱动：
 * 本文件只负责"这一步该发什么、结果对不对、下一步是什么"，不实现协议本身。
 *
 * 为什么不卡在循环里等：cats_app_poll() 1 ms 一拍，任何一步都不许阻塞。
 * 上游的 Upload/Download 恰好是步进函数，等待期返回 waitingResponse 即可。
 */

#include <cia402/cia402.h>

#include <canopen/sdo_client.h>

#include <string.h>

/* 402 用到的对象（只在这里出现号码） */
#define IDX_CONTROLWORD 0x6040u
#define IDX_STATUSWORD 0x6041u
#define IDX_MODE 0x6060u

/* 控制字（DS 402 命令字，手册 5.3 的位定义） */
#define CW_SHUTDOWN 0x0006u  /* 电压输出 + 允许急停 */
#define CW_SWITCH_ON 0x0007u /* 再置位 bit0 = 启动 */
#define CW_ENABLE_OP 0x000Fu /* 再置位 bit3 = 允许操作 */

/* 6060 = 1 是 PP（协议位置模式），手册 5.1.3 */
#define MODE_PP 1u

/* SDO 的 COB-ID（DS 301 默认）：0x600/0x580 + 站号 */
#define SDO_COB_ID_CLIENT_TO_SERVER 0x600u
#define SDO_COB_ID_SERVER_TO_CLIENT 0x580u

/*
 * 单次 SDO 的等待上限。上游在 initiate 时把它记进对象里，超时自己报 abort
 * （CO_SDO_AB_TOD），所以本层不需要另设计时器。
 */
#define SDO_TIMEOUT_MS 200u

/* 一个需要写控制字的步骤：写什么、等状态字的哪个结果 */
struct step_cmd {
	enum cia402_step step;
	uint16_t controlword;
	enum cia402_state expect;
};

static const struct step_cmd STEPS[] = {
	{CIA402_STEP_SHUTDOWN, CW_SHUTDOWN, CIA402_READY},
	{CIA402_STEP_SWITCH_ON, CW_SWITCH_ON, CIA402_SWITCHED_ON},
	{CIA402_STEP_ENABLE, CW_ENABLE_OP, CIA402_OP_ENABLED},
};

#define STEP_COUNT (sizeof(STEPS) / sizeof(STEPS[0]))

/* ---- SDO 事务：发起 / 推进 / 取结果（都用上游原语）---- */

static int sdo_setup(struct cia402_node *n, CO_SDOclient_t **out)
{
	CO_SDOclient_t *cli = co_master_sdo_client(n->master);

	if (cli == NULL) {
		return -1;
	}
	if (CO_SDOclient_setup(cli, SDO_COB_ID_CLIENT_TO_SERVER + n->node_id,
			       SDO_COB_ID_SERVER_TO_CLIENT + n->node_id,
			       n->node_id) != CO_SDO_RT_ok_communicationEnd) {
		return -1;
	}
	*out = cli;
	return 0;
}

static void tx_issue_read(struct cia402_node *n, uint16_t index, uint8_t sub)
{
	CO_SDOclient_t *cli;

	n->abort_code = 0;
	if (sdo_setup(n, &cli) != 0 ||
	    CO_SDOclientUploadInitiate(cli, index, sub, SDO_TIMEOUT_MS, false) < 0) {
		n->tx = CIA402_TX_FAILED;
		return;
	}
	n->tx = CIA402_TX_READ;
}

static void tx_issue_write(struct cia402_node *n, uint16_t index, uint8_t sub, uint32_t value,
			   uint8_t size)
{
	CO_SDOclient_t *cli;
	uint8_t data[4];
	uint8_t i;

	n->abort_code = 0;
	if (size == 0U || size > sizeof(data) || sdo_setup(n, &cli) != 0) {
		n->tx = CIA402_TX_FAILED;
		return;
	}
	for (i = 0; i < size; i++) {
		data[i] = (uint8_t)((value >> (8U * i)) & 0xFFu); /* 小端 */
	}
	if (CO_SDOclientDownloadInitiate(cli, index, sub, size, SDO_TIMEOUT_MS, false) < 0 ||
	    CO_SDOclientDownloadBufWrite(cli, data, size) != size) {
		n->tx = CIA402_TX_FAILED;
		return;
	}
	n->tx = CIA402_TX_WRITE;
}

/* 推进一拍；事务结束时把结果摆进 tx（DONE / FAILED）等推进器取 */
static void tx_step(struct cia402_node *n, uint32_t diff_us)
{
	CO_SDOclient_t *cli;
	CO_SDO_abortCode_t abort_code = CO_SDO_AB_NONE;
	size_t size_indicated = 0;
	size_t size_transferred = 0;
	CO_SDO_return_t ret;

	if (n->tx != CIA402_TX_READ && n->tx != CIA402_TX_WRITE) {
		return; /* 没有在途事务 */
	}
	cli = co_master_sdo_client(n->master);
	if (cli == NULL) {
		n->tx = CIA402_TX_FAILED;
		return;
	}

	if (n->tx == CIA402_TX_READ) {
		ret = CO_SDOclientUpload(cli, diff_us, false, &abort_code, &size_indicated,
					 &size_transferred, NULL);
	} else {
		ret = CO_SDOclientDownload(cli, diff_us, false, false, &abort_code,
					   &size_transferred, NULL);
	}
	if (ret > 0) {
		return; /* 还在等驱动器应答 */
	}

	if (ret == CO_SDO_RT_ok_communicationEnd) {
		if (n->tx == CIA402_TX_READ) {
			uint8_t buf[4] = {0, 0, 0, 0};
			uint8_t len = (uint8_t)CO_SDOclientUploadBufRead(cli, buf, sizeof(buf));

			if (len > 0U) {
				n->statusword = (uint16_t)(buf[0] |
							   (uint16_t)(len > 1U ? buf[1] : 0U) << 8);
			}
			n->state = cia402_parse(n->statusword);
		}
		n->tx = CIA402_TX_DONE;
	} else {
		n->abort_code = (uint32_t)abort_code;
		n->tx = CIA402_TX_FAILED;
	}
	CO_SDOclientClose(cli);
}

/* 事务结束了没有：1 = 成功（已取走），-1 = 失败（已取走），0 = 还在途 */
static int tx_result(struct cia402_node *n)
{
	if (n->tx == CIA402_TX_DONE) {
		n->tx = CIA402_TX_NONE;
		return 1;
	}
	if (n->tx == CIA402_TX_FAILED) {
		n->tx = CIA402_TX_NONE;
		return -1;
	}
	return 0;
}

/* ---- 使能序列 ---- */

static const struct step_cmd *cmd_of(enum cia402_step step)
{
	size_t i;

	for (i = 0; i < STEP_COUNT; i++) {
		if (STEPS[i].step == step) {
			return &STEPS[i];
		}
	}
	return NULL;
}

static enum cia402_step cmd_next(const struct step_cmd *cmd)
{
	size_t i = (size_t)(cmd - STEPS);

	return (i + 1U < STEP_COUNT) ? STEPS[i + 1U].step : CIA402_STEP_ENABLED;
}

/* 推进"写控制字 → 读状态字"一轮；substep 记住这一轮走到哪儿 */
static void run_step(struct cia402_node *n, const struct step_cmd *cmd)
{
	switch (n->substep) {
	case 0: /* 该写控制字 */
		tx_issue_write(n, IDX_CONTROLWORD, 0, cmd->controlword, 2);
		n->substep = 1;
		break;

	case 1: { /* 等写结果 */
		int r = tx_result(n);

		if (r == 0) {
			break;
		}
		if (r < 0) {
			n->step = CIA402_STEP_FAILED;
			break;
		}
		n->substep = 2;
		break;
	}

	default: { /* 等读结果：不对就再读一次，对了就进下一步 */
		int r;

		if (n->tx == CIA402_TX_NONE) {
			tx_issue_read(n, IDX_STATUSWORD, 0);
			break;
		}
		r = tx_result(n);
		if (r == 0) {
			break;
		}
		if (r < 0) {
			n->step = CIA402_STEP_FAILED;
			break;
		}
		if (n->state == CIA402_FAULT) {
			n->step = CIA402_STEP_FAILED;
			break;
		}
		if (n->state == cmd->expect) {
			n->step = cmd_next(cmd);
			n->substep = 0;
		}
		break;
	}
	}
}

static void drive_step(struct cia402_node *n)
{
	const struct step_cmd *cmd;
	int r;

	if (n->step == CIA402_STEP_SET_MODE) {
		if (n->substep == 0) {
			tx_issue_write(n, IDX_MODE, 0, MODE_PP, 1);
			n->substep = 1;
			return;
		}
		r = tx_result(n);
		if (r < 0) {
			n->step = CIA402_STEP_FAILED;
		} else if (r > 0) {
			n->step = CIA402_STEP_SHUTDOWN;
			n->substep = 0;
		}
		return;
	}

	cmd = cmd_of(n->step);
	if (cmd == NULL) {
		n->step = CIA402_STEP_FAILED;
		return;
	}
	run_step(n, cmd);
}

/* ---- 对外接口 ---- */

void cia402_init(struct cia402_node *n, struct co_master *m, uint8_t node_id)
{
	memset(n, 0, sizeof(*n));
	n->master = m;
	n->node_id = node_id;
	n->step = CIA402_STEP_IDLE;
	n->state = CIA402_STATE_UNKNOWN;
}

int cia402_request_enable(struct cia402_node *n)
{
	if (n == NULL || n->master == NULL || n->node_id == 0U) {
		return -1;
	}
	if (n->step != CIA402_STEP_IDLE) {
		return 1; /* 已经要求过：不重复发起 */
	}
	n->step = CIA402_STEP_SET_MODE;
	n->substep = 0;
	n->tx = CIA402_TX_NONE;
	n->abort_code = 0;
	return 0;
}

void cia402_poll(struct cia402_node *n, uint32_t now_ms)
{
	uint32_t diff_us = 0;

	if (n == NULL) {
		return;
	}
	if (n->started) {
		diff_us = (uint32_t)(now_ms - n->last_ms) * 1000u; /* 无符号相减处理回绕 */
	}
	n->last_ms = now_ms;
	n->started = 1;

	tx_step(n, diff_us);

	if (n->step == CIA402_STEP_IDLE || n->step == CIA402_STEP_ENABLED ||
	    n->step == CIA402_STEP_FAILED) {
		return; /* 不在推进中 */
	}
	drive_step(n);
}

int cia402_is_enabled(const struct cia402_node *n)
{
	return n != NULL && n->step == CIA402_STEP_ENABLED && n->state == CIA402_OP_ENABLED;
}

int cia402_failed(const struct cia402_node *n)
{
	return n != NULL && n->step == CIA402_STEP_FAILED;
}
