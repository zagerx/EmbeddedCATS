#include "status_led.h"

#include "theme/theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>

namespace
{
constexpr int kDotMargin = 7;

QColor color_of(StatusLed::State state)
{
	switch (state) {
	case StatusLed::State::Ok:
		return QColor(QString::fromLatin1(theme::color::kOk));
	case StatusLed::State::Busy:
		return QColor(QString::fromLatin1(theme::color::kPrimary));
	case StatusLed::State::Fault:
		return QColor(QString::fromLatin1(theme::color::kFault));
	case StatusLed::State::Idle:
		return QColor(QString::fromLatin1(theme::color::kWarn));
	case StatusLed::State::Off:
	default:
		return QColor(QString::fromLatin1(theme::color::kFgDim));
	}
}
} // namespace

StatusLed::StatusLed(const QString &text, QWidget *parent) : QWidget(parent)
{
	auto *layout = new QHBoxLayout(this);
	layout->setContentsMargins(dot_size_ + kDotMargin, 0, 0, 0);
	layout->setSpacing(0);

	label_ = new QLabel(text, this);
	layout->addWidget(label_);
}

void StatusLed::setState(State state)
{
	state_ = state;
	update();
}

void StatusLed::setText(const QString &text)
{
	label_->setText(text);
}

void StatusLed::setDotSize(int size)
{
	dot_size_ = size;
	if (auto *box = qobject_cast<QHBoxLayout *>(layout())) {
		box->setContentsMargins(dot_size_ + kDotMargin, 0, 0, 0);
	}
	update();
}

void StatusLed::paintEvent(QPaintEvent *event)
{
	QWidget::paintEvent(event);

	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing, true);
	painter.setPen(Qt::NoPen);
	painter.setBrush(color_of(state_));
	painter.drawEllipse(QPointF(dot_size_ / 2.0, height() / 2.0), dot_size_ / 2.0,
			    dot_size_ / 2.0);
}
