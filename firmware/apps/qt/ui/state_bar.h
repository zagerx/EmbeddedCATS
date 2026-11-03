/*
 * state_bar.h — 顶部状态区：显示应用层状态机的当前状态
 *
 * 只有两部分：圆点 + 大号状态码（PREPARING / ENABLING / RUNNING / …），
 * 下面一行中文说明。
 *
 * 枚举与 app 的 `cats_run_state` 一一对应，但这里**不 include app.h**：
 * 只是长得像，接逻辑时由 bindings/ 做一次映射。
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
	enum class State {
		Preparing = 0,
		Enabling,
		Running,
		Stopping,
		Fault,
		EStop,
	};

	explicit StateBar(QWidget *parent = nullptr);

	/* 纯显示接口：改圆点颜色、改大字、改提示 */
	void setState(State state);

      private:
	StatusLed *led_ = nullptr;
	QLabel *hint_ = nullptr;
};

#endif /* STATE_BAR_H */
