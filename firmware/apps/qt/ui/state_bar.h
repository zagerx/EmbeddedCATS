/* 状态显示区；只负责显示绑定层提供的文字，不识别控制层枚举。
 *
 * 内部纵向分两块：1.1.1 主站状态（灯 + 状态文字）、1.1.2 主站故障信息。 */

#ifndef STATE_BAR_H
#define STATE_BAR_H

#include <QString>
#include <QWidget>

class QLabel;
class StatusLed;

class StateBar : public QWidget
{
	Q_OBJECT

      public:
	/* 粗粒度档位：只用于圆点颜色，不追求与 controller_state 一一对应 */
	enum class State {
		Preparing = 0,
		Idle,
		Enabling,
		Running,
		Halting,
		Fault,
		EStop,
	};

	explicit StateBar(QWidget *parent = nullptr);

	/* 纯显示接口：state 决定颜色与圆点，text 由绑定层提供 */
	void setState(State state, const QString &text);

	/* 1.1.2 故障四项：阶段 / 原因 / 轴 / abort 码，文字由绑定层提供 */
	void setFault(const QString &stage, const QString &reason, const QString &axis,
		      const QString &abort);

      private:
	StatusLed *led_ = nullptr;      /* 1.1.1：圆点 + 状态文字 */
	QLabel *stage_value_ = nullptr; /* 1.1.2：阶段 */
	QLabel *cause_value_ = nullptr; /* 1.1.2：原因 */
	QLabel *axis_value_ = nullptr;  /* 1.1.2：轴位图 */
	QLabel *abort_value_ = nullptr; /* 1.1.2：abort 码 */
};

#endif /* STATE_BAR_H */
