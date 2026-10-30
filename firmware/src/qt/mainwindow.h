/*
 * mainwindow.h — 集成层（Qt 壳）的主窗口
 *
 * 这一层只做三件事：改按钮文字、显示状态、按节拍调 cats_app_poll()。
 * 它只 include app.h（和选后端用的 socketcan 工厂），不认识 CANopen。
 */

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

#include <socketcan/can_io_socketcan.h> /* 选哪个后端只发生在集成层 */

class QLabel;
class QPushButton;
class QTimer;

class MainWindow : public QMainWindow {
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

  socketcan_dev_t dev_{}; /* 通道存储归壳所有 */
  can_io_t bus_{};
  bool bus_open_ = false;
};

#endif /* MAINWINDOW_H */
