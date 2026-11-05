/*
 * axis_card.cpp — 纯展示：标题 + 状态灯 + 命令组 + 反馈组
 */

#include "axis_card.h"

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

	/* 标题行：只有轴名 */
	auto *header = new QHBoxLayout();
	title_ = new QLabel(title, this);
	title_->setObjectName(QStringLiteral("axisTitle"));
	header->addWidget(title_);
	header->addStretch(1);
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
	target_position_->setValue(0.0);

	target_velocity_ = new QDoubleSpinBox(group);
	target_velocity_->setObjectName(QStringLiteral("targetVelocity"));
	target_velocity_->setDecimals(3);
	target_velocity_->setRange(0.0, 10000.0);
	target_velocity_->setSingleStep(1.0);
	target_velocity_->setValue(0.0);

	target_acceleration_ = new QDoubleSpinBox(group);
	target_acceleration_->setObjectName(QStringLiteral("targetAcceleration"));
	target_acceleration_->setDecimals(3);
	target_acceleration_->setRange(0.0, 100000.0);
	target_acceleration_->setSingleStep(10.0);
	target_acceleration_->setValue(0.0);

	/* 单位放在框外，括号围起来 */
	form->addRow(QStringLiteral("目标位置"),
		     make_command_row(target_position_, QStringLiteral("(mm)")));
	form->addRow(QStringLiteral("目标速度"),
		     make_command_row(target_velocity_, QStringLiteral("(mm/s)")));
	form->addRow(QStringLiteral("目标加速度"),
		     make_command_row(target_acceleration_, QStringLiteral("(mm/s²)")));
	return group;
}

QWidget *AxisCard::make_command_row(QDoubleSpinBox *spin, const QString &unit)
{
	auto *row = new QWidget(this);
	auto *layout = new QHBoxLayout(row);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(6);

	layout->addWidget(spin, 1);

	auto *unit_label = new QLabel(unit, row);
	unit_label->setObjectName(QStringLiteral("unitLabel"));
	layout->addWidget(unit_label, 0, Qt::AlignVCenter);

	return row;
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
