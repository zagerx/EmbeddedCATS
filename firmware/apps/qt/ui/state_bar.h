/* 状态显示区；精确状态名由绑定层传入，本类只管理粗粒度展示。 */

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

	/* 纯显示接口：state 决定颜色与说明，code 是展示用的精确状态名 */
	void setState(State state, const QString &code);

	/* 补充一行说明（整机故障名等）；传空串则清掉 */
	void setDetail(const QString &detail);

      private:
	void refreshHint(void);

	StatusLed *led_ = nullptr;
	QLabel *hint_ = nullptr;
	QString hint_base_; /* “档位 · 说明”，由 setState 算好 */
	QString detail_;    /* 附在后面的补充信息 */
};

#endif /* STATE_BAR_H */
