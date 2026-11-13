/*
 * 单个 CANopen 工作线程执行 epoll 事件循环。
 * promise 返回初始化结果，停止时先唤醒等待，再 join 和释放协议资源。
 */

#include "can_thread.h"

#include <control/control.h>

#include <QByteArray>
#include <time.h>
#include <utility>

namespace
{
constexpr uint32_t kLateThresholdUs = 2000u;

uint64_t monotonic_us()
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<uint64_t>(ts.tv_sec) * 1000000u +
	       static_cast<uint64_t>(ts.tv_nsec) / 1000u;
}
} // namespace

CanThread::~CanThread()
{
	stop();
}

bool CanThread::start(const QString &ifname, QString *err)
{
	std::promise<bool> init_done;
	std::future<bool> init_result = init_done.get_future();

	if (ifname.isEmpty()) {
		if (err != nullptr) {
			*err = QStringLiteral("CAN 接口名为空");
		}
		return false;
	}
	if (thread_.joinable()) {
		if (err != nullptr) {
			*err = QStringLiteral("CANopen 工作线程已经在跑");
		}
		return false;
	}
	ifname_ = ifname;
	stop_.store(false, std::memory_order_relaxed);
	thread_ = std::thread(
		[this, done = std::move(init_done)]() mutable { run(std::move(done)); });
	if (init_result.get()) {
		return true;
	}
	stop();
	if (err != nullptr) {
		*err = QStringLiteral("CANopen 初始化失败（接口是否存在且已 up？）");
	}
	return false;
}

void CanThread::stop()
{
	stop_.store(true, std::memory_order_relaxed);
	controller_wakeup();
	if (thread_.joinable()) {
		thread_.join();
	}
	(void)controller_deinit();
}

CanThread::Stats CanThread::stats() const
{
	return {ticks_.load(std::memory_order_relaxed), max_gap_us_.load(std::memory_order_relaxed),
		late_ticks_.load(std::memory_order_relaxed)};
}

void CanThread::run(std::promise<bool> init_done)
{
	const QByteArray ifname = ifname_.toUtf8();
	const bool ok = controller_init(ifname.constData()) == CONTROLLER_OK;
	uint64_t prev = monotonic_us();

	init_done.set_value(ok);
	if (!ok) {
		return;
	}
	while (!stop_.load(std::memory_order_relaxed)) {
		const uint64_t t0 = monotonic_us();
		const uint32_t gap = static_cast<uint32_t>(t0 - prev);

		prev = t0;
		if (gap > max_gap_us_.load(std::memory_order_relaxed)) {
			max_gap_us_.store(gap, std::memory_order_relaxed);
		}
		if (gap > kLateThresholdUs) {
			late_ticks_.fetch_add(1u, std::memory_order_relaxed);
		}
		if (controller_event_poll() != CONTROLLER_OK) {
			break;
		}
		ticks_.fetch_add(1u, std::memory_order_relaxed);
	}
}
