/* 只读日志面板。 */

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
