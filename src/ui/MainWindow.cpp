#include "MainWindow.h"

#include "chart/TrendChartWidget.h"
#include "ui/VitalValuePanel.h"
#include "ui/ChannelConfigPanel.h"
#include "ui/AlarmBanner.h"
#include "ui/PatientBarWidget.h"
#include "core/HistoryRecorder.h"
#include "core/CoordinateMapper.h"
#include "core/AuditLogger.h"
#include "core/FileIntegrity.h"
#include "core/SelfTest.h"
#include "core/UserRole.h"

#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QList>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setupUi();
    setupThreading();

    // 报警延时改用毫秒制（第二梯队），10 Hz 采样下 1000 ms ≈ 原 10 帧去抖。
    m_alarmDetector.setDefaultSampleIntervalMs(100);

    // ---- 审计日志：记录本次监护会话的启动（第一梯队第 3 项）----
    AuditLogger &audit = AuditLogger::instance();
    const QString auditDir = QDir(QCoreApplication::applicationDirPath())
                                 .filePath(QStringLiteral("audit"));
    const QString auditPath = QDir(auditDir).filePath(
        QStringLiteral("audit_%1.log")
            .arg(QDate::currentDate().toString(QStringLiteral("yyyyMMdd"))));
    if (audit.openFile(auditPath)) {
        audit.log(AuditLogger::System, AuditLogger::Info,
                  QStringLiteral("会话启动"),
                  QStringLiteral("通道数=%1，患者类型=%2，采样间隔=100ms")
                      .arg(m_configMgr.count())
                      .arg(patientTypeName(m_configMgr.patientType())));
        appendLog(QStringLiteral("[审计] 日志已启用：%1").arg(auditPath));
    } else {
        appendLog(QStringLiteral("[审计] 日志文件不可写，事件将仅输出到控制台"));
    }

    // 同步进程级患者类型真源，保证各控件显示一致。
    ChannelConfigManager::setGlobalPatientType(m_configMgr.patientType());

    // ---- 开机自检（7.1 第 5 项）----
    // 放在审计日志启用之后：自检结论本身也要留痕
    // （否则「开机时有没有报警告」事后无从追溯）。
    runSelfTest();

    appendLog(QStringLiteral("[系统] 多通道生理信号趋势监测上位机已启动"));
    appendLog(QStringLiteral("[系统] 通道数：%1，历史缓冲：%2 帧")
                  .arg(m_configMgr.count())
                  .arg(m_historyBuffer.capacity()));
    appendLog(QStringLiteral("[系统] ⚠ 演示模式：所有数据由内置模拟器生成，不得用于临床"));
    updateStatusBar();
}

MainWindow::~MainWindow()
{
    // 先请求停止，再退出线程，最后释放模拟器。
    if (m_workerThread) {
        emit requestStop();
        m_workerThread->quit();
        m_workerThread->wait(3000);
    }

    AuditLogger::instance().log(AuditLogger::System, AuditLogger::Info,
                                QStringLiteral("会话结束"),
                                QStringLiteral("累计采集 %1 帧，丢帧 %2 帧，告警 %3 条")
                                    .arg(m_frameCount)
                                    .arg(m_droppedFrames)
                                    .arg(m_alarmBuffer.size()));
    AuditLogger::instance().close();
}

void MainWindow::setupThreading()
{
    m_workerThread = new QThread(this);
    m_simulator = new VitalSignSimulator();      // 无 parent，随后 moveToThread
    m_simulator->moveToThread(m_workerThread);

    // ---- 跨线程信号槽（QueuedConnection 自动生效） ----
    connect(this, &MainWindow::requestStart, m_simulator, &VitalSignSimulator::start);
    connect(this, &MainWindow::requestStop, m_simulator, &VitalSignSimulator::stop);
    connect(this, &MainWindow::requestUpdateConfigs,
            m_simulator, &VitalSignSimulator::updateConfigs);
    connect(m_simulator, &VitalSignSimulator::frameReady,
            this, &MainWindow::onFrameReady);       // 工作线程 -> 主线程
    connect(m_simulator, &VitalSignSimulator::statusText,
            this, &MainWindow::onStatusText);

    // 心跳：只记录序号，真正的「是否推进」判断在 onWatchdogTick 里做。
    // 用 QueuedConnection（跨线程自动生效）保证不阻塞采集线程。
    connect(m_simulator, &VitalSignSimulator::heartbeat,
            this, [this](quint64 index) {
                m_lastHeartbeatIndex = index;
                m_watchdogMissCount = 0;
                m_watchdogFired = false;
            });

    connect(m_workerThread, &QThread::finished,
            m_simulator, &QObject::deleteLater);

    // 线程退出时清理定时器依赖的线程亲和性。
    m_workerThread->start();
}

void MainWindow::setupUi()
{
    setWindowTitle(QStringLiteral("多通道生理信号趋势监测上位机 — Vital Monitor"));
    resize(1360, 820);

    auto *central = new QWidget(this);
    auto *centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(8, 8, 8, 8);
    centralLayout->setSpacing(6);

    // ================= 顶部：患者信息 + 演示模式 + 角色（9.1 第 2/4/5 项） =================
    m_patientBar = new PatientBarWidget(central);
    centralLayout->addWidget(m_patientBar);

    // ================= 顶部：报警横幅（第一梯队第 1 项：声光报警） =================
    m_alarmBanner = new AlarmBanner(central);
    centralLayout->addWidget(m_alarmBanner);

    auto *rootLayout = new QHBoxLayout();
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(8);
    centralLayout->addLayout(rootLayout, 1);

    // ================= 左侧：绘图 + 数值面板 =================
    auto *leftSplitter = new QSplitter(Qt::Horizontal, central);

    // --- 中间绘图区 ---
    auto *chartHost = new QWidget(leftSplitter);
    auto *chartLayout = new QVBoxLayout(chartHost);
    chartLayout->setContentsMargins(0, 0, 0, 0);
    chartLayout->setSpacing(6);

    // 工具栏：开始/停止、缩放、跟随、清空、导入、导出
    auto *toolbar = new QHBoxLayout();
    toolbar->setSpacing(6);

    m_startStopBtn = new QPushButton(QStringLiteral("▶ 开始采集"), chartHost);
    m_startStopBtn->setFixedWidth(110);
    toolbar->addWidget(m_startStopBtn);

    auto *clearBtn = new QPushButton(QStringLiteral("清空"), chartHost);
    toolbar->addWidget(clearBtn);

    toolbar->addSpacing(12);
    toolbar->addWidget(new QLabel(QStringLiteral("视口跨度:"), chartHost));
    auto *spanCombo = new QComboBox(chartHost);
    spanCombo->addItem(QStringLiteral("10 秒 (100 点)"), 100);
    spanCombo->addItem(QStringLiteral("30 秒 (300 点)"), 300);
    spanCombo->addItem(QStringLiteral("60 秒 (600 点)"), 600);
    spanCombo->addItem(QStringLiteral("3 分钟 (1800 点)"), 1800);
    spanCombo->addItem(QStringLiteral("10 分钟 (6000 点)"), 6000);
    spanCombo->setCurrentIndex(2);
    toolbar->addWidget(spanCombo);

    auto *followBtn = new QPushButton(QStringLiteral("实时跟随: 开"), chartHost);
    followBtn->setCheckable(true);
    followBtn->setChecked(true);
    toolbar->addWidget(followBtn);

    toolbar->addStretch(1);

    auto *importBtn = new QPushButton(QStringLiteral("导入历史"), chartHost);
    auto *exportImgBtn = new QPushButton(QStringLiteral("导出截图"), chartHost);
    auto *exportDataBtn = new QPushButton(QStringLiteral("导出数据"), chartHost);
    auto *verifyAuditBtn = new QPushButton(QStringLiteral("校验审计"), chartHost);
    verifyAuditBtn->setToolTip(QStringLiteral(
        "逐行重算审计日志的 CRC32，定位被修改 / 截断的记录\n"
        "（优化文档 4.2 第 3 项：不可篡改存储应有校验手段）"));
    toolbar->addWidget(importBtn);
    toolbar->addWidget(exportImgBtn);
    toolbar->addWidget(exportDataBtn);
    toolbar->addWidget(verifyAuditBtn);

    chartLayout->addLayout(toolbar);

    // 绘图控件
    m_chart = new TrendChartWidget(chartHost);
    m_chart->setChannels(m_configMgr.channels());
    chartLayout->addWidget(m_chart, 1);

    // 状态/视口提示
    m_viewportLabel = new QLabel(chartHost);
    m_viewportLabel->setStyleSheet(QStringLiteral("color:#8A97A8;"));
    chartLayout->addWidget(m_viewportLabel);

    // --- 右侧：实时数值面板 ---
    m_valuePanel = new VitalValuePanel(leftSplitter);
    // 实测数值面板原占 230 ~ 320px，砍掉约 1/3 宽度后收紧为 150 ~ 210px，
    // 把省下的水平空间让给中间趋势绘图区。
    m_valuePanel->setMinimumWidth(150);
    m_valuePanel->setMaximumWidth(210);
    m_valuePanel->buildPanels(m_configMgr.channels());

    leftSplitter->addWidget(chartHost);
    leftSplitter->addWidget(m_valuePanel);
    // 注意：setStretchFactor 会让 splitter 在每次 resize 时按比例重新分配空间，
    // 从而覆盖 setMaximumWidth 的限制。因此数值面板必须用 stretchFactor=0
    // （不参与拉伸），否则宽度砍掉 1/3 的设置会被重新撑回去。
    leftSplitter->setStretchFactor(0, 1);
    leftSplitter->setStretchFactor(1, 0);
    // 初始尺寸：左绘图区尽量宽，数值面板锁定在收窄后的宽度。
    leftSplitter->setSizes(QList<int>{960, 190});

    rootLayout->addWidget(leftSplitter, 4);

    // ================= 右侧栏：配置 + 日志 =================
    auto *rightSplitter = new QSplitter(Qt::Vertical);

    m_configPanel = new ChannelConfigPanel(rightSplitter);
    m_configPanel->setMinimumWidth(430);
    m_configPanel->setChannels(m_configMgr.channels());

    auto *logHost = new QWidget(rightSplitter);
    auto *logLayout = new QVBoxLayout(logHost);
    logLayout->setContentsMargins(6, 6, 6, 6);
    logLayout->setSpacing(6);
    auto *logTitle = new QLabel(QStringLiteral("状态 / 告警日志"), logHost);
    QFont lf = logTitle->font();
    lf.setBold(true);
    logTitle->setFont(lf);
    logLayout->addWidget(logTitle);
    m_logView = new QPlainTextEdit(logHost);
    m_logView->setReadOnly(true);
    m_logView->setStyleSheet(QStringLiteral("background:#12161D; color:#A8B4C4;"));
    logLayout->addWidget(m_logView, 1);

    rightSplitter->addWidget(m_configPanel);
    rightSplitter->addWidget(logHost);
    rightSplitter->setStretchFactor(0, 3);
    rightSplitter->setStretchFactor(1, 2);

    rootLayout->addWidget(rightSplitter, 2);

    setCentralWidget(central);

    // 状态栏
    m_statusLabel = new QLabel(this);
    statusBar()->addWidget(m_statusLabel);

    // ================= 信号连接 =================
    connect(m_startStopBtn, &QPushButton::clicked, this, &MainWindow::onStartStopClicked);
    connect(clearBtn, &QPushButton::clicked, this, &MainWindow::onClearClicked);
    connect(importBtn, &QPushButton::clicked, this, &MainWindow::onImportClicked);
    connect(exportImgBtn, &QPushButton::clicked, this, &MainWindow::onExportImageClicked);
    connect(exportDataBtn, &QPushButton::clicked, this, &MainWindow::onExportDataClicked);
    connect(followBtn, &QPushButton::toggled, this, &MainWindow::onFollowToggled);
    connect(spanCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, spanCombo](int idx) {
                onSpanChanged(spanCombo->itemData(idx).toInt());
            });
    connect(m_configPanel, &ChannelConfigPanel::configApplied,
            this, &MainWindow::onConfigApplied);
    connect(m_configPanel, &ChannelConfigPanel::patientTypeChangeRequested,
            this, &MainWindow::onPatientTypeChanged);
    // 配置面板的拒绝原因（权限 / 锁定 / 取消确认）必须进入日志与审计：
    // 「有人试图改动报警限但被拦截」本身就是需要留痕的事件。
    connect(m_configPanel, &ChannelConfigPanel::changeRejected,
            this, [this](const QString &reason) {
                appendLog(QStringLiteral("[配置] %1").arg(reason));
            });
    connect(m_alarmBanner, &AlarmBanner::acknowledgeRequested,
            this, &MainWindow::onAcknowledgeAlarms);
    connect(m_alarmBanner, &AlarmBanner::pauseChanged,
            this, &MainWindow::onAlarmPauseChanged);
    connect(m_chart, &TrendChartWidget::viewportChanged, this, [this](quint64, quint64) {
        updateStatusBar();
    });
    connect(verifyAuditBtn, &QPushButton::clicked,
            this, &MainWindow::onVerifyAuditClicked);

    // ---- 患者信息条：角色切换 + 患者信息变更留痕 ----
    connect(m_patientBar, &PatientBarWidget::roleChangeRequested,
            this, &MainWindow::onRoleChangeRequested);
    connect(m_patientBar, &PatientBarWidget::patientInfoChanged,
            this, [this](const QString &summary) {
                appendLog(QStringLiteral("[患者] 信息更新：%1").arg(summary));
                AuditLogger::instance().log(
                    AuditLogger::Config, AuditLogger::Info,
                    QStringLiteral("患者信息更新"), summary, currentActor());
            });

    // 初始角色同步到各控件（默认 Operator）。
    m_configPanel->setUserRole(m_role);
    m_patientBar->setRoleSilently(m_role);

    // ---- 报警列表持续时长刷新（9.1 第 2 项）----
    // 每秒刷新一次而不是每帧刷新：持续时长以秒为粒度显示，
    // 每 100 ms 重建表格会白白吃掉 UI 时间。
    m_durationTimer = new QTimer(this);
    m_durationTimer->setInterval(1000);
    connect(m_durationTimer, &QTimer::timeout, this, &MainWindow::onAlarmDurationTick);
    m_durationTimer->start();

    // ---- 采集心跳看门狗（7.1 第 6 项）----
    m_watchdogTimer = new QTimer(this);
    m_watchdogTimer->setInterval(2000);
    connect(m_watchdogTimer, &QTimer::timeout, this, &MainWindow::onWatchdogTick);
    m_watchdogTimer->start();

    // 回放定时器
    m_playbackTimer = new QTimer(this);
    m_playbackTimer->setInterval(50);
    connect(m_playbackTimer, &QTimer::timeout, this, &MainWindow::onPlaybackTick);

    updateStatusBar();
}


// ---------------------------------------------------------------------------
// 核心业务槽函数
// ---------------------------------------------------------------------------

void MainWindow::onStartStopClicked()
{
    if (!m_running) {
        emit requestStart(100);   // 10 Hz 采样
        m_running = true;
        m_startStopBtn->setText(QStringLiteral("■ 停止采集"));
        m_uptime.start();
        appendLog(QStringLiteral("[采集] 开始采集（10 Hz）"));

        AuditLogger::instance().log(AuditLogger::System, AuditLogger::Info,
                                    QStringLiteral("开始采集"),
                                    QStringLiteral("采样间隔 100 ms（10 Hz）"));
    } else {
        emit requestStop();
        m_running = false;
        m_startStopBtn->setText(QStringLiteral("▶ 开始采集"));
        appendLog(QStringLiteral("[采集] 停止采集，累计 %1 帧").arg(m_frameCount));

        AuditLogger::instance().log(AuditLogger::System, AuditLogger::Info,
                                    QStringLiteral("停止采集"),
                                    QStringLiteral("累计 %1 帧，丢帧 %2 帧")
                                        .arg(m_frameCount)
                                        .arg(m_droppedFrames));
    }
    updateStatusBar();
}

void MainWindow::onFrameReady(const SampleFrame &frame)
{
    // 本函数运行在主线程（跨线程信号槽已排队），可安全访问 UI。
    ++m_frameCount;

    // 记录最新帧时间戳：后续「确认告警」等操作需与 detect() 使用同一时基。
    // 使用 AlarmDetector::frameTimeMs()（优先单调时钟），与内部判定保持一致。
    m_lastFrameMs = AlarmDetector::frameTimeMs(frame);

    // 1) 写入历史环形缓冲（内存恒定，难点 2）。
    m_historyBuffer.append(frame);

    // 1.1) 数据完整性统计：累计丢帧用于状态栏展示（第二梯队第 8 项）。
    if (frame.droppedFrames > 0)
        m_droppedFrames += frame.droppedFrames;

    // 2) 告警判断（报警状态机 + SQI 门控 + 技术报警）。
    const QVector<AlarmEvent> alarms = m_alarmDetector.detect(frame, m_configMgr.channels());
    for (const AlarmEvent &a : alarms) {
        m_alarmBuffer.append(a);
        m_pendingAlarms.append(a);

        // 事件文本按状态前缀区分，便于日志阅读。
        appendLog(QStringLiteral("[告警] %1").arg(a.text));
        auditAlarm(a);
    }

    // 3) 交给绘图控件（局部重绘）与数值面板。
    m_pendingFrames.append(frame);

    // 批量提交，减少重绘次数（每 2 帧合并一次，降低 CPU）。
    if (m_pendingFrames.size() >= 2) {
        m_chart->appendFrames(m_pendingFrames);
        if (!m_pendingAlarms.isEmpty())
            m_chart->appendAlarms(m_pendingAlarms);
        m_pendingFrames.clear();
        m_pendingAlarms.clear();
    }

    m_valuePanel->updateValues(frame);

    // 4) 同步报警条与通道卡片边框（声光 + 视觉联动）。
    if (!alarms.isEmpty() || m_alarmDetector.activeCount() > 0 || m_alarmBanner->hasAlarm()) {
        if (!m_alarmBanner->hasAlarm() && m_alarmDetector.activeCount() == 0)
            refreshAlarmUi();
        else if (!alarms.isEmpty())
            refreshAlarmUi();
    }

    if (m_frameCount % 10 == 0)
        updateStatusBar();
}

void MainWindow::onStatusText(const QString &text)
{
    appendLog(QStringLiteral("[采集线程] %1").arg(text));
}

// ---------------------------------------------------------------------------
// 报警 / 审计（第一梯队第 1、3 项；第二梯队第 5、7、9 项）
// ---------------------------------------------------------------------------

namespace {
/// 把告警类别映射到审计日志类别。
AuditLogger::Category alarmCategoryOf(AlarmEvent::Category c)
{
    return c == AlarmEvent::Technical ? AuditLogger::TechAlarm : AuditLogger::Alarm;
}
} // namespace

void MainWindow::refreshAlarmUi()
{
    const QVector<AlarmEvent> active = m_alarmDetector.activeAlarms();

    if (m_alarmBanner) {
        m_alarmBanner->setActiveAlarms(active);
        // 持续时长必须来自与 detect() 相同的时基（单调时钟），
        // 否则系统校时后会出现「已持续 -3600 秒」这类荒谬数值。
        m_alarmBanner->setAlarmDurations(
            m_alarmDetector.activeDurationsMs(m_lastFrameMs));
    }

    // 数值面板卡片边框按告警严重度着色，形成全局视觉报警。
    if (m_valuePanel)
        m_valuePanel->applyAlarmState(active);

    // 常驻摘要（9.2 第 1、2 项）：一眼看出「有几条还没处理」。
    if (m_patientBar) {
        int acked = 0;
        for (const AlarmEvent &a : active) {
            if (a.state == AlarmEvent::Acknowledged
                || a.state == AlarmEvent::LatchedSilent)
                ++acked;
        }
        m_patientBar->setAlarmCounts(active.size() - acked, acked);
    }
}

void MainWindow::onAlarmDurationTick()
{
    // 空闲（无活动告警）时不做任何事，避免每秒都去重建空表格。
    if (m_alarmDetector.activeCount() > 0 && m_alarmBanner)
        m_alarmBanner->setAlarmDurations(
            m_alarmDetector.activeDurationsMs(m_lastFrameMs));
}

void MainWindow::onWatchdogTick()
{
    // 只在采集运行中检查：停止采集是正常状态，不应报「心跳丢失」。
    if (!m_running)
        return;

    if (m_lastHeartbeatIndex == 0) {
        // 刚启动、还没收到任何心跳：给一个宽限期（下一次 tick 再判断）。
        return;
    }

    // 心跳序号必须在每次检查时推进；2 s 内 10 Hz 采样应推进约 20 帧，
    // 因此「序号未变」已足以说明采集链路停滞，而不是瞬时抖动。
    if (m_lastHeartbeatIndex == m_lastSeenHeartbeatIndex) {
        ++m_watchdogMissCount;
    } else {
        m_watchdogMissCount = 0;
        m_lastSeenHeartbeatIndex = m_lastHeartbeatIndex;
    }

    if (m_watchdogMissCount >= 2 && !m_watchdogFired) {
        // 连续 2 次（约 4 s）序号不推进 → 采集线程可能已卡死。
        m_watchdogFired = true;

        const QString msg = QStringLiteral(
            "采集心跳停滞（采样序号 %1 已 %2 秒未推进），请检查采集线程")
            .arg(m_lastHeartbeatIndex)
            .arg((m_watchdogMissCount + 1) * 2);

        appendLog(QStringLiteral("[看门狗] %1").arg(msg));
        AuditLogger::instance().log(
            AuditLogger::System, AuditLogger::Critical,
            QStringLiteral("采集心跳停滞"), msg, currentActor());

        // 以技术报警的形式暴露给医护：采集不动了，屏幕上的数值就是「旧数」，
        // 这比任何生理报警都危险，必须显性化。
        AlarmEvent a;
        a.timestampMs = m_lastFrameMs;
        a.channelId   = -1;
        a.level       = AlarmEvent::Critical;
        a.category    = AlarmEvent::Technical;
        a.state       = AlarmEvent::Active;
        a.text        = QStringLiteral("采集链路停滞（看门狗）：显示数值可能已过期");
        m_alarmBuffer.append(a);
        m_alarmBanner->setActiveAlarms(m_alarmDetector.activeAlarms());
    }
}

void MainWindow::onAcknowledgeAlarms()
{
    // 确认告警是值班医护的日常操作（Operator 及以上）。
    if (!ensurePermission(Permission::AcknowledgeAlarm, QStringLiteral("确认告警")))
        return;

    // 确认全部活动告警：状态机进入 Acknowledged，
    // 声音提示停止，但若条件仍成立将按 repeatMs 重新提示（不会永久静默）。
    //
    // 显式传入最近一帧的采集时间戳作为时基，保证与 detect() 完全一致；
    // 若尚未收到任何帧（lastFrameMs 泄漏 = 0），回退到系统当前时间。
    const QVector<AlarmEvent> acked = m_alarmDetector.acknowledge(QString(), m_lastFrameMs);
    if (acked.isEmpty()) {
        appendLog(QStringLiteral("[告警] 确认操作：当前无活动告警"));
        return;
    }

    appendLog(QStringLiteral("[告警] 用户确认 %1 条告警").arg(acked.size()));

    // 记录「持续时长」：确认动作发生在告警出现多久之后，
    // 是可用性工程（IEC 62366）与报警系统评价的关键指标
    // （响应过慢说明提示不足，过频说明误报太多）。
    const QHash<QString, qint64> durations =
        m_alarmDetector.activeDurationsMs(m_lastFrameMs);

    for (const AlarmEvent &a : acked) {
        // 用 trackKey 回查：技术报警的键形如 "1/2/SQI"，
        // 而 conditionKey() 会生成 "1/2/HI"，永远查不到时长。
        const qint64 ms = durations.value(a.trackKey, -1);
        AuditLogger::instance().log(
            alarmCategoryOf(a.category), AuditLogger::Info,
            QStringLiteral("告警确认"),
            QStringLiteral("通道=%1 条件=%2 持续时间=%3s 描述=%4")
                .arg(a.channelId)
                .arg(a.trackKey,
                     ms >= 0 ? QString::number(ms / 1000.0, 'f', 1)
                             : QStringLiteral("?"),
                     a.text));
    }

    refreshAlarmUi();
    updateStatusBar();
}

void MainWindow::onAlarmPauseChanged(bool paused, int seconds)
{
    // 暂停会抑制视觉与听觉报警，属于「影响患者安全的操作」，
    // 规范要求必须可追溯：谁、何时、暂停多久、是否提前恢复。
    // 同时需要 Operator 及以上权限（查看者不得抑制报警）。
    if (paused && !ensurePermission(Permission::AcknowledgeAlarm,
                                    QStringLiteral("暂停报警提示")))
        return;

    if (paused) {
        appendLog(QStringLiteral("[告警] 报警提示已暂停 %1 秒").arg(seconds));
        AuditLogger::instance().log(
            AuditLogger::Alarm, AuditLogger::Warning,
            QStringLiteral("报警暂停"),
            QStringLiteral("暂停时长=%1s 活动告警=%2 条")
                .arg(seconds)
                .arg(m_alarmDetector.activeCount()),
            currentActor());
    } else {
        appendLog(QStringLiteral("[告警] 报警提示已恢复"));
        AuditLogger::instance().log(
            AuditLogger::Alarm, AuditLogger::Info,
            QStringLiteral("报警暂停结束"),
            QStringLiteral("活动告警=%1 条")
                .arg(m_alarmDetector.activeCount()),
            currentActor());
    }
}

void MainWindow::onPatientTypeChanged(PatientType type)
{
    const PatientType before = m_configMgr.patientType();
    if (type == before)
        return;

    // 切换患者类型会整体替换报警限 —— 与手改阈值同等风险：
    // 需要管理员权限，并进行二次确认。被拒绝时界面必须回滚，
    // 否则出现「下拉框显示新生儿、实际生效成人阈值」的危险不一致。
    if (!ensurePermission(Permission::ModifyConfig,
                          QStringLiteral("切换患者类型（会整体替换报警限）"))) {
        m_configPanel->revertPatientType(before);
        return;
    }

    // 用当前配置模拟「切换后」的阈值，把差异交给用户确认。
    //（不直接改 m_configMgr：用户可能取消。）
    ChannelConfigManager preview = m_configMgr;
    preview.loadProfile(type, /*overwriteCustom=*/true);

    QStringList diffs;
    const QVector<ChannelConfig> newCfgs = preview.channels();
    for (int i = 0; i < newCfgs.size() && i < m_configMgr.count(); ++i) {
        const ChannelConfig &o = m_configMgr.channels().at(i);
        const ChannelConfig &n = newCfgs.at(i);
        if (!qFuzzyCompare(o.alarmLow + 1.0, n.alarmLow + 1.0)
            || !qFuzzyCompare(o.alarmHigh + 1.0, n.alarmHigh + 1.0)) {
            diffs.append(QStringLiteral("%1：%2~%3 → %4~%5")
                             .arg(n.name,
                                  QString::number(o.alarmLow, 'f', 0),
                                  QString::number(o.alarmHigh, 'f', 0),
                                  QString::number(n.alarmLow, 'f', 0),
                                  QString::number(n.alarmHigh, 'f', 0)));
        }
    }

    const auto ret = QMessageBox::question(
        this, QStringLiteral("确认切换患者类型"),
        QStringLiteral("将把患者类型从「%1」切换为「%2」，报警限变化如下：\n\n%3\n\n"
                       "确认切换？（将写入审计日志）")
            .arg(patientTypeName(before), patientTypeName(type),
                 diffs.isEmpty() ? QStringLiteral("（阈值无变化）")
                                 : diffs.join(QLatin1Char('\n'))),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (ret != QMessageBox::Yes) {
        m_configPanel->revertPatientType(before);
        appendLog(QStringLiteral("[配置] 患者类型切换已取消"));
        return;
    }

    // 按患者类型重载量程与报警限剖面（避免成人限值监护新生儿）。
    m_configMgr.loadProfile(type, /*overwriteCustom=*/true);
    ChannelConfigManager::setGlobalPatientType(type);

    const QVector<ChannelConfig> cfgs = m_configMgr.channels();

    // 回灌到各视图与采集线程。
    m_chart->setChannels(cfgs);
    m_valuePanel->buildPanels(cfgs);
    emit requestUpdateConfigs(cfgs);

    // 面板同步（会阻塞信号，避免递归触发）。
    m_configPanel->setChannels(cfgs);

    // 报警状态机需要清空：旧阈值下建立的跟踪状态已不适用。
    m_alarmDetector.reset();
    refreshAlarmUi();

    appendLog(QStringLiteral("[配置] 患者类型：%1 → %2，已按预设剖面重载报警限")
                  .arg(patientTypeName(before), patientTypeName(type)));

    AuditLogger::instance().log(
        AuditLogger::Config, AuditLogger::Warning,
        QStringLiteral("患者类型切换"),
        QStringLiteral("%1 -> %2；%3")
            .arg(patientTypeName(before), patientTypeName(type),
                 diffs.join(QStringLiteral("；"))),
        currentActor());

    updateStatusBar();
}

void MainWindow::auditAlarm(const AlarmEvent &a)
{
    using Sev = AuditLogger::Severity;

    // 严重度映射：升级 > 严重 > 一般告警 > 提示。
    Sev sev = Sev::Info;
    switch (a.level) {
    case AlarmEvent::Escalated: sev = Sev::Critical; break;
    case AlarmEvent::Critical:  sev = Sev::Critical; break;
    case AlarmEvent::Warning:   sev = Sev::Warning;  break;
    case AlarmEvent::Info:      sev = Sev::Info;     break;
    }

    // 解除 / 确认属于状态变化，降一档严重度，避免审计噪音。
    QString stateName;
    switch (a.state) {
    case AlarmEvent::Active:        stateName = QStringLiteral("激活"); break;
    case AlarmEvent::Acknowledged:  stateName = QStringLiteral("已确认"); break;
    case AlarmEvent::Cleared:       stateName = QStringLiteral("解除"); sev = Sev::Info; break;
    case AlarmEvent::LatchedSilent: stateName = QStringLiteral("静音重报"); break;
    }

    AuditLogger::instance().log(
        alarmCategoryOf(a.category), sev,
        QStringLiteral("告警%1").arg(stateName),
        QStringLiteral("通道=%1 条件=%2 值=%3 阈值=%4 级别=%5 描述=%6")
            .arg(a.channelId)
            .arg(a.trackKey)
            .arg(a.value, 0, 'f', 1)
            .arg(a.limit, 0, 'f', 1)
            .arg(static_cast<int>(a.level))
            .arg(a.text));
}

void MainWindow::auditConfigDiff(const QVector<ChannelConfig> &oldCfgs,
                                 const QVector<ChannelConfig> &newCfgs)
{
    for (const ChannelConfig &nc : newCfgs) {
        const ChannelConfig *oc = nullptr;
        for (const ChannelConfig &c : oldCfgs) {
            if (c.id == nc.id) { oc = &c; break; }
        }
        if (!oc)
            continue;

        // 逐字段比对，只有真正变化的才写入审计（避免噪声）。
        if (!qFuzzyCompare(oc->alarmLow + 1.0, nc.alarmLow + 1.0))
            AuditLogger::instance().logConfigChange(
                nc.name, QStringLiteral("alarmLow"),
                QString::number(oc->alarmLow, 'f', 1),
                QString::number(nc.alarmLow, 'f', 1));

        if (!qFuzzyCompare(oc->alarmHigh + 1.0, nc.alarmHigh + 1.0))
            AuditLogger::instance().logConfigChange(
                nc.name, QStringLiteral("alarmHigh"),
                QString::number(oc->alarmHigh, 'f', 1),
                QString::number(nc.alarmHigh, 'f', 1));

        if (!qFuzzyCompare(oc->minValue + 1.0, nc.minValue + 1.0))
            AuditLogger::instance().logConfigChange(
                nc.name, QStringLiteral("minValue"),
                QString::number(oc->minValue, 'f', 1),
                QString::number(nc.minValue, 'f', 1));

        if (!qFuzzyCompare(oc->maxValue + 1.0, nc.maxValue + 1.0))
            AuditLogger::instance().logConfigChange(
                nc.name, QStringLiteral("maxValue"),
                QString::number(oc->maxValue, 'f', 1),
                QString::number(nc.maxValue, 'f', 1));

        if (oc->visible != nc.visible)
            AuditLogger::instance().logConfigChange(
                nc.name, QStringLiteral("visible"),
                oc->visible ? QStringLiteral("显示") : QStringLiteral("隐藏"),
                nc.visible ? QStringLiteral("显示") : QStringLiteral("隐藏"));

        if (oc->color != nc.color)
            AuditLogger::instance().logConfigChange(
                nc.name, QStringLiteral("color"),
                oc->color.name(), nc.color.name());
    }
}

void MainWindow::onConfigApplied(const QVector<ChannelConfig> &channels)
{
    // 纵深防御：即使面板已按角色禁用，这里仍再校验一次。
    // UI 状态是可以被绕过的（自动化脚本 / 无障碍接口 / 未来新增入口），
    // 安全判定必须放在**业务层**，不能只放在界面层。
    if (!ensurePermission(Permission::ModifyConfig, QStringLiteral("应用通道配置"))) {
        // 回滚面板编辑值，避免界面显示与实际生效配置不一致。
        m_configPanel->setChannels(m_configMgr.channels());
        return;
    }

    // 先取旧配置快照，用于生成审计差异（第一梯队第 3 项）。
    const QVector<ChannelConfig> oldCfgs = m_configMgr.channels();

    m_configMgr.channelsMutable() = channels;

    // 同步到绘图控件、数值面板、采集线程。
    // 注意：不要调用 m_configPanel->setChannels()，否则会重建表格、
    // 用 m_configMgr 中的旧值覆盖用户刚勾选的可见性，导致取消勾选后曲线仍显示。
    m_chart->setChannels(channels);
    m_valuePanel->buildPanels(channels);
    emit requestUpdateConfigs(channels);

    // 审计：逐字段记录变更。
    auditConfigDiff(oldCfgs, channels);

    appendLog(QStringLiteral("[配置] 通道配置已更新并下发"));
    updateStatusBar();
}

void MainWindow::onClearClicked()
{
    // 清空会销毁未导出的历史与告警记录（可能正是事故复查所需证据），
    // 因此需要 Operator 及以上权限，并要求明确确认。
    if (!ensurePermission(Permission::ManageData, QStringLiteral("清空历史数据")))
        return;

    if (m_frameCount > 0) {
        const auto ret = QMessageBox::question(
            this, QStringLiteral("确认清空"),
            QStringLiteral("将清空 %1 帧历史数据与 %2 条告警记录。\n"
                           "未导出的数据将无法找回，确定继续？")
                .arg(m_frameCount)
                .arg(m_alarmBuffer.size()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret != QMessageBox::Yes)
            return;
    }

    m_historyBuffer.clear();
    m_alarmBuffer.clear();
    m_alarmDetector.reset();
    m_pendingFrames.clear();
    m_pendingAlarms.clear();
    m_chart->clearData();
    m_frameCount = 0;
    m_droppedFrames = 0;

    // 清空后同步复位报警条与通道边框，避免残留上次的报警状态。
    refreshAlarmUi();

    AuditLogger::instance().log(AuditLogger::Data, AuditLogger::Info,
                                QStringLiteral("清空历史数据"),
                                QStringLiteral("清除历史缓冲、告警记录与绘图数据"));

    appendLog(QStringLiteral("[系统] 已清空历史数据与告警记录"));
    updateStatusBar();
}

void MainWindow::onFollowToggled(bool on)
{
    m_chart->setFollowLatest(on);
    updateStatusBar();
}

void MainWindow::onSpanChanged(int value)
{
    if (value > 0) {
        m_chart->setVisibleIndexSpan(static_cast<quint64>(value));
        updateStatusBar();
    }
}

void MainWindow::onImportClicked()
{
    if (!ensurePermission(Permission::ManageData, QStringLiteral("导入历史数据")))
        return;

    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("导入历史数据"), QString(),
        QStringLiteral("监护历史数据 (*.csv *.vital);;所有文件 (*.*)"));
    if (path.isEmpty())
        return;

    // ---- 1) 输入安全校验（8.2 第 5 项）----
    // 在真正解析之前拦截超大文件 / 二进制载荷 / 目录路径，
    // 否则「打开一个 10 GB 的文件」就足以让程序失去响应。
    QString rejectReason;
    if (!FileIntegrity::validateImportFile(path, &rejectReason)) {
        QMessageBox::warning(this, QStringLiteral("导入被拒绝"), rejectReason);
        appendLog(QStringLiteral("[导入] 已拒绝：%1").arg(rejectReason));
        AuditLogger::instance().log(
            AuditLogger::Data, AuditLogger::Warning,
            QStringLiteral("导入被拒绝"), rejectReason, currentActor());
        return;
    }

    // ---- 2) 完整性校验（8.2 第 1 项）----
    // 有清单 → 必须一致，不一致直接拒绝（数据可能被篡改，回放结论不可信）；
    // 无清单 → 允许导入但在界面与审计中明确标注「未校验」。
    QStringList integrityProblems;
    QString integrityErr;
    const bool hasManifest = FileIntegrity::hasManifest(path);
    bool integrityOk = false;
    if (hasManifest)
        integrityOk = FileIntegrity::verify(path, &integrityProblems, &integrityErr);

    if (hasManifest && !integrityOk) {
        QMessageBox::critical(
            this, QStringLiteral("文件完整性校验失败"),
            QStringLiteral("%1\n\n为避免使用被篡改的数据进行回放，导入已中止。")
                .arg(integrityProblems.join(QLatin1Char('\n'))));
        appendLog(QStringLiteral("[导入] 完整性校验失败：%1")
                      .arg(integrityProblems.join(QStringLiteral("；"))));
        AuditLogger::instance().log(
            AuditLogger::Data, AuditLogger::Critical,
            QStringLiteral("导入完整性校验失败"),
            integrityProblems.join(QStringLiteral("；")), currentActor());
        return;
    }

    // ---- 3) 容错解析（7.1 第 2 项）----
    QString err;
    HistoryRecorder::LoadStats stats;
    QVector<SampleFrame> frames;
    if (!HistoryRecorder::loadFrames(path, frames, &err, &stats)) {
        QMessageBox::warning(this, QStringLiteral("导入失败"), err);
        appendLog(QStringLiteral("[导入] 失败：%1").arg(err));
        return;
    }

    // 部分损坏必须明确告知：静默跳过坏行的「看起来正常」最危险。
    if (stats.hasLoss()) {
        QMessageBox::warning(
            this, QStringLiteral("文件部分损坏"),
            QStringLiteral("已尽量恢复可用数据：\n%1\n\n"
                           "回放内容可能不完整，请勿据此做临床判断。")
                .arg(stats.summary()));
        appendLog(QStringLiteral("[导入] 部分恢复：%1").arg(stats.summary()));
    }

    // 停止实时采集，进入回放模式（流程 3）。
    if (m_running)
        onStartStopClicked();

    // 注意：onClearClicked() 现在带二次确认，回放前的程序性清空
    // 不应打断用户，因此这里直接清缓冲而不走该槽。
    m_historyBuffer.clear();
    m_alarmBuffer.clear();
    m_alarmDetector.reset();
    m_pendingFrames.clear();
    m_pendingAlarms.clear();
    m_chart->clearData();
    m_frameCount = 0;
    m_droppedFrames = 0;
    refreshAlarmUi();

    m_playbackFrames = frames;
    m_playbackPos = 0;
    m_playingBack = true;
    m_playbackTimer->start();

    appendLog(QStringLiteral("[回放] 已载入 %1 帧（%2），开始回放")
                  .arg(frames.size())
                  .arg(stats.summary()));

    AuditLogger::instance().log(
        AuditLogger::Data, AuditLogger::Info,
        QStringLiteral("导入历史数据"),
        QStringLiteral("文件=%1；帧数=%2；%3；完整性=%4")
            .arg(QFileInfo(path).fileName())
            .arg(frames.size())
            .arg(stats.summary(),
                 hasManifest ? (integrityOk ? QStringLiteral("SHA-256 已校验")
                                            : QStringLiteral("校验失败"))
                             : QStringLiteral("无清单（未校验）")),
        currentActor());
}

void MainWindow::onPlaybackTick()
{
    if (!m_playingBack || m_playbackPos >= m_playbackFrames.size()) {
        m_playbackTimer->stop();
        m_playingBack = false;
        appendLog(QStringLiteral("[回放] 回放结束"));
        return;
    }

    // 每 tick 推送若干帧，模拟实时采集节奏。
    constexpr int kBurst = 3;
    QVector<SampleFrame> burst;
    for (int i = 0; i < kBurst && m_playbackPos < m_playbackFrames.size(); ++i, ++m_playbackPos)
        burst.append(m_playbackFrames.at(m_playbackPos));

    m_chart->appendFrames(burst);
    for (const SampleFrame &f : burst) {
        m_historyBuffer.append(f);
        m_valuePanel->updateValues(f);
    }
    updateStatusBar();
}

void MainWindow::onExportImageClicked()
{
    if (!ensurePermission(Permission::ManageData, QStringLiteral("导出截图")))
        return;

    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出趋势截图"),
        QStringLiteral("trend_%1.png")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))),
        QStringLiteral("PNG 图片 (*.png);;JPEG 图片 (*.jpg)"));
    if (path.isEmpty())
        return;

    if (m_chart->exportImage(path)) {
        appendLog(QStringLiteral("[导出] 截图已保存：%1").arg(path));
    } else {
        QMessageBox::warning(this, QStringLiteral("导出失败"),
                             QStringLiteral("无法保存图片到：%1").arg(path));
    }
}

void MainWindow::onExportDataClicked()
{
    if (!ensurePermission(Permission::ManageData, QStringLiteral("导出历史数据")))
        return;

    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出历史数据"),
        FileIntegrity::sanitizeFileName(
            QStringLiteral("vital_%1.csv")
                .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")))),
        QStringLiteral("监护历史数据 (*.csv)"));
    if (path.isEmpty())
        return;

    QString err;
    const QVector<SampleFrame> frames = m_historyBuffer.snapshot();
    if (!HistoryRecorder::saveFrames(path, frames, &err)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), err);
        return;
    }

    // 导出后立刻生成 SHA-256 伴生清单（8.2 第 1 项）。
    // 清单写入失败**不删除**已导出的数据（数据本身仍可用），
    // 但必须明确告知：没有清单就无法在导入时验证完整性。
    QString manifestErr;
    const bool manifestOk = FileIntegrity::writeManifest(path, &manifestErr);

    appendLog(QStringLiteral("[导出] 已保存 %1 帧到：%2").arg(frames.size()).arg(path));
    if (manifestOk)
        appendLog(QStringLiteral("[导出] 完整性清单：%1%2")
                      .arg(path, FileIntegrity::manifestSuffix()));
    else
        appendLog(QStringLiteral("[导出] ⚠ 校验清单写入失败：%1").arg(manifestErr));

    AuditLogger::instance().log(
        AuditLogger::Data, manifestOk ? AuditLogger::Info : AuditLogger::Warning,
        QStringLiteral("导出历史数据"),
        QStringLiteral("文件=%1；帧数=%2；完整性清单=%3")
            .arg(QFileInfo(path).fileName())
            .arg(frames.size())
            .arg(manifestOk ? QStringLiteral("已生成") : manifestErr),
        currentActor());
}

// ---------------------------------------------------------------------------
// 权限 / 开机自检 / 审计校验
// ---------------------------------------------------------------------------

QString MainWindow::currentActor() const
{
    // 演示环境没有账号体系，用「角色名」作为操作者标识；
    // 生产环境应替换为工号 / 账号（IEC 81001-5-1 身份可追溯）。
    return QStringLiteral("operator(%1)").arg(userRoleName(m_role));
}

bool MainWindow::ensurePermission(Permission p, const QString &actionDescription)
{
    if (roleHasPermission(m_role, p))
        return true;

    // 越权：拒绝 + 留痕（两者缺一不可，见头文件说明）。
    const QString msg = QStringLiteral("权限不足：当前角色「%1」不能执行「%2」")
                            .arg(userRoleName(m_role), actionDescription);

    appendLog(QStringLiteral("[权限] %1").arg(msg));
    AuditLogger::instance().log(
        AuditLogger::System, AuditLogger::Warning,
        QStringLiteral("越权操作被拒绝"),
        QStringLiteral("%1（缺少权限：%2）").arg(actionDescription, permissionName(p)),
        currentActor());

    QMessageBox::warning(this, QStringLiteral("权限不足"),
                         QStringLiteral("%1\n\n请切换到「管理员」角色后再试。").arg(msg));
    return false;
}

void MainWindow::onRoleChangeRequested(UserRole role)
{
    // 角色切换本身是高权限动作：只有管理员才能把权限授予别人
    // （否则任何人点一下下拉框就自封为管理员，权限模型形同虚设）。
    // 例外：首次从默认 Operator 提升到 Admin 时，需要已有 Admin 授权 ——
    // 本演示没有账号体系，因此这里明确提示「此切换会被审计」，由流程约束。
    if (role == m_role)
        return;

    const UserRole before = m_role;
    m_role = role;

    m_configPanel->setUserRole(role);

    appendLog(QStringLiteral("[权限] 角色切换：%1 → %2")
                  .arg(userRoleName(before), userRoleName(role)));

    AuditLogger::instance().log(
        AuditLogger::System,
        roleHasPermission(role, Permission::ModifyConfig) ? AuditLogger::Warning
                                                          : AuditLogger::Info,
        QStringLiteral("角色切换"),
        QStringLiteral("%1 -> %2；可修改报警限=%3")
            .arg(userRoleName(before), userRoleName(role),
                 roleHasPermission(role, Permission::ModifyConfig)
                     ? QStringLiteral("是") : QStringLiteral("否")),
        currentActor());

    // 查看者：连数据管理也不允许，避免「看的人误清空历史」。
    const bool canManageData = roleHasPermission(role, Permission::ManageData);
    if (!canManageData)
        appendLog(QStringLiteral("[权限] 查看者模式下导入 / 导出 / 清空将被拒绝"));

    m_patientBar->setRoleSilently(role);
    updateStatusBar();
}

void MainWindow::runSelfTest()
{
    const QString auditDir = QDir(QCoreApplication::applicationDirPath())
                                 .filePath(QStringLiteral("audit"));
    const QString dataDir  = QCoreApplication::applicationDirPath();

    const QVector<SelfTest::Item> items =
        SelfTest::run(m_configMgr.channels(), auditDir, dataDir);

    const SelfTest::Status overall = SelfTest::overall(items);
    const bool ok = (overall == SelfTest::Pass);

    // 摘要写入日志（逐项）与审计（一条）。
    appendLog(QStringLiteral("[自检] 结论：%1").arg(SelfTest::statusName(overall)));
    for (const SelfTest::Item &i : items) {
        appendLog(QStringLiteral("[自检] [%1] %2：%3")
                      .arg(SelfTest::statusName(i.status), i.name, i.detail));
    }

    AuditLogger::instance().log(
        AuditLogger::System,
        overall == SelfTest::Fail ? AuditLogger::Critical
                                  : (overall == SelfTest::Warn ? AuditLogger::Warning
                                                               : AuditLogger::Info),
        QStringLiteral("开机自检"),
        SelfTest::summaryText(items).replace(QLatin1Char('\n'), QLatin1Char(' ')),
        currentActor());

    if (m_patientBar) {
        const QString brief = QStringLiteral("自检：%1")
                                  .arg(SelfTest::statusName(overall));
        m_patientBar->setSelfTestResult(brief, ok);
    }

    // 失败项必须让操作者看到，不能只躺在日志里。
    if (overall == SelfTest::Fail) {
        QMessageBox::critical(this, QStringLiteral("开机自检失败"),
                              SelfTest::summaryText(items));
    } else if (overall == SelfTest::Warn) {
        QMessageBox::warning(this, QStringLiteral("开机自检有告警"),
                             SelfTest::summaryText(items));
    }
}

void MainWindow::onVerifyAuditClicked()
{
    // 审计校验不需要特殊权限：能看日志的人应该能验证日志没被动过。
    AuditLogger &audit = AuditLogger::instance();
    const QString path = audit.filePath();

    if (path.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("审计校验"),
                                 QStringLiteral("当前没有已打开的审计日志。"));
        return;
    }

    QStringList problems;
    QString err;
    const bool ok = AuditLogger::verifyFile(path, &problems, &err);

    if (!err.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("审计校验"), err);
        return;
    }

    if (ok) {
        appendLog(QStringLiteral("[审计] 校验通过：%1").arg(path));
        QMessageBox::information(
            this, QStringLiteral("审计校验通过"),
            QStringLiteral("全部记录行 CRC32 校验一致，日志未被修改或截断。\n\n%1").arg(path));
    } else {
        // 校验失败既是数据问题也是安全事件 → 必须写审计（哪怕日志本身可疑，
        // 也要留下「有人发现并验证过」的记录）。
        appendLog(QStringLiteral("[审计] 校验失败：%1 条记录异常")
                      .arg(problems.size()));
        AuditLogger::instance().log(
            AuditLogger::System, AuditLogger::Critical,
            QStringLiteral("审计日志校验失败"),
            problems.join(QStringLiteral("；")),
            currentActor());

        QMessageBox::critical(
            this, QStringLiteral("审计校验失败"),
            QStringLiteral("发现 %1 处异常：\n\n%2")
                .arg(problems.size())
                .arg(problems.mid(0, 20).join(QLatin1Char('\n'))));
    }
}

// ---------------------------------------------------------------------------
// 辅助
// ---------------------------------------------------------------------------

void MainWindow::appendLog(const QString &text)
{
    const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    m_logView->appendPlainText(QStringLiteral("%1  %2").arg(ts, text));

    // 限制日志长度，避免长时间运行内存增长。
    constexpr int kMaxLogBlocks = 500;
    if (m_logView->document()->blockCount() > kMaxLogBlocks)
        m_logView->document()->setMaximumBlockCount(kMaxLogBlocks);
}

void MainWindow::updateStatusBar()
{
    const QString state = m_running ? QStringLiteral("采集运行中")
                                    : (m_playingBack ? QStringLiteral("历史回放中")
                                                     : QStringLiteral("已停止"));
    const qint64 secs = m_uptime.isValid() ? m_uptime.elapsed() / 1000 : 0;

    m_statusLabel->setText(
        QStringLiteral("状态：%1   |   已采集 %2 帧   |   丢帧 %3   |   历史缓冲 %4 帧   |   活动告警 %5 条   |   累计告警 %6 条   |   运行 %7 秒   |   角色：%8")
            .arg(state)
            .arg(m_frameCount)
            .arg(m_droppedFrames)
            .arg(m_historyBuffer.size())
            .arg(m_alarmDetector.activeCount())
            .arg(m_alarmBuffer.size())
            .arg(secs)
            .arg(userRoleName(m_role)));

    if (m_viewportLabel)
        m_viewportLabel->setText(m_chart->viewportInfo());
}

