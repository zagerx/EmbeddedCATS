#include "state_bar.h"

#include "theme/theme.h"
#include "ui/status_led.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace
{
struct StateInfo {
	StatusLed::State led;
	int dot_size;
};

/* 1.1.2（故障码）的固定高度：一行标签 + 数值，上下各 8 px 内边距 */
constexpr int kFaultAreaHeight = 56;

/* 粗粒度档位表（顺序与 StateBar::State 一致）；文字只显示控制层上报的数值 */
const StateInfo kStates[] = {
	{StatusLed::State::Busy, 12},  {StatusLed::State::Idle, 12}, {StatusLed::State::Busy, 12},
	{StatusLed::State::Ok, 12},    {StatusLed::State::Busy, 12}, {StatusLed::State::Fault, 12},
	{StatusLed::State::Fault, 12},
};
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
	auto *fault_layout = new QHBoxLayout(fault_area);
	fault_layout->setContentsMargins(16, 8, 16, 8);
	fault_layout->setSpacing(12);

	fault_label_ = new QLabel(QStringLiteral("故障码"), fault_area);
	fault_label_->setObjectName(QStringLiteral("faultLabel"));
	fault_value_ = new QLabel(QStringLiteral("0"), fault_area);
	fault_value_->setObjectName(QStringLiteral("faultValue"));
	fault_layout->addWidget(fault_label_, 0, Qt::AlignVCenter);
	fault_layout->addWidget(fault_value_, 0, Qt::AlignVCenter);
	fault_layout->addStretch(1);
	root->addWidget(fault_area);
}

void StateBar::setState(State state, int code)
{
	const StateInfo &info = kStates[static_cast<int>(state)];

	led_->setState(info.led);
	led_->setDotSize(info.dot_size);
	led_->setText(QString::number(code)); /* 直接显示控制层上报的状态值 */
}

void StateBar::setDetail(int fault)
{
	fault_value_->setText(QString::number(fault)); /* 0 = 无故障 */
}
