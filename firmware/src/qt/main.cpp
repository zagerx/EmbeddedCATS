/*
 * main.cpp — 集成层（Qt 壳）的进程入口
 *
 * 只做三件事：建 QApplication、套主题、建窗口进事件循环。
 */

#include "shell/mainwindow.h"
#include "theme/theme.h"

#include <QApplication>

int main(int argc, char *argv[])
{
	QApplication app(argc, argv);
	theme::apply(app);

	MainWindow window;
	window.show();

	return QApplication::exec();
}
