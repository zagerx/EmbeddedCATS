/*
 * app_binder.h — 界面与 app 的唯一接缝
 *
 * 职责（只做"搬运"，不含任何业务判断）：
 *   CanThread            → 建主站、cats_control_init、1 ms 节拍调 cats_control_poll()
 *   QTimer 50 ms         → 读状态刷界面（不跟着 1 ms 节拍跑）
 *   QTimer 5 s           → 打一行节拍统计
 *
 * 不绑定任何按钮：启动流程由状态机自己推进。
 *
 * 所有权：CAN 通道由本层（集成层）打开与关闭；主站对象在 CanThread 里，
 * 只在控制线程访问。界面控件用 objectName 取（findChild），不向 shell/ui
 * 暴露任何业务头。
 *
 * 时序：**两个线程**。控制线程只跑节拍，界面线程只跑界面；两者之间只有
 * control 层发布的只读值（见 control.h「线程约定」），本层不共享别的数据。
 */

#ifndef BINDINGS_APP_BINDER_H
#define BINDINGS_APP_BINDER_H

#include <socketcan/can_io_socketcan.h>

#include <control/control.h>

#include "can_thread.h"

#include <QObject>
#include <QString>

class QTimer;
class QWidget;

class LogPanel;
class StateBar;

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

      private:
	void findWidgets(QWidget *root);         /* 按 objectName 取状态条与日志面板 */
	bool openChannel(const QString &ifname); /* 只开通道；建主站在控制线程里做 */
	void startTimers();                      /* 刷新与统计两个界面定时器 */
	void publish();                    /* app 状态 → 界面（状态没变就不碰） */
	void pushLog(const QString &line); /* 往日志面板追加一行 */

	struct can_io bus_;
	struct socketcan_dev dev_;
	CanThread worker_;          /* 控制线程：节拍与状态机的唯一所有者 */
	bool channel_open_ = false; /* 只为析构时决定要不要 close */

	QTimer *refresh_ = nullptr;
	QTimer *stats_ = nullptr;
	StateBar *state_bar_ = nullptr;
	LogPanel *log_panel_ = nullptr;

	/* 上一次刷给界面的状态；初值用一个不可能的状态，保证首帧一定刷 */
	enum cats_state last_state_ = CATS_STATE_COUNT;
};

#endif /* BINDINGS_APP_BINDER_H */
