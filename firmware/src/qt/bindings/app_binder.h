/*
 * app_binder.h — 界面与 app 的唯一接缝
 *
 * 职责（只做"搬运"，不含任何业务判断）：
 *   cats_app_init()     → 绑定主站并进入准备态
 *   QTimer 20 ms        → cats_app_poll()
 *   app 状态            → StateBar::setState()
 *   cats_app_set_log()  → LogPanel::append()
 *
 * 不绑定任何按钮：启动流程由状态机自己推进。
 *
 * 所有权：CAN 通道与主站由本层（集成层）持有并创建，app 只借用指针。
 * 界面控件用 objectName 取（findChild），不向 shell/ui 暴露任何业务头。
 *
 * 时序：单线程。定时器跑在 GUI 线程，CATS 一侧全是非阻塞调用，
 * 所以不需要加锁，也不会有界面卡顿。
 */

#ifndef BINDINGS_APP_BINDER_H
#define BINDINGS_APP_BINDER_H

#include <canopen/master.h>
#include <socketcan/can_io_socketcan.h>

#include <app/app.h>

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

	/* 找控件、开通道、建主站、启动节拍。失败时已自行报警并禁用按钮，无需返回值 */
	void attach(QWidget *root, const QString &ifname = QStringLiteral("can0"));

      private slots:
	void onTick();

      private:
	void findWidgets(QWidget *root);         /* 按 objectName 取状态条与日志面板 */
	bool openChannel(const QString &ifname); /* 开通道 + 建主站（节点 1/2） */
	void startTimer();                       /* 20 ms 节拍 → cats_app_poll */
	void publish();                          /* app 状态 → 界面 */
	void pushLog(const QString &line);       /* 往日志面板追加一行 */

	static void logSink(const char *line); /* C 回调 → 本对象 */

	struct can_io bus_;
	struct socketcan_dev dev_;
	struct co_master master_;
	bool channel_open_ = false; /* 只为析构时决定要不要 close */

	QTimer *timer_ = nullptr;
	StateBar *state_bar_ = nullptr;
	LogPanel *log_panel_ = nullptr;

	/* 上一次刷给界面的状态；初值用一个不可能的状态，保证首帧一定刷 */
	enum cats_run_state last_state_ = CATS_RUN_STATE_COUNT;
};

#endif /* BINDINGS_APP_BINDER_H */
