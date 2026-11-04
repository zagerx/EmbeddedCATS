/*
 * cia402_sdo.c — 一次 SDO 事务（读或写一个对象），用上游原语驱动
 *
 * 只回答三件事：
 *   能不能发       cia402_sdo_read() / cia402_sdo_write()
 *   回来没有       cia402_sdo_step()，每拍推一步
 *   结果是什么     cia402_sdo_result()
 *
 * 不认识 402（不知道 6040 / 6041 是什么），也不决定"什么时候发"——
 * 发的时机归 cia402_enable.c，本文件只当一次传输的载体。
 *
 * 为什么不是阻塞等：cats_app_poll() 1 ms 一拍，上游 Upload/Download 恰好是
 * 步进函数，等待期返回 waitingResponse，所以天然能"每拍走一步"。
 */

#include "cia402_internal.h"

#include <canopen/sdo_client.h>

#include <stddef.h>

/*
 * 单次 SDO 的等待上限。上游在 initiate 时把它记进对象里，超时自己报 abort
 * （CO_SDO_AB_TOD），所以本层不需要另设计时器。
 */
#define SDO_TIMEOUT_MS 200u

static CO_SDOclient_t *client_of(const struct cia402_node *n)
{
	return co_master_sdo_client(n->master, n->node_id);
}

int cia402_sdo_read(struct cia402_node *n, uint16_t index, uint8_t sub)
{
	CO_SDOclient_t *cli = client_of(n);

	n->abort_code = 0;
	n->data_len = 0;
	if (cli == NULL || CO_SDOclientUploadInitiate(cli, index, sub, SDO_TIMEOUT_MS, false) < 0) {
		n->sdo = CIA402_SDO_FAILED;
		return -1;
	}
	n->sdo = CIA402_SDO_READING;
	return 0;
}

int cia402_sdo_write(struct cia402_node *n, uint16_t index, uint8_t sub, uint32_t value,
		     uint8_t size)
{
	CO_SDOclient_t *cli = client_of(n);
	uint8_t data[4];
	uint8_t i;

	n->abort_code = 0;
	if (cli == NULL || size == 0U || size > sizeof(data)) {
		n->sdo = CIA402_SDO_FAILED;
		return -1;
	}
	for (i = 0; i < size; i++) {
		data[i] = (uint8_t)((value >> (8U * i)) & 0xFFu); /* SDO 用小端 */
	}
	if (CO_SDOclientDownloadInitiate(cli, index, sub, size, SDO_TIMEOUT_MS, false) < 0 ||
	    CO_SDOclientDownloadBufWrite(cli, data, size) != size) {
		n->sdo = CIA402_SDO_FAILED;
		return -1;
	}
	n->sdo = CIA402_SDO_WRITING;
	return 0;
}

void cia402_sdo_step(struct cia402_node *n, uint32_t diff_us)
{
	CO_SDOclient_t *cli;
	CO_SDO_abortCode_t abort_code = CO_SDO_AB_NONE;
	size_t size_indicated = 0;
	size_t size_transferred = 0;
	CO_SDO_return_t ret;

	if (n->sdo != CIA402_SDO_READING && n->sdo != CIA402_SDO_WRITING) {
		return; /* 没有在途事务 */
	}
	cli = client_of(n);
	if (cli == NULL) {
		n->sdo = CIA402_SDO_FAILED;
		return;
	}

	if (n->sdo == CIA402_SDO_READING) {
		ret = CO_SDOclientUpload(cli, diff_us, false, &abort_code, &size_indicated,
					 &size_transferred, NULL);
	} else {
		ret = CO_SDOclientDownload(cli, diff_us, false, false, &abort_code,
					   &size_transferred, NULL);
	}
	if (ret > 0) {
		return; /* 还在等驱动器应答 */
	}

	if (ret != CO_SDO_RT_ok_communicationEnd) {
		n->abort_code = (uint32_t)abort_code; /* 驱动器回的 abort，诊断用 */
		n->sdo = CIA402_SDO_FAILED;
	} else {
		if (n->sdo == CIA402_SDO_READING) {
			n->data_len =
				(uint8_t)CO_SDOclientUploadBufRead(cli, n->data, sizeof(n->data));
		}
		n->sdo = CIA402_SDO_DONE;
	}
	CO_SDOclientClose(cli);
}

int cia402_sdo_result(struct cia402_node *n)
{
	if (n->sdo == CIA402_SDO_DONE) {
		n->sdo = CIA402_SDO_IDLE;
		return 1;
	}
	if (n->sdo == CIA402_SDO_FAILED) {
		n->sdo = CIA402_SDO_IDLE;
		return -1;
	}
	return 0;
}

int cia402_write_controlword(struct cia402_node *n, uint16_t value)
{
	int r = cia402_sdo_write(n, CIA402_IDX_CONTROLWORD, 0, (uint32_t)value, 2);

	if (r == 0) {
		n->controlword = value; /* 记住当前值，后续按位改（bit4 / bit8） */
	}
	return r;
}
