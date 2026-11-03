/*
 * CO_driver_target.h — CANopenNode 的平台适配声明（本项目自己的版本）
 *
 * 上游 301/CO_driver.h 用 #include "CO_driver_target.h" 引用本文件，
 * 而 301/ 目录里没有这个文件，所以会落到 include 路径里找到这一份。
 *
 * 本文件只有"类型与宏"。真正的收发在 ../src/canopen/src/node/co_driver_cats.c，
 * 底层用 can_io 契约，不认识 socketcan。
 *
 * 线程模型：单线程。CANopenNode 的全部处理都跑在壳的轮询节拍里，
 * 因此临界区与内存屏障都是空的。
 */

#ifndef CO_DRIVER_TARGET_H
#define CO_DRIVER_TARGET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 字节序：本工程只跑小端平台，交换宏原样返回 */
#define CO_LITTLE_ENDIAN
#define CO_SWAP_16(x) (x)
#define CO_SWAP_32(x) (x)
#define CO_SWAP_64(x) (x)

typedef uint_fast8_t bool_t;
typedef float float32_t;
typedef double float64_t;

/* 收到的一帧：由驱动用 can_io 的帧填好，再交给协议对象 */
typedef struct {
	uint16_t ident; /* 11 位标识符 */
	uint8_t DLC;
	uint8_t data[8];
} CO_CANrxMsg_t;

#define CO_CANrxMsg_readIdent(msg) ((uint16_t)((const CO_CANrxMsg_t *)(msg))->ident)
#define CO_CANrxMsg_readDLC(msg) ((uint8_t)((const CO_CANrxMsg_t *)(msg))->DLC)
#define CO_CANrxMsg_readData(msg) ((const uint8_t *)((const CO_CANrxMsg_t *)(msg))->data)

/* 接收槽：ident 与 mask 匹配后调 CANrx_callback */
typedef struct {
	uint16_t ident;
	uint16_t mask;
	void *object;
	void (*CANrx_callback)(void *object, void *message);
} CO_CANrx_t;

/* 发送槽：ident 只放 COB-ID，长度在 DLC 里（上游只在 init 给、在 CO_CANsend 取） */
typedef struct {
	uint32_t ident; /* bit0..10 = COB-ID */
	uint8_t DLC;
	uint8_t data[8];
	volatile bool_t bufferFull;
	volatile bool_t syncFlag;
} CO_CANtx_t;

/* CAN 模块对象 */
typedef struct {
	void *CANptr; /* 指向 struct can_io */
	CO_CANrx_t *rxArray;
	uint16_t rxSize;
	CO_CANtx_t *txArray;
	uint16_t txSize;
	uint16_t CANerrorStatus;
	volatile bool_t CANnormal;
	volatile bool_t useCANrxFilters;
	volatile bool_t bufferInhibitFlag;
	volatile bool_t firstCANtxMessage;
	volatile uint16_t CANtxCount;
	uint32_t errOld;
} CO_CANmodule_t;

/* 本项目不做非易失存储（CO_CONFIG_STORAGE 未启用），所以不定义
 * CO_storage_entry_t；将来要存参数时再按 301/CO_driver.h 的字段要求补齐。 */

/* 单线程：临界区全部为空 */
#define CO_LOCK_CAN_SEND(CAN_MODULE)
#define CO_UNLOCK_CAN_SEND(CAN_MODULE)
#define CO_LOCK_EMCY(CAN_MODULE)
#define CO_UNLOCK_EMCY(CAN_MODULE)
#define CO_LOCK_OD(CAN_MODULE)
#define CO_UNLOCK_OD(CAN_MODULE)
#define CO_MemoryBarrier()

/* 接收标志：单线程下只需普通的标志读写 */
#define CO_FLAG_READ(rxNew) ((rxNew) != NULL)
#define CO_FLAG_SET(rxNew)                                                                         \
	do {                                                                                       \
		(rxNew) = (void *)1L;                                                              \
	} while (0)
#define CO_FLAG_CLEAR(rxNew)                                                                       \
	do {                                                                                       \
		(rxNew) = NULL;                                                                    \
	} while (0)

#ifdef __cplusplus
}
#endif

#endif /* CO_DRIVER_TARGET_H */
