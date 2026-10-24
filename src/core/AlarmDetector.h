#ifndef ALARMDETECTOR_H
#define ALARMDETECTOR_H

#include <QVector>
#include <QHash>
#include <QString>

#include "data/SampleData.h"
#include "core/ChannelConfig.h"

/**
 * @brief 判断是否已突破配置的二级（危及生命）阈值。
 *
 * 「是否危及生命」的判据独立成函数，原因有两个：
 *  1. `AlarmDetector` 用它决定报警等级；
 *  2. 报警条的**暂停时长上限**也用它（危急值不允许长时间暂停）。
 * 抽成同一个实现，避免两处判断漂移。
 *
 * @param cfg   通道配置
 * @param high  是否判断上限（false 表示下限）
 * @param value 原始数值（**不得**传入 clamp() 后的值，否则越界信息丢失）
 */
inline bool isLifeThreatening(const ChannelConfig &cfg, bool high, double value)
{
    if (high)
        return cfg.hasSecondaryHigh && value > cfg.alarmHigh2;
    return cfg.hasSecondaryLow && value < cfg.alarmLow2;
}

/**
 * @brief 告警判断模块（对应架构 2.2 第 4 点，并按优化文档第一梯队第 2 项重写）。
 *
 * 相较初版的关键修复（原缺陷：`highLatched` 锁存导致「患者持续超阈值只报警一次」）：
 *
 *  1. **持续报警**：越限条件持续成立时，按 `alarmRepeatMs` 周期重复上报，
 *     绝不因为「报过一次」就永久静默，避免漏掉持续恶化的患者。
 *  2. **毫秒制延时**：去抖改用 `alarmDelayMs`（毫秒）而非帧数，
 *     采样率变化时行为一致（第一梯队第 4 项）。
 *  3. **报警状态机**：Active → Acknowledged / Cleared / Escalated / LatchedSilent
 *     （第二梯队第 5 项）。
 *  4. **信号质量门控**：SQI 过低时抑制生理报警并改发技术报警，
 *     避免基于不可靠数据误报（第二梯队第 7 项）。
 *  5. **技术报警通道**：数据丢帧 / 信号不良 / 数值无效独立成类（第二梯队第 9 项）。
 *  6. **优先级语义**：报警等级来自通道显式配置的 `AlarmPriority` 与二级阈值，
 *     不再按「超出阈值半个量程」折算（优化文档 2.1 第 1 项）。
 */
class AlarmDetector
{
public:
    AlarmDetector();

    /**
     * @brief 统一时基：取一帧用于告警计时的毫秒值。
     *
     * @details
     * 计时优先级：`monotonicMs`（单调时钟，不受 NTP 校时影响）> `timestampMs`。
     * `detect()` 内部与 `acknowledge()` 的调用方都应使用本函数取时基，
     * 避免「确认」与「判定」使用不同时间基准导致重报 / 升级计时错乱。
     *
     * @note 测试用例可直接构造带 monotonicMs 的帧来精确推进时间；
     *       若两者都为 0（完全未初始化的帧），返回 0，由调用方兜底。
     */
    static qint64 frameTimeMs(const SampleFrame &frame)
    {
        if (frame.monotonicMs != 0)
            return frame.monotonicMs;
        return frame.timestampMs;
    }

    /**
     * @brief 设置越限去抖判据（兼容旧接口的帧数版本）。
     *
     * @note 该接口保留是为了向后兼容；采样间隔已知时请优先使用
     *       setDefaultSampleIntervalMs() 走毫秒制，避免采样率变化导致
     *       实际延时漂移。此处假定采样间隔为 100 ms。
     */
    void setDebounceFrames(int frames);

    /// 设置默认采样间隔（毫秒），用于在通道未显式配置时换算延时。
    void setDefaultSampleIntervalMs(int ms);

    /**
     * @brief 检测一帧数据。
     * @param frame   采样帧
     * @param configs 当前通道配置
     * @return 本帧产生的告警事件列表（可能为空）
     *
     * 返回的事件既可能是「新产生的告警」，也可能是「重复提示 / 升级 / 解除」，
     * 通过 `AlarmEvent::state` 区分。
     */
    QVector<AlarmEvent> detect(const SampleFrame &frame,
                               const QVector<ChannelConfig> &configs);

    /// 确认（签收）某条活动告警；返回被确认的事件（供审计）。
    /// @param conditionKey 由 AlarmEvent::conditionKey() 生成；
    ///                     传空字符串表示确认全部活动告警。
    /// @param nowMs        当前时间基准（毫秒），必须与 detect() 传入的
    ///                     SampleFrame::timestampMs 使用同一时基，
    ///                     否则后续「重复提示 / 升级」的计时会因时基错乱而失效。
    ///                     传 0 表示使用系统当前时间（仅在无采集数据时使用）。
    QVector<AlarmEvent> acknowledge(const QString &conditionKey = QString(),
                                    qint64 nowMs = 0);

    /// 当前活动（未解除）告警数量。
    int activeCount() const;

    /// 当前处于「未确认且未静音」状态的最高严重度（供报警条 / 声音使用）。
    /// 无告警时返回 AlarmEvent::Info。
    AlarmEvent::Level highestPendingLevel() const;

    /// 是否存在需要引起注意的未确认告警。
    bool hasPendingAlarm() const;

    /// 遍历当前活动告警（含状态），用于界面刷新。
    QVector<AlarmEvent> activeAlarms() const;

    /**
     * @brief 各活动告警的持续时长（毫秒，相对 `nowMs`）。
     *
     * 用于报警列表按「已持续 N 秒」排序显示（优化文档 9.2 第 1 项：
     * 报警列表须显示持续时间）。
     *
     * @param nowMs 与 detect() 相同的时基；传 0 时取内部记录的最新时基。
     * @return key = AlarmEvent::conditionKey()，value = 持续毫秒数。
     */
    QHash<QString, qint64> activeDurationsMs(qint64 nowMs = 0) const;

    /// 清空内部状态（停止采集 / 清空时调用）。
    void reset();

private:
    /// 单条告警条件的跟踪状态。
    struct Track {
        bool    active      = false;  ///< 条件当前是否成立（已越过延时）
        bool    acknowledged= false;  ///< 是否已被人工确认
        bool    escalated   = false;  ///< 是否已升级
        bool    announced   = false;  ///< 是否已经首次上报过
        qint64  onsetMs     = 0;      ///< 条件首次成立的时间（毫秒）
        qint64  lastReportMs= 0;      ///< 上一次上报/重复提示时间
        /// 上一次「升级判定」参考时刻（毫秒）。
        ///
        /// @note 必须与 lastReportMs 分离：若复用同一次重复提示的计时，
        ///       重复周期（如 2 s）短于升级时限（如 3 s）时，计时器会被
        ///       重复提示不断刷新，导致升级**永远无法触发**。
        qint64  escalateAnchorMs = 0;
        /// 报警暂停（Pause）时累计的有效计时偏移（毫秒）。
        ///
        /// @note 暂停期间不应计入「已持续时长」与「升级倒计时」（IEC 60601-1-8
        ///       要求暂停必须有明确时长且到期自动恢复）。恢复时把暂停时长累加
        ///       到该字段，所有计时比较都以 `nowMs - pauseOffsetMs - anchor`
        ///       进行，从而「冻结」而不是「跳过」计时。
        qint64  pauseOffsetMs = 0;
        double  lastValue   = 0.0;    ///< 最近一次数值
        double  limit       = 0.0;    ///< 阈值
        AlarmEvent::Level  level = AlarmEvent::Warning;
        AlarmEvent::Category category = AlarmEvent::Physiological;
        int     channelId   = -1;
        bool    highLimit   = true;
        QString text;                 ///< 首次上报时的描述文本

        /// 该跟踪记录在 `m_tracks` 中的真实键。
        ///
        /// @note 必须显式保存，不能由 `AlarmEvent::conditionKey()` 反推。
        ///       技术报警的键形如 `"1/2/SQI"`、丢帧键为 `"1/-1/DROP"`，
        ///       它们与「类别/通道/方向」三元组**不是同一套编码**；
        ///       而 `AlarmEvent::conditionKey()` 只能生成 `"1/2/HI"` 这类
        ///       生理告警键。早期版本正是靠 `conditionKey()` 反查
        ///       `activeDurationsMs()`，导致技术报警的持续时长永远查不到、
        ///       审计日志里出现「持续时间=?s」。
        QString key;
    };

    /// 取（或创建）指定条件的跟踪记录。
    Track &trackFor(const QString &key);

    /// 构造一条面向外部的 AlarmEvent。
    AlarmEvent makeEvent(const Track &t, const SampleFrame &frame,
                         AlarmEvent::State state) const;

    /// 处理一个「上限越限 / 下限越限 / 技术事件」条件在某一帧的状态迁移。
    /// @return 若本帧需要向外发出事件，则返回 true。
    bool updateCondition(const QString &key,
                         bool conditionMet,
                         bool highLimit,
                         int channelId,
                         double value,
                         double limit,
                         AlarmEvent::Category category,
                         const QString &textIfNew,
                         AlarmEvent::Level levelIfNew,
                         qint64 nowMs,
                         qint64 delayMs,
                         qint64 repeatMs,
                         qint64 escalateMs,
                         const SampleFrame &frame,
                         QVector<AlarmEvent> &out);

    int m_defaultSampleIntervalMs = 100;

    /// 最近一次 detect() 使用的时基（毫秒）。
    ///
    /// 供 `activeDurationsMs(0)` / `acknowledge("")` 在无参数时兜底，
    /// 保证「持续时长」永远与同一时基比较，不会出现负数。
    qint64 m_lastTickMs = 0;

    /// 累计丢帧计数（相对增量需自行累加，见 detect()）。
    quint64 m_accumulatedDrops = 0;

    /// conditionKey -> 跟踪状态
    QHash<QString, Track> m_tracks;

    /// 已上报过的技术报警种类（避免每帧重复刷屏），key 为 conditionKey。
    QHash<QString, qint64> m_lastTechReportMs;
};

#endif // ALARMDETECTOR_H
