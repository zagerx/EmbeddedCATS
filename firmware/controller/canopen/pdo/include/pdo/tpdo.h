/*
 * tpdo.h — 收取从站 TPDO 并把载荷解成主站可用的字段
 *
 * 从站的 TPDO1 / TPDO2 布局由 apps/tools/drive_setup 写入，本模块是它的
 * 主站侧镜像：
 *   TPDO1（5 字节）：6041 状态字(16) + 603F 错误码(16) + 6061 当前模式(8)
 *   TPDO2（8 字节）：6064 实际位置(32) + 606C 实际速度(32)
 *
 * 接收走本机闲置的 RPDO 接收槽（本机 OD 的 1400h 全部禁用，槽位空转），
 * 因此不需要改对象字典，也不需要 CANopenEditor。回调与解包都在 CANopen
 * 工作线程里执行，与 SDO 通道同一个线程，不使用锁。
 *
 * Author: zager
 */

#ifndef PDO_TPDO_H
#define PDO_TPDO_H

#include <master/master.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 一条 TPDO 的各字段长度，用于校验实际帧长 */
#define TPDO1_LENGTH 5u
#define TPDO2_LENGTH 8u

/* 解出来的最新值；两路 TPDO 各更新自己负责的字段 */
struct tpdo_frame {
	uint16_t statusword; /* TPDO1：6041h */
	uint16_t error_code; /* TPDO1：603Fh */
	uint8_t mode;        /* TPDO1：6061h */
	int32_t position;    /* TPDO2：6064h，指令单位 */
	int32_t velocity;    /* TPDO2：606Ch，指令单位/s */
};

/* 收帧上下文：每路 TPDO 一个，作为接收回调的 object 传入 */
struct tpdo_rx {
	struct tpdo_node *node;
	uint8_t channel; /* 0 = TPDO1，1 = TPDO2 */
};

struct tpdo_node {
	struct master *master; /* 只用于注册与注销接收槽 */
	uint8_t node_id;
	uint8_t attached;        /* 非 0：两路接收槽都已占上 */
	uint32_t slot[2];        /* 占用的接收槽下标 */
	struct tpdo_rx rx[2];    /* 接收回调上下文 */
	struct tpdo_frame frame; /* 最新解出的值 */
	uint32_t frames[2];      /* 各通道累计帧数，上层据此判断是否还在更新 */
};

/**
 * tpdo_attach - 在已建好的协议栈上占用两路接收槽
 * @t: 节点对象，生命周期内地址不变
 * @m: 已完成通信初始化的主站
 * @node_id: 从站站号
 *
 * 协议栈每次重建后都要重新调用（接收槽属于协议栈，不随主站保留）。
 *
 * Return: 0 成功；-1 参数无效或没有空闲接收槽。
 */
int tpdo_attach(struct tpdo_node *t, struct master *m, uint8_t node_id);

/**
 * tpdo_frames - 某一路 TPDO 的累计帧数
 * @t: 节点对象；NULL 安全
 * @channel: 0 = TPDO1，1 = TPDO2
 *
 * Return: 累计帧数；入参无效返回 0。值只在收到帧时增加。
 */
uint32_t tpdo_frames(const struct tpdo_node *t, uint8_t channel);

/**
 * tpdo_values - 读取最新解出的值
 * @t: 节点对象；NULL 安全
 *
 * Return: 与节点同生命周期的快照指针；NULL 入参返回 NULL。
 */
const struct tpdo_frame *tpdo_values(const struct tpdo_node *t);

#ifdef __cplusplus
}
#endif

#endif /* PDO_TPDO_H */
