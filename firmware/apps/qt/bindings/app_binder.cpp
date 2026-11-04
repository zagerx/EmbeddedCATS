/*
 * app_binder.cpp — 界面与 app 的唯一接缝（实现）
 *
 * 这里没有业务逻辑：不判故障、不改状态、不认 COB-ID，
 * 只是把「app 状态」变成「控件刷新」，把「app 日志」送到面板。
 */

#include "app_binder.h"

#include "ui/log_panel.h"
#include "ui/state_bar.h"

#include <QTimer>

namespace
{
/* 界面刷新：50 ms 对人眼足够。控制节拍是 1 ms，那个在 CanThread 里，别混 */
constexpr int kRefreshIntervalMs = 50;
/* 节拍统计：每 5 s 往日志写一行，用来判断节拍稳不稳 */
constexpr int kStatsIntervalMs = 5000;

/* app 的运行状态 → 顶部状态条。两边枚举刻意一一对应，仍显式写全，免得日后错位 */
StateBar::State to_bar_state(enum cats_run_state s)
{
	switch (s) {
	case CATS_RUN_PREPARING:
		return StateBar::State::Preparing;
	case CATS_RUN_ENABLING:
		return StateBar::State::Enabling;
	/* READY 与 MOVING 在界面上合成一档“运行中”：状态条只给粗粒度提示，
	 * 精确状态看 cats_app_state_name() 的文本（日志/终端）。 */
	case CATS_RUN_READY:
	case CATS_RUN_MOVING:
		return StateBar::State::Running;
	case CATS_RUN_STOPPING:
		return StateBar::State::Stopping;
	case CATS_RUN_FAULT:
		return StateBar::State::Fault;
	case CATS_RUN_ESTOP:
		return StateBar::State::EStop;
	case CATS_RUN_STATE_COUNT:
		break;
	}
	return StateBar::State::Preparing;
}
} // namespace

AppBinder::AppBinder(QObject *parent) : QObject(parent)
{
}

AppBinder::~AppBinder()
{
	worker_.stop(); /* 先停控制线程：之后没人再碰总线 */
	if (channel_open_) {
		can_io_close(&bus_);
	}
}

void AppBinder::attach(QWidget *root, const QString &ifname)
{
	findWidgets(root);
	if (!openChannel(ifname)) {
		return; /* 没通道就不起节拍 */
	}

	QString err;
	if (!worker_.start(&bus_, &err)) {
		pushLog(QStringLiteral("控制线程启动失败：%1").arg(err));
		return;
	}

	startTimers();
	pushLog(QStringLiteral("控制线程已启动：1 ms 节拍"));
	publish();
}

void AppBinder::findWidgets(QWidget *root)
{
	log_panel_ = root->findChild<LogPanel *>();
	state_bar_ = root->findChild<StateBar *>();

	if (state_bar_ == nullptr) {
		pushLog(QStringLiteral("绑定：找不到状态条，状态不会刷新"));
	}
}

bool AppBinder::openChannel(const QString &ifname)
{
	if (socketcan_open(&bus_, &dev_, ifname.toUtf8().constData()) != CAN_IO_OK) {
		pushLog(QStringLiteral("打不开通道 %1（接口是否 up？）").arg(ifname));
		return false;
	}
	channel_open_ = true;
	/* 建主站与 cats_app_init 放到控制线程里做（见 CanThread）：它们会发 NMT 复位，
	 * 而且之后所有状态都在那个线程里改 */
	return true;
}

void AppBinder::startTimers()
{
	refresh_ = new QTimer(this);
	refresh_->setInterval(kRefreshIntervalMs);
	connect(refresh_, &QTimer::timeout, this, &AppBinder::onRefresh);
	refresh_->start();

	stats_ = new QTimer(this);
	stats_->setInterval(kStatsIntervalMs);
	connect(stats_, &QTimer::timeout, this, &AppBinder::onStats);
	stats_->start();
}

void AppBinder::onRefresh()
{
	publish();
}

void AppBinder::onStats()
{
	const CanThread::Stats s = worker_.stats();

	pushLog(QStringLiteral("节拍 %1 拍，最大间隔 %2 µs，掉拍 %3 次")
			.arg(s.ticks)
			.arg(s.max_gap_us)
			.arg(s.late_ticks));
}

void AppBinder::pushLog(const QString &line)
{
	if (log_panel_ != nullptr) {
		log_panel_->append(line);
	}
}

void AppBinder::publish()
{
	const enum cats_run_state state = cats_app_state();

	if (state == last_state_) {
		return; /* 状态没变就不碰控件 */
	}
	last_state_ = state;

	if (state_bar_ != nullptr) {
		state_bar_->setState(to_bar_state(state));
	}
	pushLog(QStringLiteral("状态 → %1").arg(QString::fromUtf8(cats_app_state_name(state))));
}
