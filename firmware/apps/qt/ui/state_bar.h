/*
 * state_bar.h — 顶部状态区：显示控制层的当前状态
 *
 * 只有两部分：圆点 + 大号状态码（精确状态名，如 HOLDING / MOVING），
 * 下面一行中文说明。
 *
 * 状态码**不由界面生成**：由集成层从 `cats_control_state_name()` 取来传入，
 * 保证界面与日志永远同一套名字。界面只按粗粒度档位决定圆点颜色与说明文字。
 */

#ifndef STATE_BAR_H
#define STATE_BAR_H

#include <QWidget>

class QLabel;
class StatusLed;

class StateBar : public QWidget
{
	Q_OBJECT

      public:
	/* 粗粒度档位：只用于圆点颜色与说明文字，不追求与 cats_state 一一对应 */
	enum class State {
		Preparing = 0,
		Enabling,
		Running,
		Halting,
		Fault,
		EStop,
	};

	explicit StateBar(QWidget *parent = nullptr);

	/* 纯显示接口：state 决定颜色与说明，code 是展示用的精确状态名 */
	void setState(State state, const QString &code);

      private:
	StatusLed *led_ = nullptr;
	QLabel *hint_ = nullptr;
};

#endif /* STATE_BAR_H */
