/*
 * sdo_arbiter.h — 一条 SDO 通道上的控制与反馈会话仲裁
 *
 * 控制请求永远可以登记：底层忙时等待，反馈结束后自动发起。
 * 反馈请求只在控制空闲时受理，不排队。底层结果只交付给发起它的会话。
 * 所有函数仅由同一个 CANopen 工作线程调用。
 *
 * Author: zager
 */

#ifndef TRANSPORT_SDO_ARBITER_H
#define TRANSPORT_SDO_ARBITER_H

#include <transport/sdo_access.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sdo_arbiter;
struct sdo_client;

void sdo_arbiter_init(struct sdo_arbiter *a, struct sdo_client *client);
void sdo_arbiter_step(struct sdo_arbiter *a);

/* 控制与反馈各持有一个接口；生命周期与仲裁器一致 */
const struct sdo_access *sdo_arbiter_control(struct sdo_arbiter *arbiter);
const struct sdo_access *sdo_arbiter_feedback(struct sdo_arbiter *arbiter);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_SDO_ARBITER_H */
