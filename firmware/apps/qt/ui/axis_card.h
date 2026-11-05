/*
 * axis_card.h — 单轴卡片（轴1 / 轴2 各建一个，内容完全一致）
 *
 * 上半部分「命令」：目标位置 / 目标速度 / 目标加速度（可输入，但没有提交按钮）；
 * 下半部分「反馈」：实际位置 / 实际速度 / 状态字 / 错误码（纯显示，由绑定层填）。
 */

#ifndef AXIS_CARD_H
#define AXIS_CARD_H

#include <QFrame>

class QDoubleSpinBox;
class QGroupBox;
class QLabel;

class AxisCard : public QFrame
{
	Q_OBJECT

      public:
	explicit AxisCard(const QString &title, QWidget *parent = nullptr);

	/* 纯显示接口：绑定层把样本格式化成字符串后填进来 */
	void setPosition(const QString &text);
	void setVelocity(const QString &text);
	void setStatusword(const QString &text);
	void setError(const QString &text);
	QGroupBox *build_command_group();
	QGroupBox *build_feedback_group();
	QWidget *make_command_row(QDoubleSpinBox *spin, const QString &unit);
	QLabel *make_value(QWidget *parent);

	QLabel *title_ = nullptr;
	QDoubleSpinBox *target_position_ = nullptr;
	QDoubleSpinBox *target_velocity_ = nullptr;
	QDoubleSpinBox *target_acceleration_ = nullptr;
	QLabel *position_value_ = nullptr;
	QLabel *velocity_value_ = nullptr;
	QLabel *status_word_value_ = nullptr;
	QLabel *error_code_value_ = nullptr;
};

#endif /* AXIS_CARD_H */
