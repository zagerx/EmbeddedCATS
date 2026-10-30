/*
 * mainwindow.cpp — 一个「启动 / 停止」按钮 + 一行状态文字
 *
 * 装配顺序（集成层的本职）：
 *   选后端 socketcan → 打开 can0 → 建主站 → 交给 app → 起 QTimer 周期推进
 */

#include "mainwindow.h"

#include <app/app.h>

#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace
{
constexpr int kTickMs = 20; /* 界面节拍；也是 cats_app_poll 的单次等待上限 */
const char kCanIfName[] = "can0";
} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
	auto *central = new QWidget(this);
	auto *layout = new QVBoxLayout(central);

	button_ = new QPushButton(QStringLiteral("启动"), central);
	status_ = new QLabel(QStringLiteral("未启动"), central);
	layout->addWidget(button_);
	layout->addWidget(status_);
	setCentralWidget(central);

	setWindowTitle(QStringLiteral("CATS 主站"));
	resize(360, 120);

	timer_ = new QTimer(this);
	timer_->setInterval(kTickMs);
	connect(timer_, &QTimer::timeout, this, &MainWindow::on_tick);
	connect(button_, &QPushButton::clicked, this, &MainWindow::on_button_clicked);
}

MainWindow::~MainWindow()
{
	timer_->stop();
	cats_app_stop();
	if (bus_open_) {
		can_io_close(&bus_);
	}
}

void MainWindow::on_button_clicked()
{
	if (cats_app_state() == CATS_RUN_RUNNING) {
		timer_->stop();
		cats_app_stop();
		can_io_close(&bus_);
		bus_open_ = false;
		set_ui_running(false);
		show_message(QStringLiteral("已停止"));
		return;
	}

	/* 1. 选后端并打开通道（这一段将来换平台时只改这里） */
	if (socketcan_open(&bus_, &dev_, kCanIfName) != CAN_IO_OK) {
		show_message(QStringLiteral("打开 %1 失败：接口是否 up、波特率是否 1M？")
				     .arg(QString::fromLatin1(kCanIfName)));
		return;
	}
	bus_open_ = true;

	/* 2. 建主站并登记节点（双轴 = 节点 1 与节点 2） */
	co_master_init(&master_, &bus_);
	co_master_add_node(&master_, 1);
	co_master_add_node(&master_, 2);

	/* 3. 交给应用层，由它负责之后的时序 */
	if (cats_app_start(&master_) != CATS_APP_OK) {
		can_io_close(&bus_);
		bus_open_ = false;
		show_message(QStringLiteral("app 启动失败"));
		return;
	}

	/* 3. 起节拍 */
	timer_->start();
	set_ui_running(true);
	show_message(QStringLiteral("已启动"));
}

void MainWindow::on_tick()
{
	if (cats_app_poll(kTickMs) == CATS_APP_OK) {
		return;
	}
	timer_->stop();
	cats_app_stop();
	can_io_close(&bus_);
	bus_open_ = false;
	set_ui_running(false);
	show_message(QStringLiteral("总线出错，已停止"));
}

void MainWindow::set_ui_running(bool running)
{
	button_->setText(running ? QStringLiteral("停止") : QStringLiteral("启动"));
}

void MainWindow::show_message(const QString &text)
{
	status_->setText(text);
}
