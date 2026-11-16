#include "state_bar.h"

#include "theme/theme.h"
#include "ui/status_led.h"

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QString>
#include <QVBoxLayout>

namespace
{
struct StateInfo {
	StatusLed::State led;
	int dot_size;
};

/* 1.1.2（故障四项）的固定高度：标题 + 两行字段，上下各 8 px 内边距 */
constexpr int kFaultAreaHeight = 88;

/* 粗粒度档位表（顺序与 StateBar::State 一致）；文字只显示控制层上报的数值 */
const StateInfo kStates[] = {
	{StatusLed::State::Busy, 12},  {StatusLed::State::Idle, 12}, {StatusLed::State::Busy, 12},
	{StatusLed::State::Ok, 12},    {StatusLed::State::Busy, 12}, {StatusLed::State::Fault, 12},
	{StatusLed::State::Fault, 12},
};

/* 1.1.2 的字段标签（界面自己的静态文字，不来自控制层） */
QLabel *make_tag(const QString &text, QWidget *parent)
{
	auto *label = new QLabel(text, parent);

	label->setObjectName(QStringLiteral("faultLabel"));
	return label;
}

/* 1.1.2 的字段值：初始为 0，宽度固定避免数字变化时抖动 */
QLabel *make_value(QWidget *parent)
{
	auto *label = new QLabel(QStringLiteral("0"), parent);

	label->setObjectName(QStringLiteral("faultValue"));
	label->setMinimumWidth(56);
	return label;
}
} // namespace

StateBar::StateBar(QWidget *parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("stateBar"));

	/* 纵向分两块：1.1.1 主站状态（占满剩余高度）/ 横线 / 1.1.2 故障码 */
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(0);

	auto *status_area = new QWidget(this);
	status_area->setObjectName(QStringLiteral("stateMain"));
	auto *status_layout = new QHBoxLayout(status_area);
	status_layout->setContentsMargins(16, 10, 16, 10);
	status_layout->setSpacing(12);

	/* 状态值来自控制层，未接入前先显示占位符 */
	led_ = new StatusLed(QStringLiteral("—"), status_area);
	led_->setObjectName(QStringLiteral("stateLed"));
	led_->setDotSize(kStates[0].dot_size);
	status_layout->addWidget(led_, 0, Qt::AlignVCenter);
	status_layout->addStretch(1);
	root->addWidget(status_area, 1);

	auto *divider = new QFrame(this);
	divider->setObjectName(QStringLiteral("divider"));
	divider->setFrameShape(QFrame::NoFrame);
	divider->setFixedHeight(1);
	divider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	root->addWidget(divider);

	auto *fault_area = new QWidget(this);
	fault_area->setObjectName(QStringLiteral("stateFault"));
	fault_area->setFixedHeight(kFaultAreaHeight);
	auto *fault_layout = new QGridLayout(fault_area);
	fault_layout->setContentsMargins(16, 8, 16, 8);
	fault_layout->setHorizontalSpacing(8);
	fault_layout->setVerticalSpacing(4);

	auto *fault_title = new QLabel(QStringLiteral("故障"), fault_area);
	fault_title->setObjectName(QStringLiteral("faultTitle"));
	fault_layout->addWidget(fault_title, 0, 0, 1, 4);

	/* 第一行：阶段 / 原因 */
	fault_layout->addWidget(make_tag(QStringLiteral("阶段"), fault_area), 1, 0);
	stage_value_ = make_value(fault_area);
	fault_layout->addWidget(stage_value_, 1, 1);
	fault_layout->addWidget(make_tag(QStringLiteral("原因"), fault_area), 1, 2);
	cause_value_ = make_value(fault_area);
	fault_layout->addWidget(cause_value_, 1, 3);

	/* 第二行：轴位图 / abort 码 */
	fault_layout->addWidget(make_tag(QStringLiteral("轴"), fault_area), 2, 0);
	axis_value_ = make_value(fault_area);
	fault_layout->addWidget(axis_value_, 2, 1);
	fault_layout->addWidget(make_tag(QStringLiteral("码"), fault_area), 2, 2);
	abort_value_ = make_value(fault_area);
	fault_layout->addWidget(abort_value_, 2, 3);

	fault_layout->setColumnStretch(4, 1); /* 右侧留白，不把字段拉伸 */
	root->addWidget(fault_area);
}

void StateBar::setState(State state, int code)
{
	const StateInfo &info = kStates[static_cast<int>(state)];

	led_->setState(info.led);
	led_->setDotSize(info.dot_size);
	led_->setText(QString::number(code)); /* 直接显示控制层上报的状态值 */
}

void StateBar::setFault(int stage, int cause, int axis_mask, uint32_t abort_code)
{
	/* 全部数字直出：十六进制字段保留固定宽度，方便对位 */
	stage_value_->setText(QString::number(stage));
	cause_value_->setText(QString::number(cause));
	axis_value_->setText(QStringLiteral("0x") +
			     QString::number(static_cast<uint>(axis_mask), 16)
				     .toUpper()
				     .rightJustified(2, QLatin1Char('0')));
	abort_value_->setText(
		QStringLiteral("0x") +
		QString::number(abort_code, 16).toUpper().rightJustified(8, QLatin1Char('0')));
}
