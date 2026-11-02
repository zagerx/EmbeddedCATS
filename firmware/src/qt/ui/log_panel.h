/*
 * log_panel.h — 底部只读日志面板
 *
 * 只有「追加一行 / 清空」两个纯显示动作，不产生任何日志内容。
 */

#ifndef LOG_PANEL_H
#define LOG_PANEL_H

#include <QFrame>

class QPlainTextEdit;
class QPushButton;

class LogPanel : public QFrame
{
	Q_OBJECT

      public:
	explicit LogPanel(QWidget *parent = nullptr);

	void append(const QString &line);
	void clear();

      private:
	QPlainTextEdit *view_ = nullptr;
	QPushButton *btn_clear_ = nullptr;
};

#endif /* LOG_PANEL_H */
