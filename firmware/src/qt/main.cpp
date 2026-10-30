/*
 * main.cpp — 集成层（Qt 壳）的进程入口
 *
 * 这里只做三件事：建 QApplication、建窗口、进事件循环。
 * 不放任何业务：装配在 MainWindow 里，时序在 app 里。
 */

#include "mainwindow.h"

#include <QApplication>

int main(int argc, char *argv[]) {
  QApplication app(argc, argv);

  MainWindow window;
  window.show();

  return QApplication::exec();
}
