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
constexpr int kPollIntervalMs = 20; /* 节拍：判 750 ms 心跳超时只引入 2.7% 误差 */

/* app 的运行状态 → 顶部状态条。两边枚举刻意一一对应，仍显式写全，免得日后错位 */
StateBar::State to_bar_state(enum cats_run_state s)
{
	switch (s) {
	case CATS_RUN_PREPARING:
		return StateBar::State::Preparing;
	case CATS_RUN_RUNNING:
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
	if (channel_open_) {
		can_io_close(&bus_);
	}
}

void AppBinder::attach(QWidget *root, const QString &ifname)
{
	findWidgets(root);
	if (!openChannel(ifname)) {
		return; /* 没通道就不起节拍：onTick 根本不会被调用 */
	}
	startTimer();
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

	co_master_init(&master_, &bus_);
	(void)co_master_add_node(&master_, 1);
	(void)co_master_add_node(&master_, 2);

	if (cats_app_init(&master_) != CATS_APP_OK) {
		pushLog(QStringLiteral("app 初始化失败"));
		return false;
	}
	return true;
}

void AppBinder::startTimer()
{
	timer_ = new QTimer(this);
	timer_->setTimerType(Qt::PreciseTimer);
	timer_->setInterval(kPollIntervalMs);
	connect(timer_, &QTimer::timeout, this, &AppBinder::onTick);
	timer_->start();
}

void AppBinder::onTick()
{
	/* app 内部先收帧再推进状态机；非阻塞，20 ms 一次不会卡界面 */
	(void)cats_app_poll();
	publish();
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
}
