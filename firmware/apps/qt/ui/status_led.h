/*
 * status_led.h — 圆点 + 文字的状态灯
 *
 * 只负责「按给定状态画」，不判断状态从哪来。圆点直径可调（状态区用大号）。
 */

#ifndef STATUS_LED_H
#define STATUS_LED_H

#include <QWidget>

class QLabel;

class StatusLed : public QWidget
{
	Q_OBJECT

      public:
	enum class State {
		Off,   /* 灰：未知 / 未连接 */
		Idle,  /* 黄：已连接未使能 */
		Busy,  /* 蓝：过程中 */
		Ok,    /* 绿：就绪 */
		Fault, /* 红：故障 */
	};

	explicit StatusLed(const QString &text, QWidget *parent = nullptr);

	void setState(State state);
	void setText(const QString &text);
	void setDotSize(int size);

      protected:
	void paintEvent(QPaintEvent *event) override;

      private:
	State state_ = State::Off;
	QLabel *label_ = nullptr;
	int dot_size_ = 9;
};

#endif /* STATUS_LED_H */
