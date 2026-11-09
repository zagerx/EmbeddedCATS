/* 加载 QSS 并展开调色板占位符。 */

#include "theme.h"

#include <QApplication>
#include <QFile>
#include <QString>

namespace
{
struct Token {
	const char *name;
	const char *value;
};

QString load_qss()
{
	QFile file(QStringLiteral(":/theme/app.qss"));
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		return QString();
	}
	return QString::fromUtf8(file.readAll());
}

QString expand_placeholders(QString qss)
{
	/* 长 token 必须排在它自己的前缀前面（@fgDim 要先于 @fg） */
	static const Token kTokens[] = {
		{"@fgDim", theme::color::kFgDim},   {"@bg", theme::color::kBg},
		{"@deep", theme::color::kDeep},     {"@surface", theme::color::kSurface},
		{"@border", theme::color::kBorder}, {"@divider", theme::color::kDivider},
		{"@fg", theme::color::kFg},         {"@primary", theme::color::kPrimary},
		{"@ok", theme::color::kOk},         {"@warn", theme::color::kWarn},
		{"@fault", theme::color::kFault},
	};

	for (const Token &token : kTokens) {
		qss.replace(QString::fromLatin1(token.name), QString::fromLatin1(token.value));
	}
	return qss;
}
} // namespace

void theme::apply(QApplication &app)
{
	app.setStyleSheet(expand_placeholders(load_qss()));
}
