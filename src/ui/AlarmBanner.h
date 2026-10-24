#ifndef ALARMBANNER_H
#define ALARMBANNER_H

#include <QWidget>
#include <QHash>
#include <QVector>

#include "data/SampleData.h"

class QLabel;
class QPushButton;
class QTimer;
class QTableWidget;

/**
 * @brief 报警横幅控件（对应优化文档第一梯队第 1 项：报警声光指示）。
 *
 * 医疗器械要求「报警必须同时提供视觉与听觉指示」，因此本控件同时承担：
 *
 *  - **视觉**：顶部高对比横幅，按严重度切换底色并闪烁（闪烁频率随严重度提高）；
 *  - **听觉**：按严重度以不同节奏发出提示音，并通过静音开关控制；
 *  - **交互**：提供「确认」按钮，对应报警状态机中的 Acknowledged 状态。
 *
 * 设计要点：
 *  - 只负责「呈现 + 提示」，不参与任何告警判定逻辑；
 *  - 通过 setActiveAlarms() 接收当前活动告警集合，自行去重与排序；
 *  - 使用 QApplication::beep() 发声，避免引入多媒体依赖。
 */
class AlarmBanner : public QWidget
{
    Q_OBJECT

public:
    explicit AlarmBanner(QWidget *parent = nullptr);
    ~AlarmBanner() override;

    /**
     * @brief 更新当前活动告警集合（每次收到新告警/解除时调用）。
     *
     * 控件会依据严重度决定闪烁频率与提示音节奏；
     * 传入空列表表示当前无活动告警，控件自动隐藏。
     */
    void setActiveAlarms(const QVector<AlarmEvent> &alarms);

    /// 当前是否有需要提示的告警。
    bool hasAlarm() const { return !m_alarms.isEmpty(); }

    /**
     * @brief 更新各活动告警的持续时长（毫秒），用于报警列表排序与显示
     *        （对应优化文档 9.1 第 2 项「无报警计时与优先级排序」）。
     *
     * 时基由 MainWindow 传入（`AlarmDetector::activeDurationsMs()`），
     * 控件自身不持有时钟 —— 避免出现「禁用 / 阻塞 UI 时计时停走」的偏差。
     */
    void setAlarmDurations(const QHash<QString, qint64> &durationsMs);

    /// 报警列表当前显示的行数（0 表示列表隐藏）。
    int listedAlarmCount() const;

    /// 声音是否处于静音状态。
    bool isMuted() const { return m_muted; }

    /// 设置静音（不影响视觉提示，仅关闭声音）。
    void setMuted(bool muted);

    /**
     * @brief 报警暂停（Pause）—— 对应优化文档 9.2 第 6 项。
     *
     * 与「静音」的区别：
     *  - **静音（Mute）**：只关声音，视觉提示照常，适用于暂时不想听声音；
     *  - **暂停（Pause）**：声音与闪烁全部停止，用于明确的短时操作
     *     （如翻身、更换电极）。
     *
     * @note 规范要点（IEC 60601-1-8）：
     *  1. 暂停必须有**明确时长**，不得无限期静默；
     *  2. 到期**自动恢复**，且恢复时若仍有未处理告警必须重新提示；
     *  3. 暂停状态必须**可见**（横幅显示剩余秒数），否则等同于藏起报警。
     *
     * @param maxSeconds 允许暂停的秒数上限；危急值（LifeThreatening）
     *                   会被强制缩短到 `kCriticalPauseMaxSeconds`。
     * @return 实际生效的暂停秒数；0 表示未暂停（例如当前没有活动告警）。
     */
    int startPause(int maxSeconds = 120);

    /// 提前结束暂停并恢复提示。
    void cancelPause();

    /// 是否处于暂停中。
    bool isPaused() const { return m_paused; }

    /// 当前暂停的剩余秒数（未暂停时为 0）。
    int pauseRemainingSeconds() const;

signals:
    /// 用户点击「确认」按钮，请求确认当前全部活动告警。
    void acknowledgeRequested();

    /// 静音状态变化。
    void mutedChanged(bool muted);

    /// 报警暂停开始 / 结束（用于主窗口的审计记录）。
    /// @param seconds 本次暂停时长（开始时为设定值，结束时为 0）。
    void pauseChanged(bool paused, int seconds);

protected:
    // 自绘条子控件（AlarmStripWidget，定义在 .cpp）需要读取以下状态；
    // 用 friend 而不是新增 public 接口，避免这些内部状态被外部随意改写。
    friend class AlarmStripWidget;

private slots:
    void onBlinkTick();
    void onSoundTick();
    void onPauseTick();

private:
    /// 依据当前告警集合刷新文案与定时器参数。
    void refresh();

    /// 刷新报警列表（按优先级 + 持续时间排序）。
    void refreshList();

    /// 取当前最高严重度（无告警时返回 Info）。
    AlarmEvent::Level topLevel() const;

    /// 危急值允许的最长暂停秒数（规范要求危急报警不可长时间静默）。
    static constexpr int kCriticalPauseMaxSeconds = 30;
    /// 非危急告警允许的最长暂停秒数。
    static constexpr int kNormalPauseMaxSeconds   = 120;

    QVector<AlarmEvent> m_alarms;
    /// 各活动告警的持续时长（conditionKey → ms）。
    QHash<QString, qint64> m_durations;

    QLabel      *m_textLabel = nullptr;
    QPushButton *m_ackBtn    = nullptr;
    QPushButton *m_muteBtn   = nullptr;
    QPushButton *m_pauseBtn  = nullptr;   ///< 暂停 / 恢复切换

    QTimer *m_blinkTimer = nullptr;   ///< 视觉闪烁
    QTimer *m_soundTimer = nullptr;   ///< 声音节奏
    QTimer *m_pauseTimer = nullptr;   ///< 暂停倒计时（秒级）

    /// 活动报警列表（优先级 / 持续时长 / 确认状态）。
    /// 与横幅同处一个控件：报警信息必须**紧贴**声光提示出现，
    /// 否则医护需要「先看横幅、再去另一处找详情」，延误处置。
    QTableWidget *m_list = nullptr;

    bool m_muted     = false;
    bool m_blinkOn   = true;
    bool m_soundOn   = true;

    bool m_paused          = false;
    int  m_pauseRemaining  = 0;   ///< 暂停剩余秒数
};

#endif // ALARMBANNER_H
