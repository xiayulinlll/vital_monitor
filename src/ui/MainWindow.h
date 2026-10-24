#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QThread>
#include <QVector>
#include <QElapsedTimer>

#include "data/SampleData.h"
#include "data/VitalSignSimulator.h"
#include "core/ChannelConfig.h"
#include "core/RingBuffer.h"
#include "core/AlarmDetector.h"
#include "core/UserRole.h"

class TrendChartWidget;
class VitalValuePanel;
class ChannelConfigPanel;
class AlarmBanner;
class PatientBarWidget;
class QPlainTextEdit;
class QLabel;
class QPushButton;
class QTimer;
class QAction;

/**
 * @brief 主窗口（对应架构 2.4 主 UI 面板层，并充当业务与数据管理层宿主）。
 *
 * 线程模型：
 *  - VitalSignSimulator 位于独立 QThread；
 *  - 采样帧经 QueuedConnection 信号到达本窗口后才能访问 UI；
 *  - 历史数据保存在环形缓冲区（RingBuffer），内存恒定。
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

signals:
    /// 通知采集线程启停（跨线程调用，自动排队）。
    void requestStart(int intervalMs);
    void requestStop();
    void requestUpdateConfigs(const QVector<ChannelConfig> &configs);

private slots:
    void onStartStopClicked();
    void onFrameReady(const SampleFrame &frame);
    void onStatusText(const QString &text);
    void onConfigApplied(const QVector<ChannelConfig> &channels);
    void onClearClicked();
    void onImportClicked();
    void onExportImageClicked();
    void onExportDataClicked();
    void onFollowToggled(bool on);
    void onSpanChanged(int value);
    void onPlaybackTick();
    /// 报警条「确认」按钮：签收全部活动告警（对应报警状态机 Acknowledged）。
    void onAcknowledgeAlarms();
    /// 报警条「暂停 / 恢复」：暂停必须留下审计轨迹（时长 + 起止）。
    void onAlarmPauseChanged(bool paused, int seconds);
    /// 配置面板切换患者类型：加载对应报警限剖面。
    void onPatientTypeChanged(PatientType type);
    /// 用户切换角色（查看者 / 操作者 / 管理员）。
    void onRoleChangeRequested(UserRole role);
    /// 定时刷新报警列表的「已持续 N 秒」（1 s 一次）。
    void onAlarmDurationTick();
    /// 采集线程心跳超时检查（看门狗）。
    void onWatchdogTick();
    /// 打开审计日志校验结果窗口（4.2 第 3 项：可校验的审计追踪）。
    void onVerifyAuditClicked();

private:
    void setupUi();
    void setupThreading();
    void appendLog(const QString &text);
    void updateStatusBar();

    /// 把「告警事件」写入审计日志（按类别 / 状态分级）。
    void auditAlarm(const AlarmEvent &a);
    /// 记录配置变更差异到审计日志（含旧值 -> 新值）。
    void auditConfigDiff(const QVector<ChannelConfig> &oldCfgs,
                         const QVector<ChannelConfig> &newCfgs);
    /// 依据告警侦测器当前状态刷新报警条与数值面板边框。
    void refreshAlarmUi();

    /**
     * @brief 统一权限闸门（8.2 第 2 项）。
     *
     * 越权操作必须**同时**做两件事：拒绝执行 + 写审计。
     * 只拒绝不记录，事后无法判断「是谁尝试改了报警限」；
     * 只记录不拒绝，等于没有权限控制。
     *
     * @return true = 已授权，可继续执行。
     */
    bool ensurePermission(Permission p, const QString &actionDescription);

    /// 开机自检（7.1 第 5 项），启动时调用一次并显示结论。
    void runSelfTest();

    /// 当前角色名（写审计用）。
    QString currentActor() const;

    // ---- 业务与数据管理（主线程） ----
    ChannelConfigManager m_configMgr;
    RingBuffer<SampleFrame> m_historyBuffer{60000};   ///< 历史采样环形缓冲
    RingBuffer<AlarmEvent>  m_alarmBuffer{2000};      ///< 告警事件环形缓冲
    AlarmDetector           m_alarmDetector;

    /// 按通道缓存最近若干帧，用于绘图控件（兼容局部重绘）。
    QVector<SampleFrame> m_pendingFrames;
    QVector<AlarmEvent>  m_pendingAlarms;

    // ---- 采集线程 ----
    QThread           *m_workerThread = nullptr;
    VitalSignSimulator *m_simulator = nullptr;

    // ---- UI ----
    TrendChartWidget   *m_chart = nullptr;
    VitalValuePanel    *m_valuePanel = nullptr;
    ChannelConfigPanel *m_configPanel = nullptr;
    AlarmBanner        *m_alarmBanner = nullptr;
    PatientBarWidget   *m_patientBar = nullptr;
    QPlainTextEdit     *m_logView = nullptr;
    QLabel             *m_statusLabel = nullptr;
    QLabel             *m_viewportLabel = nullptr;
    QPushButton        *m_startStopBtn = nullptr;

    // ---- 权限 / 看门狗 / 定时刷新 ----
    UserRole  m_role = UserRole::Operator;
    QTimer   *m_durationTimer = nullptr;   ///< 报警列表持续时长刷新（1 s）
    QTimer   *m_watchdogTimer = nullptr;   ///< 采集心跳看门狗（2 s）
    quint64   m_lastHeartbeatIndex = 0;    ///< 最近一次心跳中的采样序号
    quint64   m_lastSeenHeartbeatIndex = 0;///< 上次看门狗检查时看到的采样序号
    int       m_watchdogMissCount = 0;     ///< 连续「序号未推进」次数
    bool      m_watchdogFired = false;     ///< 是否已上报过停滞（避免刷屏）

    // ---- 回放 ----
    QTimer        *m_playbackTimer = nullptr;
    QVector<SampleFrame> m_playbackFrames;
    int            m_playbackPos = 0;
    bool           m_playingBack = false;
    bool           m_running = false;

    qint64 m_frameCount = 0;
    quint64 m_droppedFrames = 0;   ///< 累计丢帧数（数据完整性统计）
    /// 最近一帧的采集时间戳（毫秒），作为告警确认等操作的统一时基。
    qint64 m_lastFrameMs = 0;
    QElapsedTimer m_uptime;
};

#endif // MAINWINDOW_H
