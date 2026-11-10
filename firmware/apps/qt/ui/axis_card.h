/* 单轴命令输入与反馈显示卡片；输入尚未绑定控制指令。 */

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

	/* 命令区读写：只交数值，不认业务语义（单位由界面自己标） */
	double targetPosition() const;
	double targetVelocity() const;
	double targetAcceleration() const;
	void setTargetPosition(double value);
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
