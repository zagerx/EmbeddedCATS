/* Qt 界面与控制层的绑定实现。 */

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
	/* 建主站与 controller_init 放到控制线程里做（见 CanThread）：它们会发 NMT 复位，
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
	for (uint8_t i = 0; i < 2; i++) {
		AxisCard *card = axis_cards_[i];
		struct cats_axis_sample sample;

		if (card == nullptr) {
			continue;
		}
		if (controller_axis_sample(i, &sample) != CATS_OK) {
			continue;
		}

		card->setPosition(QString::number(static_cast<double>(sample.position_mm), 'f', 3));
		card->setVelocity(
			QString::number(static_cast<double>(sample.velocity_mm_s), 'f', 3));
		card->setStatusword(hex16(sample.statusword));

		/* 错误码只有一来源：驱动器的 603F（主站的故障判断不往这里写） */
		card->setError(hex16(sample.error_code));

		/* SDO abort 是传输层失败原因，不属于驱动器错误码：只进日志 */
		const uint32_t abort = controller_axis_abort(i);

		if (abort != last_abort_[i]) {
			last_abort_[i] = abort;
			if (abort != 0U) {
				pushLog(QStringLiteral("axis%1 SDO abort 0x%2")
						.arg(i + 1)
						.arg(abort, 8, 16, QLatin1Char('0')));
			}
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
	const enum cats_state state = controller_state();
	const enum cats_fault fault = controller_fault_code();

	if (state == last_state_ && fault == last_fault_) {
		return; /* 状态和故障都没变就不碰控件 */
	}
	last_state_ = state;
	last_fault_ = fault;

	if (state_bar_ != nullptr) {
		state_bar_->setState(to_bar_state(state),
				     QString::fromUtf8(controller_state_name(state)));
		/* 整机故障原因跟在状态后面；不在 FAULT 时清掉 */
		state_bar_->setDetail(fault != CATS_FAULT_NONE
					      ? QString::fromUtf8(controller_fault_name(fault))
					      : QString());
	}
	pushLog(QStringLiteral("状态 → %1").arg(QString::fromUtf8(controller_state_name(state))));
}
