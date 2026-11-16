/* 状态显示区；只负责把控制层上报的状态值与故障码画出来，不做任何名称映射。
 *
 * 内部纵向分两块：1.1.1 主站状态（灯 + 状态值）、1.1.2 主站故障码。 */

#ifndef STATE_BAR_H
#define STATE_BAR_H

#include <QWidget>

class QLabel;
class StatusLed;

class StateBar : public QWidget
{
	Q_OBJECT

      public:
	/* 粗粒度档位：只用于圆点颜色与说明文字，不追求与 controller_state 一一对应 */
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

	/* 纯显示接口：state 决定颜色与圆点，code 是控制层上报的状态数值 */
	void setState(State state, int code);

	/* 副行：控制层上报的故障码数值 */
	void setDetail(int fault);

      private:
	StatusLed *led_ = nullptr;      /* 1.1.1：圆点 + 状态值 */
	QLabel *fault_label_ = nullptr; /* 1.1.2 标签「故障码」 */
	QLabel *fault_value_ = nullptr; /* 1.1.2 数值（0 = 无故障） */
};

#endif /* STATE_BAR_H */
