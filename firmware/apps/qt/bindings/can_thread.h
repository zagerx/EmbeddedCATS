/*
 * 控制与服务线程宿主。控制线程使用 1 ms 绝对节拍，服务线程推进非阻塞设备状态机。
 * CAN 文件描述符由 AppBinder 持有，本类只借用。
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
		uint32_t ticks;      /* 已推进的拍数 */
		uint32_t max_gap_us; /* 相邻两拍的最大间隔（µs） */
		uint32_t late_ticks; /* 间隔超过 2 拍的次数 */
	};

	CanThread() = default;
	~CanThread();

	CanThread(const CanThread &) = delete;
	CanThread &operator=(const CanThread &) = delete;

	/* 起线程：控制线程内调用 controller_init，随后进 1 ms 循环。
	 * 会等初始化结果再返回；失败时 err 里是给界面看的说明。 */
	bool start(int fd, QString *err);

	/* 停线程（幂等）：置停止标志并 join，最多等一拍。通道由调用方自己关 */
	void stop();

	Stats stats() const;

      private:
	void run(std::promise<bool> init_done);
	void run_io();  /* I/O 线程：独占总线，收帧与发帧都在这里 */
	void run_svc(); /* 服务线程：推进非阻塞 402 状态机 */

	int fd_ = -1;
	std::thread thread_;     /* 控制线程：业务状态机 */
	std::thread io_thread_;  /* I/O 线程（不阻塞） */
	std::thread svc_thread_; /* 服务线程（独立固定节拍） */
	std::atomic<bool> stop_{false};
	std::atomic<uint32_t> ticks_{0};
	std::atomic<uint32_t> max_gap_us_{0};
	std::atomic<uint32_t> late_ticks_{0};
};

#endif /* BINDINGS_CAN_THREAD_H */
