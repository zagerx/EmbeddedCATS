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
 * 一个服务节点一条通道：OD 的 1280h / 1281h 各自绑一个站号（COB-ID +
 * server node-ID），上游 CO_SDOclient_init() 在启动时就绑好了。所以两个轴
 * 各自用自己的客户端，传输互不打断；调用方只说“我要和站号 N 说话”，不关心
 * 下标怎么排。
 */

#ifndef CANOPEN_SDO_CLIENT_H
#define CANOPEN_SDO_CLIENT_H

#include <canopen/master.h>

#include "301/CO_SDOclient.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 取绑定了该站号的 SDO 客户端；没配这个站点或协议栈没起来时返回 NULL */
CO_SDOclient_t *co_master_sdo_client(struct co_master *m, uint8_t node_id);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CANOPEN_SDO_CLIENT_H */
