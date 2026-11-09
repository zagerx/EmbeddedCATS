/*
 * can_thread.cpp — 控制线程的实现（全部节拍逻辑就在这一个文件里）
 *
 * 只有两件事值得记住：
 *   1. 节拍用**绝对时刻**（CLOCK_MONOTONIC + TIMER_ABSTIME），误差不累积；
 *      绝不用"睡 1 ms"那种相对写法，那会越跑越偏。
 *   2. 线程起来后先初始化再循环；初始化结果用 promise 同步给调用方，
 *      这样界面能立刻知道"通道能不能用"。
 */

#include "can_thread.h"

#include <control/control.h>

#include <chrono>

#include <time.h>
#include <utility>

namespace
{
/* 节拍：1 ms。状态机与协议栈都按这个周期推进 */
constexpr uint32_t kPeriodUs = 1000u;
/* 判定掉拍的门槛：间隔超过 2 拍才算（1 ms 定时本身有几十 µs 抖动，不算掉拍） */
constexpr uint32_t kLateThresholdUs = 2u * kPeriodUs;

/* 单调微秒时钟。注意 uint32 微秒只够 71 分钟，所以一律 uint64 */
uint64_t monotonic_us()
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<uint64_t>(ts.tv_sec) * 1000000u +
	       static_cast<uint64_t>(ts.tv_nsec) / 1000u;
}

/* 睡到"绝对时刻" deadline_us；早于该时刻返回才是正常 */
void sleep_until(uint64_t deadline_us)
{
	struct timespec ts;

	ts.tv_sec = static_cast<time_t>(deadline_us / 1000000u);
	ts.tv_nsec = static_cast<long>((deadline_us % 1000000u) * 1000u);
	(void)clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
}
} // namespace

CanThread::~CanThread()
{
	stop();
}

bool CanThread::start(struct can_io *bus, QString *err)
{
	std::promise<bool> init_done;
	std::future<bool> init_result = init_done.get_future();

	if (bus == nullptr) {
		if (err != nullptr) {
			*err = QStringLiteral("CAN 通道为空");
		}
		return false;
	}
	if (thread_.joinable()) {
		if (err != nullptr) {
			*err = QStringLiteral("控制线程已经在跑");
		}
		return false;
	}

	bus_ = bus;
	stop_.store(false, std::memory_order_relaxed);
	thread_ = std::thread(
		[this, done = std::move(init_done)]() mutable { run(std::move(done)); });

	if (init_result.get()) {
		/* 宿主职责：服务线程跑会阻塞的 402 流程 */
		svc_thread_ = std::thread([this]() { run_svc(); });
		return true;
	}

	stop(); /* 初始化失败：把刚起的线程收回来 */
	if (err != nullptr) {
		*err = QStringLiteral("app 初始化失败");
	}
	return false;
}

void CanThread::stop()
{
	stop_.store(true, std::memory_order_relaxed);
	if (svc_thread_.joinable()) {
		svc_thread_.join();
	}
	if (thread_.joinable()) {
		thread_.join(); /* 线程最多卡在 clock_nanosleep 里一拍 */
	}
}

CanThread::Stats CanThread::stats() const
{
	return {ticks_.load(std::memory_order_relaxed), max_gap_us_.load(std::memory_order_relaxed),
		late_ticks_.load(std::memory_order_relaxed)};
}

/* ---- 服务线程本体 ---- */

void CanThread::run_svc()
{
	while (!stop_.load(std::memory_order_relaxed)) {
		cats_control_svc_poll(); /* 会阻塞：最长一个 SDO 超时 */
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

/* ---- 控制线程本体 ---- */

void CanThread::run(std::promise<bool> init_done)
{
	/* 建主站：站号 1 / 2 只在这里声明一次；app 从 node_count/node_id[] 读回轴 */
	master_init(&master_, bus_);
	(void)master_add_node(&master_, 1);
	(void)master_add_node(&master_, 2);

	const bool ok = cats_control_init(&master_) == CATS_OK;
	init_done.set_value(ok);
	if (!ok) {
		return;
	}

	uint64_t next = monotonic_us();
	uint64_t prev = next;

	while (!stop_.load(std::memory_order_relaxed)) {
		next += kPeriodUs; /* 固定基准 + 固定步长：晚了会自己追，不会越跑越偏 */

		const uint64_t t0 = monotonic_us();
		const uint32_t gap = static_cast<uint32_t>(t0 - prev);
		prev = t0;

		if (gap > max_gap_us_.load(std::memory_order_relaxed)) {
			max_gap_us_.store(gap, std::memory_order_relaxed);
		}
		if (gap > kLateThresholdUs) {
			late_ticks_.fetch_add(1u, std::memory_order_relaxed);
		}

		(void)cats_control_poll(); /* 收帧 + 推进状态机；非阻塞 */
		ticks_.fetch_add(1u, std::memory_order_relaxed);

		sleep_until(next);
	}
}
