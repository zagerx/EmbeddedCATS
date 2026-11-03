/*
 * co_driver_cats.c — 用 can_io 实现 CANopenNode 的 CO_CANmodule
 *
 * 上游要求下列符号由"驱动"提供（见 301/CO_driver.h 的声明）：
 *   CO_CANsetConfigurationMode / CO_CANsetNormalMode / CO_CANmodule_init
 *   CO_CANmodule_disable / CO_CANrxBufferInit / CO_CANtxBufferInit
 *   CO_CANsend / CO_CANclearPendingSyncPDOs / CO_CANmodule_process
 *
 * 与上游示例驱动的差别：发送是同步的（can_io_send 直接写 socket），没有硬件
 * 发送队列，所以 bufferFull 恒为 false；接收没有中断，由 co_driver_cats_receive()
 * 在轮询节拍里抽帧并分派。
 */

#include "co_driver_cats.h"

#include <canopen/can_io.h>

#include <stddef.h>
#include <string.h>

/* CO_CANrx_t 里的 RTR 标志位：本工程只跑经典帧，该位恒为 0 */
#define CO_RX_RTR_BIT 0x0800u

/* 经典帧的 11 位标识符上限 */
#define CAN_STD_ID_MAX 0x07FFu

static struct can_io *bus_of(CO_CANmodule_t *module)
{
	return (struct can_io *)module->CANptr;
}

/* ---- 上游要求实现的接口 ---- */

void CO_CANsetConfigurationMode(void *CANptr)
{
	/* can_io 没有"配置模式"：接口上下线由壳用 ip link 管 */
	(void)CANptr;
}

void CO_CANsetNormalMode(CO_CANmodule_t *CANmodule)
{
	if (CANmodule != NULL) {
		CANmodule->CANnormal = true;
	}
}

CO_ReturnError_t CO_CANmodule_init(CO_CANmodule_t *CANmodule, void *CANptr, CO_CANrx_t rxArray[],
				   uint16_t rxSize, CO_CANtx_t txArray[], uint16_t txSize,
				   uint16_t CANbitRate)
{
	uint16_t i;

	if (CANmodule == NULL || CANptr == NULL || rxArray == NULL || txArray == NULL) {
		return CO_ERROR_ILLEGAL_ARGUMENT;
	}
	(void)CANbitRate; /* 波特率由 can0 接口自身配置（ip link），本层不参与 */

	CANmodule->CANptr = CANptr;
	CANmodule->rxArray = rxArray;
	CANmodule->rxSize = rxSize;
	CANmodule->txArray = txArray;
	CANmodule->txSize = txSize;
	CANmodule->CANerrorStatus = 0;
	CANmodule->CANnormal = false;
	CANmodule->useCANrxFilters = false; /* 过滤在软件里做（分派时比对 ident/mask） */
	CANmodule->bufferInhibitFlag = false;
	CANmodule->firstCANtxMessage = true;
	CANmodule->CANtxCount = 0U;
	CANmodule->errOld = 0U;

	for (i = 0U; i < rxSize; i++) {
		rxArray[i].ident = 0U;
		rxArray[i].mask = 0xFFFFU;
		rxArray[i].object = NULL;
		rxArray[i].CANrx_callback = NULL;
	}
	for (i = 0U; i < txSize; i++) {
		txArray[i].bufferFull = false;
		txArray[i].syncFlag = false;
	}
	return CO_ERROR_NO;
}

void CO_CANmodule_disable(CO_CANmodule_t *CANmodule)
{
	if (CANmodule != NULL) {
		CANmodule->CANnormal = false;
	}
}

CO_ReturnError_t CO_CANrxBufferInit(CO_CANmodule_t *CANmodule, uint16_t index, uint16_t ident,
				    uint16_t mask, bool_t rtr, void *object,
				    void (*CANrx_callback)(void *object, void *message))
{
	CO_CANrx_t *buffer;

	if (CANmodule == NULL || object == NULL || CANrx_callback == NULL ||
	    index >= CANmodule->rxSize) {
		return CO_ERROR_ILLEGAL_ARGUMENT;
	}

	buffer = &CANmodule->rxArray[index];
	buffer->object = object;
	buffer->CANrx_callback = CANrx_callback;

	/* ident/mask 只留 11 位；RTR 标志单独占一位，与上游一致 */
	buffer->ident = (uint16_t)(ident & CAN_STD_ID_MAX);
	if (rtr) {
		buffer->ident |= CO_RX_RTR_BIT;
	}
	buffer->mask = (uint16_t)((mask & CAN_STD_ID_MAX) | CO_RX_RTR_BIT);

	return CO_ERROR_NO;
}

CO_CANtx_t *CO_CANtxBufferInit(CO_CANmodule_t *CANmodule, uint16_t index, uint16_t ident,
			       bool_t rtr, uint8_t noOfBytes, bool_t syncFlag)
{
	CO_CANtx_t *buffer;

	if (CANmodule == NULL || index >= CANmodule->txSize) {
		return NULL;
	}

	buffer = &CANmodule->txArray[index];
	buffer->ident = (uint32_t)(ident & CAN_STD_ID_MAX);
	buffer->DLC = noOfBytes;
	buffer->bufferFull = false;
	buffer->syncFlag = syncFlag;
	(void)rtr; /* 本工程不发远程帧 */

	return buffer;
}

CO_ReturnError_t CO_CANsend(CO_CANmodule_t *CANmodule, CO_CANtx_t *buffer)
{
	struct can_io_frame frame;
	struct can_io *bus;

	if (CANmodule == NULL || buffer == NULL) {
		return CO_ERROR_ILLEGAL_ARGUMENT;
	}

	if (buffer->bufferFull) {
		if (!CANmodule->firstCANtxMessage) {
			CANmodule->CANerrorStatus |= CO_CAN_ERRTX_OVERFLOW;
		}
		return CO_ERROR_TX_OVERFLOW;
	}

	bus = bus_of(CANmodule);
	if (bus == NULL) {
		return CO_ERROR_TX_UNCONFIGURED;
	}

	frame.id = buffer->ident & CAN_STD_ID_MAX;
	frame.dlc = buffer->DLC;
	memcpy(frame.data, buffer->data, sizeof(frame.data));

	if (can_io_send(bus, &frame) != CAN_IO_OK) {
		return CO_ERROR_SYSCALL; /* 接口没 up / 驱动拒绝，由上层决定怎么处理 */
	}

	CANmodule->firstCANtxMessage = false;
	CANmodule->bufferInhibitFlag = buffer->syncFlag;
	return CO_ERROR_NO;
}

void CO_CANclearPendingSyncPDOs(CO_CANmodule_t *CANmodule)
{
	uint16_t i;
	CO_CANtx_t *buffer;
	uint32_t dropped = 0U;

	if (CANmodule == NULL) {
		return;
	}
	if (CANmodule->bufferInhibitFlag) {
		CANmodule->bufferInhibitFlag = false;
		dropped = 1U;
	}
	buffer = &CANmodule->txArray[0];
	for (i = CANmodule->txSize; i > 0U; i--) {
		if (buffer->bufferFull && buffer->syncFlag) {
			buffer->bufferFull = false;
			CANmodule->CANtxCount--;
			dropped = 2U;
		}
		buffer++;
	}
	if (dropped != 0U) {
		CANmodule->CANerrorStatus |= CO_CAN_ERRTX_PDO_LATE;
	}
}

void CO_CANmodule_process(CO_CANmodule_t *CANmodule)
{
	/* socketcan 的错误计数器本层读不到（需要 netlink），暂不更新错误状态 */
	(void)CANmodule;
}

/* ---- 本工程特有：接收分派 ---- */

/*
 * 收到的帧依次比对已注册的槽：命中就调它的回调并返回。
 * 上游各协议对象注册的 COB-ID 互不重叠，所以命中即返回。
 */
static void dispatch_frame(CO_CANmodule_t *CANmodule, const struct can_io_frame *frame)
{
	CO_CANrxMsg_t msg;
	uint16_t i;

	if (frame->id > CAN_STD_ID_MAX) {
		return; /* 扩展帧：本工程不处理，宁可丢掉也不能被低 11 位误认 */
	}

	msg.ident = (uint16_t)frame->id;
	msg.DLC = frame->dlc;
	memcpy(msg.data, frame->data, sizeof(msg.data));

	for (i = 0U; i < CANmodule->rxSize; i++) {
		const CO_CANrx_t *rx = &CANmodule->rxArray[i];

		if (rx->CANrx_callback == NULL) {
			continue;
		}
		if ((msg.ident & rx->mask) != rx->ident) {
			continue;
		}
		rx->CANrx_callback(rx->object, &msg);
		return;
	}
}

void co_driver_cats_receive(CO_CANmodule_t *CANmodule)
{
	struct can_io *bus;
	struct can_io_frame frame;
	uint32_t n = 0U;

	if (CANmodule == NULL) {
		return;
	}
	bus = bus_of(CANmodule);
	if (bus == NULL) {
		return;
	}

	while (n < CO_DRIVER_RX_MAX_PER_POLL) {
		if (can_io_recv(bus, &frame, 0) != CAN_IO_OK) {
			break; /* 抽干了 */
		}
		dispatch_frame(CANmodule, &frame);
		n++;
	}
}
