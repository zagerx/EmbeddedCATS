/*
 * state_bar.h — 顶部状态区：显示应用层状态机的当前状态
 *
 * 左侧：圆点 + 大号状态码（IDLE / STARTING / RUNNING / STOPPING / FAULT / ESTOP）
 *       下面一行中文说明；
 * 右侧：六个状态小标签，当前那个高亮。
 *
 * 枚举与 app 的 `cats_run_state` 一一对应，但这里**不 include app.h**：
 * 只是长得像，接逻辑时由 bindings/ 做一次映射。
 */

#ifndef STATE_BAR_H
#define STATE_BAR_H

#include <QVector>
#include <QWidget>

class QLabel;
class StatusLed;

class StateBar : public QWidget
{
	Q_OBJECT

      public:
	enum class State {
		Idle = 0,
		Starting,
		Running,
		Stopping,
		Fault,
		EStop,
	};

	explicit StateBar(QWidget *parent = nullptr);

	/* 纯显示接口：改圆点颜色、改大字、改提示、换高亮小标签 */
	void setState(State state);

      private:
	struct Chip {
		State state;
		QLabel *label;
	};

	StatusLed *led_ = nullptr;
	QLabel *hint_ = nullptr;
	QVector<Chip> chips_;
};

#endif /* STATE_BAR_H */
