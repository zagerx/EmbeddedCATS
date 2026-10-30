/*
 * mainwindow.h — 集成层（Qt 壳）的主窗口
 *
 * 这一层只做三件事：改按钮文字、显示状态、按节拍调 cats_app_poll()。
 * 它只 include app.h（和选后端用的 socketcan 工厂），不认识 CANopen 细节。
 *
 * 装配规则：主站 struct co_master、通道 struct socketcan_dev 的**存储都归壳**。
 * app 只借用指针；app 停下来之后，壳才能关通道。
 */

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

#include <app/app.h>
#include <socketcan/can_io_socketcan.h> /* 选哪个后端只发生在集成层 */

class QLabel;
class QPushButton;
class QTimer;

class MainWindow : public QMainWindow
{
	Q_OBJECT

      public:
	explicit MainWindow(QWidget *parent = nullptr);
	~MainWindow() override;

      private slots:
	void on_button_clicked();
	void on_tick();

      private:
	void set_ui_running(bool running);
	void show_message(const QString &text);

	QPushButton *button_ = nullptr;
	QLabel *status_ = nullptr;
	QTimer *timer_ = nullptr;

	socketcan_dev dev_{}; /* 通道存储归壳 */
	co_master master_{};  /* 主站存储归壳，app 借用指针 */
	can_io bus_{};
	bool bus_open_ = false;
};

#endif /* MAINWINDOW_H */
