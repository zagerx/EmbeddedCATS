/*
 * main.cpp — 集成层（Qt 壳）的进程入口
 *
 * 只做四件事：建 QApplication、套主题、建窗口、把业务接缝挂上去。
 */

#include "bindings/app_binder.h"
#include "shell/mainwindow.h"
#include "theme/theme.h"

#include <QApplication>

int main(int argc, char *argv[])
{
	QApplication app(argc, argv);
	theme::apply(app);

	MainWindow window;
	AppBinder binder; /* 通道与主站的所有权在集成层 */
	binder.attach(&window);
	window.show();

	return QApplication::exec();
}
