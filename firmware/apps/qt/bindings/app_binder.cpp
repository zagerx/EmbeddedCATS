/*
 * app_binder.cpp — 界面与 app 的唯一接缝（实现）
 *
 * 这里没有业务逻辑：不判故障、不改状态、不认 COB-ID，
 * 只是把「app 状态」变成「控件刷新」，把「app 日志」送到面板。
 */

#include "app_binder.h"

#include "ui/axis_card.h"
#include "ui/log_panel.h"
#include "ui/state_bar.h"

#include <QLatin1Char>
#include <QString>
#include <QTimer>

namespace
{
/* 界面刷新：50 ms 对人眼足够。控制节拍是 1 ms，那个在 CanThread 里，别混 */
constexpr int kRefreshIntervalMs = 50;
/* 节拍统计：每 5 s 往日志写一行，用来判断节拍稳不稳 */
constexpr int kStatsIntervalMs = 5000;

/* 状态字显示成 0x001F 这种定宽十六进制 */
QString hex16(uint16_t v)
{
	return QStringLiteral("0x") +
	       QString::number(v, 16).rightJustified(4, QLatin1Char('0')).toUpper();
}

/* app 的运行状态 → 状态条档位（只决定颜色与说明，精确状态名另外传入） */
StateBar::State to_bar_state(enum cats_state s)
{
	switch (s) {
	case CATS_STATE_PREPARING:
		return StateBar::State::Preparing;
	/* CONFIGURING（写参数）与 ENABLING（使能）在界面上合成一档“正在准备驱动器” */
	case CATS_STATE_CONFIGURING:
	case CATS_STATE_ENABLING:
		return StateBar::State::Enabling;
	/* HOLDING 与 MOVING 在界面上合成一档“已使能” */
	case CATS_STATE_HOLDING:
	case CATS_STATE_MOVING:
		return StateBar::State::Running;
	case CATS_STATE_HALTING:
		return StateBar::State::Halting;
	case CATS_STATE_FAULT:
		return StateBar::State::Fault;
	case CATS_STATE_ESTOP:
		return StateBar::State::EStop;
	case CATS_STATE_COUNT:
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
	axis_cards_[0] = root->findChild<AxisCard *>(QStringLiteral("axisCard1"));
	axis_cards_[1] = root->findChild<AxisCard *>(QStringLiteral("axisCard2"));

	if (state_bar_ == nullptr) {
		pushLog(QStringLiteral("绑定：找不到状态条，状态不会刷新"));
	}
	if (axis_cards_[0] == nullptr || axis_cards_[1] == nullptr) {
		pushLog(QStringLiteral("绑定：找不到轴卡片，实时反馈不会刷新"));
	}
}

bool AppBinder::openChannel(const QString &ifname)
{
	if (socketcan_open(&bus_, &dev_, ifname.toUtf8().constData()) != CAN_IO_OK) {
		pushLog(QStringLiteral("打不开通道 %1（接口是否 up？）").arg(ifname));
		return false;
	}
	channel_open_ = true;
	/* 建主站与 cats_control_init 放到控制线程里做（见 CanThread）：它们会发 NMT 复位，
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
	updateAxisCards();
}

void AppBinder::updateAxisCards()
{
	const enum cats_fault fault = cats_control_fault_code();

	for (uint8_t i = 0; i < 2; i++) {
		AxisCard *card = axis_cards_[i];
		struct cats_axis_sample sample;

		if (card == nullptr) {
			continue;
		}
		if (cats_control_axis_sample(i, &sample) != CATS_OK) {
			continue;
		}

		card->setPosition(
			(sample.valid & CATS_SAMPLE_VALID_POSITION) != 0U
				? QString::number(static_cast<double>(sample.position_mm), 'f', 3)
				: QStringLiteral("—"));
		card->setVelocity(
			(sample.valid & CATS_SAMPLE_VALID_VELOCITY) != 0U
				? QString::number(static_cast<double>(sample.velocity_mm_s), 'f', 3)
				: QStringLiteral("—"));
		card->setStatusword((sample.valid & CATS_SAMPLE_VALID_STATUSWORD) != 0U
					    ? hex16(sample.statusword)
					    : QStringLiteral("—"));

		const uint32_t abort = cats_control_axis_abort(i);
		if (abort != 0U) {
			card->setError(
				QStringLiteral("abort 0x%1").arg(abort, 8, 16, QLatin1Char('0')));
		} else if (fault != CATS_FAULT_NONE) {
			card->setError(QString::fromUtf8(cats_control_fault_name(fault)));
		} else {
			card->setError(QStringLiteral("—"));
		}
	}
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
	const enum cats_state state = cats_control_state();

	if (state == last_state_) {
		return; /* 状态没变就不碰控件 */
	}
	last_state_ = state;

	if (state_bar_ != nullptr) {
		state_bar_->setState(to_bar_state(state),
				     QString::fromUtf8(cats_control_state_name(state)));
	}
	pushLog(QStringLiteral("状态 → %1").arg(QString::fromUtf8(cats_control_state_name(state))));
}
