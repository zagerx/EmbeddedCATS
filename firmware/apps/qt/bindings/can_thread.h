/*
 * CANopen 单工作线程宿主。
 * 工作线程独占协议栈、设备与整机状态机；UI 只投递命令和读取快照。
 */

#ifndef BINDINGS_CAN_THREAD_H
#define BINDINGS_CAN_THREAD_H

#include <QString>
#include <atomic>
#include <cstdint>
#include <future>
#include <thread>

class CanThread
{
      public:
	struct Stats {
		uint32_t ticks;      /* 已处理的事件循环轮数 */
		uint32_t max_gap_us; /* 相邻两轮的最大间隔（µs） */
		uint32_t late_ticks; /* 间隔超过基础周期两倍的轮数 */
	};

	CanThread() = default;
	~CanThread();

	CanThread(const CanThread &) = delete;
	CanThread &operator=(const CanThread &) = delete;

	/* 在线程内初始化接口名对应的 CANopen 主站，并等待初始化结果 */
	bool start(const QString &ifname, QString *err);

	/* 停止标志 → eventfd 唤醒 → join → controller_deinit；幂等 */
	void stop();

	Stats stats() const;

      private:
	void run(std::promise<bool> init_done);

	QString ifname_;
	std::thread thread_;
	std::atomic<bool> stop_{false};
	std::atomic<uint32_t> ticks_{0};
	std::atomic<uint32_t> max_gap_us_{0};
	std::atomic<uint32_t> late_ticks_{0};
};

#endif /* BINDINGS_CAN_THREAD_H */
