/*
 * state_bar.cpp — 纯展示：当前状态圆点 + 状态码 + 中文说明
 */

#include "state_bar.h"

#include "theme/theme.h"
#include "ui/status_led.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace
{
struct StateInfo {
	const char *title;
	const char *hint;
	StatusLed::State led;
	int dot_size;
};

/* 粗粒度档位表（顺序与 StateBar::State 一致）；状态码本身由外部传入 */
const StateInfo kStates[] = {
	{"准备中", "正在复位节点 / 写参数 / 使能", StatusLed::State::Busy, 12},
	{"使能中", "正在让驱动器进入 PP 并使能", StatusLed::State::Busy, 12},
	{"已使能", "保持位置，可接受位置指令", StatusLed::State::Ok, 12},
	{"减速停", "正在减速停在当前位置", StatusLed::State::Busy, 12},
	{"故障", "已停住，需重启程序", StatusLed::State::Fault, 12},
	{"急停", "已断使能，需重启程序", StatusLed::State::Fault, 12},
};
} // namespace

StateBar::StateBar(QWidget *parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("stateBar"));

	auto *root = new QHBoxLayout(this);
	root->setContentsMargins(16, 10, 16, 10);
	root->setSpacing(16);

	/* 左：灯 + 大号状态码，下面一行中文说明 */
	auto *current = new QVBoxLayout();
	current->setSpacing(2);

	/* 状态码来自控制层，未接入前先显示占位符 */
	led_ = new StatusLed(QStringLiteral("—"), this);
	led_->setObjectName(QStringLiteral("stateLed"));
	led_->setDotSize(kStates[0].dot_size);

	hint_ = new QLabel(QStringLiteral("等待控制层状态"), this);
	hint_->setObjectName(QStringLiteral("stateHint"));

	current->addWidget(led_);
	current->addWidget(hint_);
	root->addLayout(current);
	root->addStretch(1);
}

void StateBar::setState(State state, const QString &code)
{
	const int index = static_cast<int>(state);
	const StateInfo &info = kStates[index];

	led_->setState(info.led);
	led_->setDotSize(info.dot_size);
	led_->setText(code);
	hint_->setText(QStringLiteral("%1 · %2").arg(QString::fromUtf8(info.title),
						     QString::fromUtf8(info.hint)));
}
