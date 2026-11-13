/* Qt 界面与控制层的绑定实现。 */

#include "app_binder.h"

#include "ui/axis_card.h"
#include "ui/log_panel.h"
#include "ui/state_bar.h"

#include <QLatin1Char>
#include <QComboBox>
#include <QPushButton>
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

/* app 的运行状态 → 状态条档位（只决定颜色与说明，精确状态名另外传入） */ StateBar::State
to_bar_state(enum controller_state s)
{
	switch (s) {
	case CONTROLLER_STATE_PREPARING:
		return StateBar::State::Preparing;
	/* CONFIGURING（写参数）与 ENABLING（使能）在界面上合成一档“正在准备驱动器” */
	case CONTROLLER_STATE_CONFIGURING:
	case CONTROLLER_STATE_ENABLING:
		return StateBar::State::Enabling;
	/* HOLDING 与 MOVING 在界面上合成一档“已使能” */
	case CONTROLLER_STATE_HOLDING:
	case CONTROLLER_STATE_MOVING:
		return StateBar::State::Running;
	case CONTROLLER_STATE_HALTING:
		return StateBar::State::Halting;
	case CONTROLLER_STATE_FAULT:
		return StateBar::State::Fault;
	case CONTROLLER_STATE_ESTOP:
		return StateBar::State::EStop;
	case CONTROLLER_STATE_COUNT:
		break;
	}
	return StateBar::State::Preparing;
}

/* 指令受理结果 → 日志里的说法 */
QString result_text(enum controller_result r)
{
	switch (r) {
	case CONTROLLER_OK:
		return QStringLiteral("已入队");
	case CONTROLLER_ERR_BUSY:
		return QStringLiteral("命令队列满，稍后重试");
	case CONTROLLER_ERR_PARAM:
		return QStringLiteral("当前状态不接受位置指令或参数无效");
	case CONTROLLER_ERR_STATE:
		return QStringLiteral("控制层未初始化");
	}
	return QStringLiteral("未知结果");
}
} // namespace

AppBinder::AppBinder(QObject *parent) : QObject(parent)
{
}

AppBinder::~AppBinder()
{
	worker_.stop(); /* 先停控制线程：之后没人再碰总线 */
	if (channel_open_) {
		controller_socketcan_close(fd_);
	}
}

void AppBinder::attach(QWidget *root, const QString &ifname)
{
	findWidgets(root);
	bindCommands(); /* 按钮先接上：通道打不开时也能看到拒绝理由 */
	if (!openChannel(ifname)) {
		return; /* 没通道就不起节拍 */
	}

	QString err;
	if (!worker_.start(fd_, &err)) {
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

	axis_selector_ = root->findChild<QComboBox *>(QStringLiteral("axisSelector"));
	btn_move_ = root->findChild<QPushButton *>(QStringLiteral("btn_move"));
	btn_halt_ = root->findChild<QPushButton *>(QStringLiteral("btn_halt"));
	btn_estop_ = root->findChild<QPushButton *>(QStringLiteral("btn_estop"));
}

void AppBinder::bindCommands()
{
	if (btn_move_ == nullptr || btn_halt_ == nullptr || btn_estop_ == nullptr) {
		pushLog(QStringLiteral("绑定：找不到 2.1 的按钮，指令无法下发"));
		return;
	}

	connect(btn_move_, &QPushButton::clicked, this, &AppBinder::onMove);
	connect(btn_halt_, &QPushButton::clicked, this, &AppBinder::onHalt);
	connect(btn_estop_, &QPushButton::clicked, this, &AppBinder::onEStop);
	updateCommands(controller_state());
}

void AppBinder::updateCommands(enum controller_state s)
{
	if (btn_move_ != nullptr) {
		/* 只有 HOLDING 接受位置指令；其余状态点下去只会被丢弃 */
		btn_move_->setEnabled(s == CONTROLLER_STATE_HOLDING);
	}
	if (btn_halt_ != nullptr) {
		btn_halt_->setEnabled(s == CONTROLLER_STATE_HOLDING ||
				      s == CONTROLLER_STATE_MOVING);
	}
	if (btn_estop_ != nullptr) {
		btn_estop_->setEnabled(s != CONTROLLER_STATE_ESTOP);
	}
}

void AppBinder::onMove()
{
	/* 选择器决定下发到哪一轴；「双轴」= 两条指令放进同一个请求，一起启动 */
	const int choice = (axis_selector_ == nullptr) ? 2 : axis_selector_->currentIndex();
	struct controller_move_req reqs[2];
	uint8_t count = 0u;

	if (choice == 0 || choice == 2) {
		if (fillMove(0U, &reqs[count])) {
			count++;
		}
	}
	if (choice == 1 || choice == 2) {
		if (fillMove(1U, &reqs[count])) {
			count++;
		}
	}
	if (count == 0u) {
		return; /* 全被拦下：fillMove 已经写过原因 */
	}

	const enum controller_result r = controller_move(reqs, count);

	if (r == CONTROLLER_OK) {
		for (uint8_t i = 0u; i < count; i++) {
			pushLog(QStringLiteral("axis%1 运动 → %2 mm / %3 mm/s / %4 mm/s²")
					.arg(reqs[i].axis + 1)
					.arg(static_cast<double>(reqs[i].mm), 0, 'f', 3)
					.arg(static_cast<double>(reqs[i].mm_s), 0, 'f', 3)
					.arg(static_cast<double>(reqs[i].mm_s2), 0, 'f', 3));
		}
	} else {
		pushLog(QStringLiteral("运动指令被拒：%1").arg(result_text(r)));
	}
}

bool AppBinder::fillMove(uint8_t axis, struct controller_move_req *out)
{
	AxisCard *card = axis_cards_[axis];

	if (card == nullptr || out == nullptr) {
		return false;
	}
	out->axis = axis;
	out->mm = static_cast<float>(card->targetPosition());
	out->mm_s = static_cast<float>(card->targetVelocity());
	out->mm_s2 = static_cast<float>(card->targetAcceleration());

	/* 速度 / 加速度为 0 的指令等于原地不动，到驱动器那里可能报错，入口拦住 */
	if (out->mm_s <= 0.0f || out->mm_s2 <= 0.0f) {
		pushLog(QStringLiteral("axis%1 未下发：速度和加速度必须大于 0").arg(axis + 1));
		return false;
	}
	return true;
}

void AppBinder::onHalt()
{
	controller_stop();
	pushLog(QStringLiteral("已请求暂停：减速停在当前位置"));
}

void AppBinder::onEStop()
{
	controller_estop();
	pushLog(QStringLiteral("已请求急停：所有轴 quick stop 并断使能"));
}

bool AppBinder::openChannel(const QString &ifname)
{
	fd_ = controller_socketcan_open(ifname.toUtf8().constData());
	if (fd_ < 0) {
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
	tickPrefill();
}

void AppBinder::tickPrefill()
{
	/* 首次进入 HOLDING：把目标位置填成当前实际位置。
	 * 否则框里默认 0，一点“运动”就命令驱动器冲回原点。只做一次，
	 * 之后不覆盖：人要重复执行同一条指令时，目标值得留着。 */
	bool all_ready = true;

	if (prefilled_ || controller_state() != CONTROLLER_STATE_HOLDING) {
		return;
	}
	for (uint8_t i = 0; i < 2; i++) {
		struct controller_axis_sample sample;

		if (axis_cards_[i] == nullptr) {
			continue;
		}
		/* 状态字为 0 说明这一轴还没读到过反馈：等下一拍再填，别填个假值 */
		if (controller_axis_sample(i, &sample) != CONTROLLER_OK ||
		    sample.statusword == 0U) {
			all_ready = false;
			continue;
		}
		axis_cards_[i]->setTargetPosition(static_cast<double>(sample.position_mm));
	}
	if (all_ready) {
		prefilled_ = true;
		pushLog(QStringLiteral("已就绪：目标位置已按当前实际位置填好"));
	}
}

void AppBinder::updateAxisCards()
{
	for (uint8_t i = 0; i < 2; i++) {
		AxisCard *card = axis_cards_[i];
		struct controller_axis_sample sample;

		if (card == nullptr) {
			continue;
		}
		if (controller_axis_sample(i, &sample) != CONTROLLER_OK) {
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
	const enum controller_state state = controller_state();
	const enum controller_fault fault = controller_fault_code();

	if (state == last_state_ && fault == last_fault_) {
		return; /* 状态和故障都没变就不碰控件 */
	}
	last_state_ = state;
	last_fault_ = fault;

	updateCommands(state);

	if (state_bar_ != nullptr) {
		state_bar_->setState(to_bar_state(state),
				     QString::fromUtf8(controller_state_name(state)));
		/* 整机故障原因跟在状态后面；不在 FAULT 时清掉 */
		state_bar_->setDetail(fault != CONTROLLER_FAULT_NONE
					      ? QString::fromUtf8(controller_fault_name(fault))
					      : QString());
	}
	pushLog(QStringLiteral("状态 → %1").arg(QString::fromUtf8(controller_state_name(state))));
}
