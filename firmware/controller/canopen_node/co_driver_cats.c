/*
 * co_driver_cats.c — CANopenNode 的 CO_driver 在本机的实现
 *
 * 平台相关的东西只有两件：
 *   发：把 CO_CANtx_t（ident + DLC + data[8]）交给 bus 的 can_io_send；
 *   收：把 bus 收到的帧按 ident/mask 匹配到 rxArray，调栈注册的回调。
 *
 * 没有 TX 中断、没有硬件过滤：栈配 useCANrxFilters = false（软件匹配）。
 * 因此 CO_LOCK_CAN_SEND / CO_UNLOCK_CAN_SEND 是空宏（见 CO_driver_target.h）。
 */

#include "co_driver_cats.h"

#include <stddef.h>
#include <string.h>

void co_cats_can_init(struct co_cats_can *c, struct can_io *bus)
{
	if (c == NULL) {
		return;
	}
	c->bus = bus;
	c->tx_error = 0u;
}

void CO_CANsetConfigurationMode(void *CANptr)
{
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

	(void)CANbitRate; /* 波特率由平台侧配置（ip link set canX type can bitrate …） */

	if (CANmodule == NULL || rxArray == NULL || txArray == NULL) {
		return CO_ERROR_ILLEGAL_ARGUMENT;
	}

	CANmodule->CANptr = CANptr;
	CANmodule->rxArray = rxArray;
	CANmodule->rxSize = rxSize;
	CANmodule->txArray = txArray;
	CANmodule->txSize = txSize;
	CANmodule->CANerrorStatus = 0;
	CANmodule->CANnormal = false;
	CANmodule->useCANrxFilters = false; /* 软件匹配：没有硬件过滤器 */
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
	}
	return CO_ERROR_NO;
}

void CO_CANmodule_disable(CO_CANmodule_t *CANmodule)
{
	(void)CANmodule;
}

CO_ReturnError_t CO_CANrxBufferInit(CO_CANmodule_t *CANmodule, uint16_t index, uint16_t ident,
				    uint16_t mask, bool_t rtr, void *object,
				    void (*CANrx_callback)(void *object, void *message))
{
	if (CANmodule == NULL || object == NULL || CANrx_callback == NULL ||
	    index >= CANmodule->rxSize) {
		return CO_ERROR_ILLEGAL_ARGUMENT;
	}

	CANmodule->rxArray[index].object = object;
	CANmodule->rxArray[index].CANrx_callback = CANrx_callback;
	CANmodule->rxArray[index].ident = (uint16_t)(ident & 0x07FFU);
	if (rtr) {
		CANmodule->rxArray[index].ident |= 0x0800U;
	}
	CANmodule->rxArray[index].mask = (uint16_t)((mask & 0x07FFU) | 0x0800U);
	return CO_ERROR_NO;
}

CO_CANtx_t *CO_CANtxBufferInit(CO_CANmodule_t *CANmodule, uint16_t index, uint16_t ident,
			       bool_t rtr, uint8_t noOfBytes, bool_t syncFlag)
{
	CO_CANtx_t *buffer;

	(void)rtr; /* 本工程只用标准数据帧 */

	if (CANmodule == NULL || index >= CANmodule->txSize) {
		return NULL;
	}
	buffer = &CANmodule->txArray[index];
	buffer->ident = (uint32_t)(ident & 0x07FFU);
	buffer->DLC = noOfBytes;
	buffer->bufferFull = false;
	buffer->syncFlag = syncFlag;
	return buffer;
}

CO_ReturnError_t CO_CANsend(CO_CANmodule_t *CANmodule, CO_CANtx_t *buffer)
{
	struct co_cats_can *can;
	struct can_io_frame frame;

	if (CANmodule == NULL || buffer == NULL || CANmodule->CANptr == NULL) {
		return CO_ERROR_ILLEGAL_ARGUMENT;
	}
	can = CANmodule->CANptr;
	if (can->bus == NULL) {
		return CO_ERROR_ILLEGAL_ARGUMENT;
	}

	frame.id = (uint32_t)(buffer->ident & 0x07FFU);
	frame.dlc = (buffer->DLC > CAN_IO_MAX_DLC) ? CAN_IO_MAX_DLC : buffer->DLC;
	memcpy(frame.data, buffer->data, frame.dlc);
	if (can_io_send(can->bus, &frame) != CAN_IO_OK) {
		can->tx_error = 1u;
		CANmodule->CANerrorStatus |= CO_CAN_ERRTX_OVERFLOW;
		return CO_ERROR_TX_OVERFLOW;
	}
	can->tx_error = 0u;
	/* 同步发完：boot-up 之后不再有"首帧"特权 */
	if (CANmodule->firstCANtxMessage) {
		CANmodule->firstCANtxMessage = false;
	}
	return CO_ERROR_NO;
}

void CO_CANclearPendingSyncPDOs(CO_CANmodule_t *CANmodule)
{
	uint16_t i;
	CO_CANtx_t *buffer;

	if (CANmodule == NULL) {
		return;
	}
	/* 同步发：没有"已提交但没发出去"的帧，只需清标记 */
	CANmodule->bufferInhibitFlag = false;
	buffer = &CANmodule->txArray[0];
	for (i = CANmodule->txSize; i > 0U; i--) {
		buffer->bufferFull = false;
		buffer++;
	}
	CANmodule->CANtxCount = 0U;
}

void CO_CANmodule_process(CO_CANmodule_t *CANmodule)
{
	struct co_cats_can *can;

	if (CANmodule == NULL) {
		return;
	}
	can = CANmodule->CANptr;
	/* 本机的 bus 层不报总线错误计数：只在发失败时置 overflow 位 */
	if (can != NULL && can->tx_error != 0u) {
		CANmodule->CANerrorStatus |= CO_CAN_ERRTX_OVERFLOW;
	} else {
		CANmodule->CANerrorStatus = 0u;
	}
}

/* ---- 收：由 I/O 线程每拍调 ---- */

void co_cats_can_receive(CO_CANmodule_t *module, const struct can_io_frame *frame)
{
	CO_CANrxMsg_t msg;
	uint16_t index;
	CO_CANrx_t *buffer;

	if (module == NULL || frame == NULL || module->rxArray == NULL) {
		return;
	}

	msg.ident = (uint16_t)(frame->id & 0x07FFU);
	msg.DLC = frame->dlc;
	memcpy(msg.data, frame->data, (frame->dlc > 8u) ? 8u : frame->dlc);

	buffer = &module->rxArray[0];
	for (index = module->rxSize; index > 0U; index--) {
		/* RTR 位在 mask 里固定为 1：数据帧进来时与 ident 的 0 位相符 */
		if (((msg.ident ^ buffer->ident) & buffer->mask) == 0U) {
			if (buffer->CANrx_callback != NULL) {
				buffer->CANrx_callback(buffer->object, &msg);
			}
			return;
		}
		buffer++;
	}
}
