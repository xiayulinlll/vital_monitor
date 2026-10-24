#ifndef SAMPLEDATA_H
#define SAMPLEDATA_H

#include <QMetaType>
#include <QString>
#include <QVector>

/**
 * @brief 单通道的一次采样值。
 *
 * 时间戳统一使用采样序号（sampleIndex）表示，便于环形缓冲区与坐标换算。
 * 同时保留 QDateTime 的毫秒时间，方便回放与文件存档。
 */
struct ChannelSample
{
    int    channelId = -1;   ///< 通道编号（对应 ChannelConfig::id）
    double value     = 0.0;  ///< 生理数值

    /// 信号质量指数 SQI（0.0 ~ 1.0）。
    /// 1.0 = 信号良好；过低时应抑制生理报警，避免基于不可靠数据误报。
    /// @note 本项目由模拟器生成，仅用于演示 SQI 机制。
    double quality   = 1.0;

    /// 该样本是否为无效值（如导联脱落导致的测量失败）。
    bool   invalid   = false;

    /// 生理合理性校验后判定为「伪迹」（运动伪迹 / 传感器瞬变）。
    ///
    /// @note 与 invalid 的区别：invalid 表示**测不到**，artifact 表示
    ///       **测到了但物理上不可能**（如 HR 单帧从 60 跳到 200）。
    ///       伪迹样本不参与告警判断，但原始值仍保留供医护人员查看，
    ///       并且不会被 clamp 静默裁剪 —— 这是「不掩盖病情」的关键。
    bool   artifact  = false;
};

/**
 * @brief 一次采集得到的多通道采样帧（同一时刻所有通道的值）。
 *
 * 采集线程每个采样周期产生一帧，通过信号槽（QueuedConnection）投递到主线程。
 */
struct SampleFrame
{
    quint64 sampleIndex = 0;      ///< 全局递增采样序号（时间轴坐标）
    qint64  timestampMs = 0;      ///< 采集时刻（UTC 毫秒，用于显示与存档）
    QVector<ChannelSample> channels; ///< 本帧内各通道数值

    /// 本帧到达时距上一帧的间隔（毫秒），用于数据完整性校验。
    /// 0 表示首帧或无法判断。
    qint64  deltaMs     = 0;

    /// 相对上一帧丢失的采样帧数（0 表示无丢失）。
    /// 用于把「数据不连续」作为技术报警上报，而非静默忽略。
    quint64 droppedFrames = 0;

    /// 单调时钟时间戳（毫秒，相对采集会话起点）。
    ///
    /// @note 与 timestampMs 的分工（优化文档 3.2 第 2 项「时间戳规范」）：
    ///  - `timestampMs` 来自系统墙钟，可能因 NTP 校时**回拨或跳跃**，
    ///    只用于显示与存档；
    ///  - `monotonicMs` 来自 QElapsedTimer（不受系统时间调整影响），
    ///    **告警延时 / 升级 / 重复提示的计时一律以它为准**。
    /// 这样即使运行期间发生校时，报警去抖与升级时限也不会错乱。
    qint64  monotonicMs = 0;
};

/**
 * @brief 告警事件：某个通道在某个时刻越限。
 */
struct AlarmEvent
{
    enum Level {
        Info = 0,     ///< 提示
        Warning,      ///< 一般告警
        Critical,     ///< 严重告警
        Escalated     ///< 超时未确认，已升级（对应报警状态机）
    };

    /// 告警类别：区分「患者生理报警」与「设备技术报警」。
    /// 两者在医疗监护仪中是独立通道，处理优先级与呈现方式不同。
    enum Category {
        Physiological = 0,  ///< 生理报警（数值越限）
        Technical           ///< 技术报警（信号质量差 / 数据丢失 / 采样异常）
    };

    /// 告警生命周期状态（对应第二梯队：报警状态机）。
    enum State {
        Active = 0,   ///< 活跃中（未确认）
        Acknowledged, ///< 已确认（静音，但仍未回到正常范围）
        Cleared,      ///< 已解除（数值回到正常范围）
        LatchedSilent ///< 已静音但条件仍成立且超过重报周期
    };

    quint64 sampleIndex = 0;   ///< 发生告警时的采样序号（用于在曲线上定位）
    qint64  timestampMs = 0;   ///< 发生时间
    int     channelId   = -1;  ///< 通道编号
    Level   level       = Warning;
    Category category   = Physiological;
    State   state       = Active;
    bool    highLimit   = true;///< true=超上限, false=低于下限
    double  value       = 0.0; ///< 触发时的数值
    double  limit       = 0.0; ///< 被突破的阈值
    QString text;              ///< 告警描述文本

    /**
     * @brief 该事件对应的跟踪键（由 `AlarmDetector` 填入，通常非空）。
     *
     * 取值来自 `AlarmDetector::Track::key`，因此技术报警会是 `"1/2/SQI"`、
     * 数据丢帧会是 `"1/-1/DROP"`，与 `conditionKey()` 生成的生理告警键
     * （`"0/2/HI"`）属于不同的键空间。
     *
     * 需要回查「持续时长」「按条件定位」时**一律用本字段**，
     * 不要用 `conditionKey()` 反推——后者对技术报警会构造出错误的键。
     */
    QString trackKey;

    /// 同一告警条件（同一通道 + 同一方向）的唯一标识，
    /// 用于把「持续越限」归并为一条活动告警而非反复新增。
    ///
    /// @warning 该函数**只描述生理告警**（类别/通道/方向三元组）。
    ///          技术报警的键由 `AlarmDetector` 自行拼接（见 `trackKey`），
    ///          两者不可互相替代。
    QString conditionKey() const
    {
        return QStringLiteral("%1/%2/%3")
            .arg(static_cast<int>(category))
            .arg(channelId)
            .arg(highLimit ? QStringLiteral("HI") : QStringLiteral("LO"));
    }
};

// 允许这些类型通过跨线程信号槽自动排队传递。
Q_DECLARE_METATYPE(ChannelSample)
Q_DECLARE_METATYPE(SampleFrame)
Q_DECLARE_METATYPE(AlarmEvent)

#include <QColor>

/**
 * @brief 取报警等级对应的视觉指示色（IEC 60601-1-8 色码）。
 *
 * IEC 60601-1-8 规定：
 *  - **高优先级（危及生命）**：红色，闪烁提示；
 *  - **中优先级（一般告警）**：黄色 / 琥珀色；
 *  - **低优先级（提示）**：青色 / 蓝绿色。
 *
 * 集中定义可避免各控件（报警条、曲线标记、数值面板）颜色不一致——
 * 旧实现中曲线三角标记使用 `#E74C3C` / `#F39C12`，与报警条不一致，
 * 医护人员可能把同一告警误认为两个不同事件（优化文档 6.1 第 1 项）。
 *
 * @param level 报警等级（Escalated 已升级，按最高优先级处理）
 * @return 该等级的标准指示色
 */
inline QColor alarmIndicatorColor(AlarmEvent::Level level)
{
    switch (level) {
    case AlarmEvent::Escalated:
    case AlarmEvent::Critical: return QColor(0xD3, 0x2F, 0x2F);   // 高优先级：红
    case AlarmEvent::Warning:  return QColor(0xD6, 0x9A, 0x09);   // 中优先级：琥珀
    case AlarmEvent::Info:     return QColor(0x2A, 0x9E, 0xB0);   // 低优先级：青
    }
    return QColor(0x2A, 0x9E, 0xB0);
}

#endif // SAMPLEDATA_H
