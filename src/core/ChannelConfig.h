#ifndef CHANNELCONFIG_H
#define CHANNELCONFIG_H

#include <QColor>
#include <QString>
#include <QVector>

/**
 * @brief 患者类型（对应优化文档第二梯队：患者类型预设）。
 *
 * 不同人群的生理基线不同（如新生儿心率远高于成人），
 * 必须按患者类型加载对应的报警限剖面，否则会产生大量误报。
 */
enum class PatientType {
    Adult = 0,   ///< 成人
    Pediatric,   ///< 小儿
    Neonate,     ///< 新生儿
    Custom       ///< 自定义（手工设定，不随切换覆盖）
};

/// 患者类型显示名（用于 UI 下拉框与审计日志）。
QString patientTypeName(PatientType t);

/**
 * @brief 报警优先级（对应优化文档 2.1 第 1 项：报警优先级语义）。
 *
 * IEC 60601-1-8 要求报警按临床紧急程度分为「高 / 中 / 低」三级，
 * 而非按「超出阈值多少」折算。优先级决定声光报警的颜色、闪烁频率与
 * 提示音节奏，必须由临床定义并写入配置，不能由数值距离推导。
 */
enum class AlarmPriority {
    Low    = 0,   ///< 低优先级（提示性，如体温轻度偏高）
    Medium,       ///< 中优先级（一般告警，默认）
    High          ///< 高优先级（危及生命，如 SpO2 严重下降 / 心率骤停）
};

/// 报警优先级显示名（用于 UI 下拉框与审计日志）。
QString alarmPriorityName(AlarmPriority p);

/**
 * @brief 单位校验结论（对应优化文档 9.1 第 3 项「单位与量纲校验」）。
 *
 * 临床风险：一个「体温」通道如果单位写成 `F` 而数值其实是 ℃，
 * 36.8 会被读成 36.8 ℉（≈ 2.7 ℃，看上去像死亡），或反过来
 * 98.6 ℃ 被当成正常。显示层必须能识别这类矛盾并提示。
 */
enum class UnitIssue {
    Ok = 0,        ///< 单位与生理量纲一致
    Unknown,       ///< 未登记的单位（无法校验，仅提示）
    NotInRange,    ///< 单位合法，但数值不可能出现在该单位下（如 SpO2 显示为 9600 %）
    Mismatch       ///< 单位与通道名称暗示的量纲不一致（如 体温 用 bpm）
};

/// 单位校验结论的显示名。
QString unitIssueName(UnitIssue i);

/// 该单位是否属于已知的生理量纲单位（用于区分「拼错」与「未登记」）。
bool isKnownUnit(const QString &unit);

/// 在给定单位下，该数值是否落在物理 / 生理上可能的范围内。
bool isValuePlausibleInUnit(const QString &unit, double value);

/**
 * @brief 单路生命体征通道的配置（名称 / 颜色 / 量程 / 告警阈值）。
 *
 * 对应架构 2.2 的「通道配置管理器」中的数据模型部分。
 * 采用值语义 + 拷贝，便于跨线程安全传递配置快照。
 */
struct ChannelConfig
{
    int     id          = -1;         ///< 通道编号
    QString name;                     ///< 显示名称，如 "心率(HR)"
    QString unit;                     ///< 单位，如 "bpm"
    QColor  color       = Qt::green;  ///< 曲线颜色
    double  minValue    = 0.0;        ///< 量程下限（原始值）
    double  maxValue    = 100.0;      ///< 量程上限（原始值）
    double  alarmLow    = 0.0;        ///< 告警下限（<= 表示不设下限；用 hasLowAlarm 控制）
    double  alarmHigh   = 100.0;      ///< 告警上限
    bool    hasLowAlarm = false;      ///< 是否启用下限告警
    bool    hasHighAlarm= true;       ///< 是否启用上限告警
    bool    visible     = true;       ///< 是否显示该通道曲线
    double  sampleIntervalMs = 100.0; ///< 该通道采样间隔（模拟用）

    // ---- 报警延时（毫秒制，替代原「帧数」概念）----
    /// 越限持续多久才产生告警（防止瞬时毛刺误报）。
    int     alarmDelayMs   = 1000;
    /// 告警产生后多久未确认升级为更高严重度。
    int     alarmEscalateMs = 10000;
    /// 告警产生后自动重复提示的周期（0 表示不重复）。
    int     alarmRepeatMs   = 5000;

    // ---- 报警优先级与二级阈值（对应优化文档 2.1 第 1、2 项）----
    /// 一般越限（alarmLow / alarmHigh 被突破）时的报警优先级。
    ///
    /// @note 取代旧实现中「超出阈值半个量程即 Critical」的折算方式：
    ///       该方式与临床无关（量程大小由显示设置决定），会掩盖真实
    ///       病情；优先级必须显式配置。
    AlarmPriority alarmPriority = AlarmPriority::Medium;

    /// 危及生命的二级阈值（必须比一级阈值更极端）。
    /// 突破后优先级提升为 High，用于区分「轻度异常」与「危及生命」。
    /// 通过 hasSecondaryLow / hasSecondaryHigh 控制是否启用。
    double  alarmLow2      = 0.0;
    double  alarmHigh2     = 0.0;
    bool    hasSecondaryLow  = false;
    bool    hasSecondaryHigh = false;

    /// 把原始值裁剪到量程范围内。
    ///
    /// @warning 该函数会丢失越限信息，**不可用于告警判断路径**；
    ///          告警判断必须使用未经裁剪的原始值。
    double clamp(double v) const
    {
        if (v < minValue) return minValue;
        if (v > maxValue) return maxValue;
        return v;
    }

    /**
     * @brief 校验当前单位是否与通道的量纲一致（优化文档 9.1 第 3 项）。
     *
     * 判定顺序：
     *  1. 单位为空 / 未登记 → Unknown（无法校验，但不阻断显示）；
     *  2. 单位与通道名暗示的量纲明显矛盾（体温配 bpm）→ Mismatch；
     *  3. 量程在该单位下物理上不可能（如 % 单位量程到 1000）→ NotInRange；
     *  4. 否则 Ok。
     *
     * @note 这是一个**事后校验**而不是强制约束：本项目允许用户自定义单位，
     *       校验结论只用于在界面上打标记 + 写审计，避免「静默的错单位」。
     */
    UnitIssue validateUnit() const;
};

/**
 * @brief 通道配置管理器：集中维护所有通道配置，并提供按 id 查询。
 */
class ChannelConfigManager
{
public:
    ChannelConfigManager();

    /// 载入一套默认的生理参数通道（心率 / 血氧 / 呼吸 / 体温）。
    void loadDefaults();

    /**
     * @brief 按患者类型加载报警限剖面（对应第二梯队：患者类型预设）。
     *
     * 仅覆盖各通道的量程与报警限，保留名称 / 颜色 / 可见性等用户设置。
     * @param type 患者类型
     * @param overwriteCustom 已手工改为 Custom 的通道是否一并覆盖
     */
    void loadProfile(PatientType type, bool overwriteCustom = false);

    /// 当前生效的患者类型。
    PatientType patientType() const { return m_patientType; }

    /**
     * @brief 进程级「当前患者类型」的单点真源。
     *
     * 配置面板 / 报警条等多个控件都需要读取当前患者类型，
     * 但只有 MainWindow 持有 ChannelConfigManager 实例；
     * 通过该静态访问器避免把管理器指针层层下传，同时保证 UI 显示一致。
     */
    static PatientType globalPatientType();
    /// 更新进程级患者类型（由 MainWindow 在加载剖面后调用）。
    static void setGlobalPatientType(PatientType type);

    const QVector<ChannelConfig> &channels() const { return m_channels; }
    QVector<ChannelConfig> &channelsMutable() { return m_channels; }

    int count() const { return m_channels.size(); }

    /// 按索引获取（越界返回默认构造）。
    ChannelConfig channelAt(int index) const;
    /// 按 id 获取；未找到返回 false。
    bool channelById(int id, ChannelConfig &out) const;

    /// 用新配置覆盖对应 id 的通道。
    void updateChannel(const ChannelConfig &cfg);

private:
    QVector<ChannelConfig> m_channels;
    PatientType            m_patientType = PatientType::Adult;
};

#endif // CHANNELCONFIG_H
