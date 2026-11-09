#include "log_panel.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace
{
constexpr int kMaxLines = 2000;
} // namespace

LogPanel::LogPanel(QWidget *parent) : QFrame(parent)
{
	setObjectName(QStringLiteral("logPanel"));

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(20, 12, 20, 16);
	root->setSpacing(8);

	auto *header = new QHBoxLayout();
	auto *title = new QLabel(QStringLiteral("日志"), this);
	title->setObjectName(QStringLiteral("panelTitle"));
	btn_clear_ = new QPushButton(QStringLiteral("清空"), this);
	btn_clear_->setObjectName(QStringLiteral("btn_clear_log"));
	header->addWidget(title);
	header->addStretch(1);
	header->addWidget(btn_clear_);
	root->addLayout(header);

	view_ = new QPlainTextEdit(this);
	view_->setObjectName(QStringLiteral("logView"));
	view_->setReadOnly(true);
	view_->setMaximumBlockCount(kMaxLines);
	view_->setPlaceholderText(QStringLiteral("暂无日志"));
	root->addWidget(view_, 1);

	connect(btn_clear_, &QPushButton::clicked, view_, &QPlainTextEdit::clear);
}

void LogPanel::append(const QString &line)
{
	view_->appendPlainText(line);
}

void LogPanel::clear()
{
	view_->clear();
}
