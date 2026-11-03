/*
 * sdo_client.h — 交出上游的 SDO 客户端对象（只给真正需要它的那一层）
 *
 * 为什么不写在 master.h：master.h 会被 C++ 壳（Qt）include，在里面出现上游
 * 类型会把整套协议栈头拖进 C++ 编译单元。只有 cia402 要驱动 SDO，所以单独
 * 一个头，只有它 include。
 *
 * 本层**不封装** SDO 动作：发起、每拍推进、取结果都用上游原语
 * （setup / UploadInitiate / DownloadInitiate / DownloadBufWrite /
 *   Upload / Download / UploadBufRead / Close），用法见 301/CO_SDOclient.h。
 *
 * 通道只有一条（OD_CNT_SDO_CLI = 1）：同一时刻只允许一次传输在途，
 * 谁先发起谁先传完；要并行的两个轴得由调用方自己排顺序。
 */

#ifndef CANOPEN_SDO_CLIENT_H
#define CANOPEN_SDO_CLIENT_H

#include <canopen/master.h>

#include "301/CO_SDOclient.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 取主站的 SDO 客户端对象；协议栈没起来时返回 NULL */
CO_SDOclient_t *co_master_sdo_client(struct co_master *m);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CANOPEN_SDO_CLIENT_H */
