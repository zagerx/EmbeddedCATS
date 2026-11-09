/*
 * can_thread.h — 控制线程：1 ms 绝对节拍，独占 cats_control_*
 *
 * 为什么单独起线程：Qt 的定时器在界面线程里派发，重绘、日志、对话框都会把节拍
 * 往后推。控制节拍不能受界面影响，所以让它自己一个线程，线程里只做一件事：
 *
 *   建主站 → 初始化 app → while (运行) { cats_control_poll(); 睡到下一拍的绝对时刻; }
 *
 * 线程归属（与 control.h 的线程约定一致）：
 *   - 本类只在**控制线程**里调 cats_control_*，是状态机的唯一写者；
 *   - 界面线程只通过只读接口看状态，用 cats_control_estop() 投急停请求。
 *
 * 所有权：CAN 通道由集成层打开与关闭，本类只借用指针，不 open / close。
 */

#ifndef BINDINGS_CAN_THREAD_H
#define BINDINGS_CAN_THREAD_H

#include <canopen/master.h>

#include <QString>
#include <atomic>
#include <cstdint>
#include <future>
#include <thread>

class CanThread
{
      public:
	/* 节拍统计：界面拿它显示"节拍稳不稳"。
	 * 只在控制线程里累加，读的一方看到的是近似值，够诊断用 */
	struct Stats {
		uint32_t ticks;      /* 已推进的拍数 */
		uint32_t max_gap_us; /* 相邻两拍的最大间隔（µs） */
		uint32_t late_ticks; /* 间隔超过 2 拍的次数 */
	};

	CanThread() = default;
	~CanThread();

	CanThread(const CanThread &) = delete;
	CanThread &operator=(const CanThread &) = delete;

	/* 起线程：线程内建主站（登记节点 1 / 2）并 cats_control_init，随后进 1 ms 循环。
	 * 会等初始化结果再返回；失败时 err 里是给界面看的说明。 */
	bool start(struct can_io *bus, QString *err);

	/* 停线程（幂等）：置停止标志并 join，最多等一拍。通道由调用方自己关 */
	void stop();

	Stats stats() const;

      private:
	void run(std::promise<bool> init_done);
	void run_svc(); /* ③ 服务线程：跑会阻塞的 402 流程 */

	struct can_io *bus_ = nullptr;
	struct co_master master_ {
	};
	std::thread thread_;
	std::thread svc_thread_; /* ③ 服务线程（会阻塞，独立一线程） */
	std::atomic<bool> stop_{false};
	std::atomic<uint32_t> ticks_{0};
	std::atomic<uint32_t> max_gap_us_{0};
	std::atomic<uint32_t> late_ticks_{0};
};

#endif /* BINDINGS_CAN_THREAD_H */
