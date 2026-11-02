/*
 * state_bar.cpp — 纯展示：当前状态 + 六态小标签
 */

#include "state_bar.h"

#include "theme/theme.h"
#include "ui/status_led.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QStyle>
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
	{"IDLE", "空闲", "等待启动", StatusLed::State::Off, 12},
	{"STARTING", "启动中", "正在连接节点并进入运行", StatusLed::State::Busy, 12},
	{"RUNNING", "运行中", "节点已使能", StatusLed::State::Ok, 12},
	{"STOPPING", "停止中", "正在退出运行", StatusLed::State::Busy, 12},
	{"FAULT", "故障", "需复位后重新启动", StatusLed::State::Fault, 12},
	{"ESTOP", "急停", "需复位后重新启动", StatusLed::State::Fault, 12},
};

constexpr int kStateCount = 6;
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

	led_ = new StatusLed(QStringLiteral("IDLE"), this);
	led_->setObjectName(QStringLiteral("stateLed"));
	led_->setDotSize(kStates[0].dot_size);

	hint_ = new QLabel(this);
	hint_->setObjectName(QStringLiteral("stateHint"));

	current->addWidget(led_);
	current->addWidget(hint_);
	root->addLayout(current);
	root->addStretch(1);

	/* 右：六个状态小标签 */
	auto *chips = new QHBoxLayout();
	chips->setSpacing(6);
	for (int i = 0; i < kStateCount; ++i) {
		auto *label = new QLabel(QString::fromLatin1(kStates[i].code), this);
		label->setObjectName(QStringLiteral("stateChip"));
		label->setAlignment(Qt::AlignCenter);
		chips->addWidget(label);
		chips_.append(Chip{static_cast<State>(i), label});
	}
	root->addLayout(chips);

	setState(State::Idle);
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

	for (const Chip &chip : chips_) {
		const bool active = (chip.state == state);
		chip.label->setProperty("active", active);
		chip.label->style()->unpolish(chip.label);
		chip.label->style()->polish(chip.label);
	}
}
