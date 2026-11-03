/*
 * mainwindow.cpp — 纯界面装配，不用 QSplitter：
 *
 *   ┌────────────────┬──────────────┐
 *   │ 1.1 状态       │ 1.2 空白      │
 *   └────────────────┴──────────────┘    ← 大区域之间：留白
 *   ┌───────────────────────────────┐
 *   │ 2.1 空白                       │
 *   ├───────────────────────────────┤    ← 内部：1 px 实心分隔条
 *   │ 2.2 轴1   │   2.3 轴2         │    ← 内部：1 px 实心分隔条
 *   └───────────────────────────────┘
 *   ┌───────────────────────────────┐    ← 大区域之间：留白
 *   │ 3 号区域（日志）                │
 *   └───────────────────────────────┘
 *
 * 只有「new 控件 + 加进布局」；没有按钮响应、没有定时器、没有总线。
 */

#include "mainwindow.h"

#include "ui/axis_card.h"
#include "ui/log_panel.h"
#include "ui/state_bar.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

namespace
{
/* 1 px 实心分隔条：固定尺寸 + objectName 上色，不依赖 QSplitter 手柄 */
QFrame *make_divider(QWidget *parent, Qt::Orientation orientation)
{
	auto *divider = new QFrame(parent);
	divider->setObjectName(QStringLiteral("divider"));
	divider->setFrameShape(QFrame::NoFrame);
	if (orientation == Qt::Horizontal) {
		divider->setFixedHeight(1);
		divider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	} else {
		divider->setFixedWidth(1);
		divider->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
	}
	return divider;
}
} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
	auto *central = new QWidget(this);
	central->setObjectName(QStringLiteral("central"));

	auto *root = new QVBoxLayout(central);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(10); /* 大区域之间：留白 */

	/* ---- 1 号区域：1.1 状态机状态 | 竖线 | 1.2 空白 ---- */
	auto *top_area = new QWidget(central);
	top_area->setObjectName(QStringLiteral("topArea"));
	auto *top_layout = new QHBoxLayout(top_area);
	top_layout->setContentsMargins(0, 0, 0, 0);
	top_layout->setSpacing(0);

	state_bar_ = new StateBar(top_area);
	top_layout->addWidget(state_bar_, 1, Qt::AlignTop);

	top_layout->addWidget(make_divider(top_area, Qt::Vertical)); /* 1.1 / 1.2 */

	auto *top_reserved = new QWidget(top_area);
	top_reserved->setObjectName(QStringLiteral("topReserved"));
	auto *reserved_layout = new QHBoxLayout(top_reserved);
	reserved_layout->setContentsMargins(16, 10, 16, 10);
	reserved_layout->setSpacing(8);

	start_button_ = new QPushButton(QStringLiteral("启动"), top_reserved);
	start_button_->setObjectName(QStringLiteral("btn_start"));
	start_button_->setProperty("role", QStringLiteral("ok"));
	start_button_->setMinimumWidth(92);
	reserved_layout->addWidget(start_button_, 0, Qt::AlignTop);
	reserved_layout->addStretch(1);

	top_layout->addWidget(top_reserved, 1);

	top_area->setMinimumHeight(240); /* 保证 1 号区域约占可用高度的 1/3 */
	root->addWidget(top_area, 1);

	/* ---- 2 号区域：2.1 空白 / 横线 / 2.2 竖线 2.3 ---- */
	auto *work_area = new QWidget(central);
	work_area->setObjectName(QStringLiteral("workArea"));
	auto *work_layout = new QVBoxLayout(work_area);
	work_layout->setContentsMargins(0, 0, 0, 0);
	work_layout->setSpacing(0);

	reserved_area_ = new QWidget(work_area);
	reserved_area_->setObjectName(QStringLiteral("reservedArea"));
	reserved_area_->setFixedHeight(64);
	work_layout->addWidget(reserved_area_);

	work_layout->addWidget(make_divider(work_area, Qt::Horizontal)); /* 2.1 / 双轴 */

	auto *axis_row = new QWidget(work_area);
	axis_row->setObjectName(QStringLiteral("axisRow"));
	auto *axis_layout = new QHBoxLayout(axis_row);
	axis_layout->setContentsMargins(0, 0, 0, 0);
	axis_layout->setSpacing(0);

	axis_layout->addWidget(new AxisCard(QStringLiteral("轴 1"), axis_row), 1);
	axis_layout->addWidget(make_divider(axis_row, Qt::Vertical)); /* 2.2 / 2.3 */
	axis_layout->addWidget(new AxisCard(QStringLiteral("轴 2"), axis_row), 1);

	work_layout->addWidget(axis_row, 1);
	root->addWidget(work_area, 1);

	/* ---- 3 号区域：日志 ---- */
	log_panel_ = new LogPanel(central);
	root->addWidget(log_panel_, 1);

	setCentralWidget(central);
	setWindowTitle(QStringLiteral("CATS 主站 — 双轴 CANopen"));
	resize(1180, 760);

	/* 底部状态栏：只显示文字，不反映任何状态 */
	auto *bus_label = new QLabel(QStringLiteral("总线：can0 · 1 Mbit/s"), this);
	auto *node_label = new QLabel(QStringLiteral("节点：1 / 2"), this);
	statusBar()->addPermanentWidget(bus_label);
	statusBar()->addPermanentWidget(node_label);
}

MainWindow::~MainWindow() = default;
