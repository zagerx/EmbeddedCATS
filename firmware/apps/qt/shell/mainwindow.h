/*
 * mainwindow.h — 壳层：主窗口只做「拼接」，不碰任何业务
 *
 * 三个大区域：1 号（状态）/ 2 号（工作区）/ 3 号（日志）
 * 区域之间用留白分隔；区域内部（1.1/1.2、2.1/2.2/2.3）用 1 px 实心分隔条。
 * 这里不 include app、不建 QTimer、不选后端 —— 逻辑接回时走 bindings/。
 */

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
