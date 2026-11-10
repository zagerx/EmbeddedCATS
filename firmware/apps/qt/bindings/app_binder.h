/*
 * Qt 与控制的绑定层：管理 CAN 通道、工作线程、界面刷新和日志。
 * 控件按 objectName 查找，shell/ui 不依赖业务头。
 */

#ifndef BINDINGS_APP_BINDER_H
#define BINDINGS_APP_BINDER_H

#include <socketcan/can_io_socketcan.h>

#include <control/control.h>

#include "can_thread.h"

#include <QObject>
#include <QString>

class QComboBox;
class QPushButton;
class QTimer;
class QWidget;

class LogPanel;
class StateBar;
class AxisCard;

class AppBinder : public QObject
{
	Q_OBJECT

      public:
	explicit AppBinder(QObject *parent = nullptr);
	~AppBinder() override;

	/* 找控件、开通道、起控制线程。失败时已自行报警，无需返回值 */
	void attach(QWidget *root, const QString &ifname = QStringLiteral("can0"));

      private slots:
	void onRefresh(); /* 50 ms：状态 → 控件 */
	void onStats();   /* 5 s：节拍统计 → 日志 */
	void onMove();    /* 按轴选择下发位置指令 */
	void onHalt();    /* 减速停在当前位置 */
	void onEStop();   /* 所有轴 quick stop 并断使能 */

      private:
	void findWidgets(QWidget *root);         /* 按 objectName 取状态条与日志面板 */
	bool openChannel(const QString &ifname); /* 只开通道；建主站在控制线程里做 */
	void startTimers();                      /* 刷新与统计两个界面定时器 */
	void bindCommands();                     /* 2.1 的轴选择与三个按钮 */
	bool fillMove(uint8_t axis, struct stm_move_axis *out); /* 读卡片参数并校验 */
	void updateCommands(enum cats_state s);                 /* 按钮可用性跟随状态 */
	void tickPrefill();                /* 首次进 HOLDING：目标位置 = 实际位置 */
	void publish();                    /* app 状态 → 界面（状态没变就不碰） */
	void updateAxisCards();            /* 两轴实时样本 → 轴卡片（每 50 ms） */
	void pushLog(const QString &line); /* 往日志面板追加一行 */

	struct can_io bus_;
	struct socketcan_dev dev_;
	CanThread worker_;          /* 控制线程：节拍与状态机的唯一所有者 */
	bool channel_open_ = false; /* 只为析构时决定要不要 close */

	QTimer *refresh_ = nullptr;
	QTimer *stats_ = nullptr;
	StateBar *state_bar_ = nullptr;
	LogPanel *log_panel_ = nullptr;
	AxisCard *axis_cards_[2] = {nullptr, nullptr}; /* 轴 1 / 轴 2，按 objectName 取 */
	QComboBox *axis_selector_ = nullptr;           /* 运动下发到哪一轴 */
	QPushButton *btn_move_ = nullptr;
	QPushButton *btn_halt_ = nullptr;
	QPushButton *btn_estop_ = nullptr;
	uint32_t last_abort_[2] = {0U, 0U}; /* 上次已打过日志的 abort，防刷屏 */
	bool prefilled_ = false; /* 首次进入 HOLDING 已把目标位置填成实际位置 */

	/* 上一次刷给界面的状态；初值用一个不可能的状态，保证首帧一定刷 */
	enum cats_state last_state_ = CATS_STATE_COUNT;
	enum cats_fault last_fault_ = CATS_FAULT_NONE;
};

#endif /* BINDINGS_APP_BINDER_H */
