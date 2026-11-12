/*
 * co_driver_cats.c — 将 CANopenNode 驱动接口适配到 CAN I/O 后端
 *
 * 发送采用同步方式，接收帧按 ident/mask 在软件中匹配。
 * 协议栈只由 I/O 线程推进，本层不保存从站节点表或 CiA 402 语义。
 *
 * Author: zager
 */

#include "co_driver_cats.h"

#include <stddef.h>
#include <string.h>

/**
 * co_cats_can_init - 初始化本机 CAN 适配句柄
 * @c: 适配句柄；NULL 时直接返回
 * @bus: CAN I/O 通道，由调用方持有
 */
void co_cats_can_init(struct co_cats_can *c, struct can_io *bus)
{
	if (c == NULL) {
		return;
	}
	c->bus = bus;
	c->tx_error = 0u;
}

/**
 * CO_CANsetConfigurationMode - 将 CAN 控制器切换到配置模式
 * @CANptr: CAN 适配句柄
 *
 * 本机由平台完成控制器配置，因此当前实现为空操作。
 */
void CO_CANsetConfigurationMode(void *CANptr)
{
	(void)CANptr;
}

/**
 * CO_CANsetNormalMode - 将 CANopenNode 模块标记为正常模式
 * @CANmodule: CANopenNode CAN 模块；NULL 时直接返回
 */
void CO_CANsetNormalMode(CO_CANmodule_t *CANmodule)
{
	if (CANmodule != NULL) {
		CANmodule->CANnormal = true;
	}
}

/**
 * CO_CANmodule_init - 初始化 CAN 模块；返回 CO_ERROR_NO 或 CO_ERROR_ILLEGAL_ARGUMENT
 * @CANmodule: CANopenNode CAN 模块
 * @CANptr: 本机 CAN 适配句柄
 * @rxArray: 接收缓冲数组
 * @rxSize: 接收缓冲数量
 * @txArray: 发送缓冲数组
 * @txSize: 发送缓冲数量
 * @CANbitRate: 波特率，本机忽略
 */
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

/**
 * CO_CANmodule_disable - 禁用 CAN 模块
 * @CANmodule: CANopenNode CAN 模块
 *
 * 当前后端无需额外禁用动作，因此为空操作。
 */
void CO_CANmodule_disable(CO_CANmodule_t *CANmodule)
{
	(void)CANmodule;
}

/**
 * CO_CANrxBufferInit - 配置接收缓冲；返回 CO_ERROR_NO 或 CO_ERROR_ILLEGAL_ARGUMENT
 * @CANmodule: CANopenNode CAN 模块
 * @index: 缓冲下标
 * @ident: CAN 标识符
 * @mask: 匹配掩码
 * @rtr: 是否匹配远程帧
 * @object: 回调上下文
 * @CANrx_callback: 接收回调
 */
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

/**
 * CO_CANtxBufferInit - 初始化一个 CAN 发送缓冲
 * @CANmodule: CANopenNode CAN 模块
 * @index: 缓冲下标
 * @ident: CAN 标识符
 * @rtr: 远程帧标记，本机忽略
 * @noOfBytes: 数据长度
 * @syncFlag: SYNC 标记
 * Return: 发送缓冲指针；模块为空或下标越界时返回 NULL。
 */
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

/**
 * CO_CANsend - 同步发送一个 CANopenNode 发送缓冲
 * @CANmodule: CANopenNode CAN 模块
 * @buffer: 待发送缓冲
 *
 * Return: CO_ERROR_NO 成功；参数无效或发送失败时返回对应错误码。
 */
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

/**
 * CO_CANclearPendingSyncPDOs - 清除同步 PDO 的待发标记
 * @CANmodule: CANopenNode CAN 模块；NULL 时直接返回
 *
 * 本机同步发送，没有额外排队帧，只需复位模块标记和计数。
 */
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

/**
 * CO_CANmodule_process - 根据后端发送结果更新模块错误状态
 * @CANmodule: CANopenNode CAN 模块；NULL 时直接返回
 *
 * 本机没有总线错误计数，只在发送失败后保留 overflow 标记。
 */
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

/**
 * co_cats_can_receive - 将一帧分派给首个匹配的接收缓冲
 * @module: CANopenNode CAN 模块
 * @frame: 已接收 CAN 帧
 *
 * 只能由 I/O 线程调用；未匹配或参数无效时直接丢弃。
 */
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
