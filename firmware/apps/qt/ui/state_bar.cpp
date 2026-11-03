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
	const char *code;
	const char *title;
	const char *hint;
	StatusLed::State led;
	int dot_size;
};

/* 顺序必须与 app 的 cats_run_state 一致 */
const StateInfo kStates[] = {
	{"PREPARING", "准备中", "正在复位节点并等待 boot-up", StatusLed::State::Busy, 12},
	{"RUNNING", "运行中", "节点已使能", StatusLed::State::Ok, 12},
	{"STOPPING", "停止中", "正在退出运行", StatusLed::State::Busy, 12},
	{"FAULT", "故障", "需复位后重新启动", StatusLed::State::Fault, 12},
	{"ESTOP", "急停", "需复位后重新启动", StatusLed::State::Fault, 12},
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

	led_ = new StatusLed(QStringLiteral("PREPARING"), this);
	led_->setObjectName(QStringLiteral("stateLed"));
	led_->setDotSize(kStates[0].dot_size);

	hint_ = new QLabel(this);
	hint_->setObjectName(QStringLiteral("stateHint"));

	current->addWidget(led_);
	current->addWidget(hint_);
	root->addLayout(current);
	root->addStretch(1);

	setState(State::Preparing);
}

void StateBar::setState(State state)
{
	const int index = static_cast<int>(state);
	const StateInfo &info = kStates[index];

	led_->setState(info.led);
	led_->setDotSize(info.dot_size);
	led_->setText(QString::fromLatin1(info.code));
	hint_->setText(QStringLiteral("%1 · %2").arg(QString::fromUtf8(info.title),
						     QString::fromUtf8(info.hint)));
}
