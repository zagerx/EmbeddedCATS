/*
 * co_driver_cats.h — CANopenNode 驱动适配层（项目内部头，不对外发布）
 *
 * 上游规定"收帧在驱动里完成，由驱动调各对象的回调"。我们的后端没有中断，
 * 所以由调用方每拍调一次 co_driver_cats_receive()：把 can_io 里已经到达的帧
 * 抽干，再按 CO_CANrxBufferInit() 注册的 ident/mask 分派给各协议对象。
 */

#ifndef CANOPEN_NODE_CO_DRIVER_CATS_H
#define CANOPEN_NODE_CO_DRIVER_CATS_H

#include "301/CO_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 单次最多抽多少帧：防止总线被灌爆时卡住调用方的节拍 */
#define CO_DRIVER_RX_MAX_PER_POLL 32u

/*
 * 抽干总线并分派。每拍在 CO_process() 之前调一次。
 * CANmodule->CANptr 必须指向有效的 struct can_io。
 */
void co_driver_cats_receive(CO_CANmodule_t *CANmodule);

#ifdef __cplusplus
}
#endif

#endif /* CANOPEN_NODE_CO_DRIVER_CATS_H */
