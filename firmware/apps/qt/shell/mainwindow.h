/* 主窗口只组装状态区、工作区和日志区，不包含业务逻辑。 */

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

class LogPanel;
class QWidget;
class StateBar;

class MainWindow : public QMainWindow
{
	Q_OBJECT

      public:
	explicit MainWindow(QWidget *parent = nullptr);
	~MainWindow() override;

      private:
	StateBar *state_bar_ = nullptr;    /* 1.1 */
	QWidget *reserved_area_ = nullptr; /* 2.1 */
	LogPanel *log_panel_ = nullptr;    /* 3 */
};

#endif /* MAINWINDOW_H */
