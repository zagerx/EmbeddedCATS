/*
 * axis_card.cpp — 纯展示：标题 + 状态灯 + 命令组 + 反馈组
 */

#include "axis_card.h"

#include "status_led.h"

#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

AxisCard::AxisCard(const QString &title, QWidget *parent) : QFrame(parent)
{
	setObjectName(QStringLiteral("axisCard"));

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(16, 10, 16, 12);
	root->setSpacing(8);

	/* 标题行：轴名 + 状态灯 */
	auto *header = new QHBoxLayout();
	title_ = new QLabel(title, this);
	title_->setObjectName(QStringLiteral("axisTitle"));
	status_ = new StatusLed(QStringLiteral("未连接"), this);
	header->addWidget(title_);
	header->addStretch(1);
	header->addWidget(status_);
	root->addLayout(header);

	root->addWidget(build_command_group());
	root->addWidget(build_feedback_group());
	root->addStretch(1);
}

QGroupBox *AxisCard::build_command_group()
{
	auto *group = new QGroupBox(QStringLiteral("命令"), this);

	auto *form = new QFormLayout(group);
	form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
	form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);

	target_position_ = new QDoubleSpinBox(group);
	target_position_->setObjectName(QStringLiteral("targetPosition"));
	target_position_->setDecimals(3);
	target_position_->setRange(-100000.0, 100000.0);
	target_position_->setSingleStep(0.5);
	target_position_->setSuffix(QStringLiteral(" mm"));
	target_position_->setValue(0.0);

	form->addRow(QStringLiteral("目标位置"), target_position_);
	return group;
}

QGroupBox *AxisCard::build_feedback_group()
{
	auto *group = new QGroupBox(QStringLiteral("反馈"), this);

	auto *form = new QFormLayout(group);
	form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
	form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);

	position_value_ = make_value(group);
	velocity_value_ = make_value(group);
	status_word_value_ = make_value(group);
	error_code_value_ = make_value(group);

	form->addRow(QStringLiteral("实际位置"), position_value_);
	form->addRow(QStringLiteral("实际速度"), velocity_value_);
	form->addRow(QStringLiteral("状态字"), status_word_value_);
	form->addRow(QStringLiteral("错误码"), error_code_value_);
	return group;
}

QLabel *AxisCard::make_value(QWidget *parent)
{
	auto *value = new QLabel(QStringLiteral("—"), parent);
	value->setObjectName(QStringLiteral("value"));
	value->setMinimumWidth(120);
	return value;
}
