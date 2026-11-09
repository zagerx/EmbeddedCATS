/* Qt 应用入口。 */

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
