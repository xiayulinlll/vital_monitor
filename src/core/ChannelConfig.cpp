#include "ChannelConfig.h"

#include <QHash>

QString patientTypeName(PatientType t)
{
    switch (t) {
    case PatientType::Adult:     return QStringLiteral("成人");
    case PatientType::Pediatric: return QStringLiteral("小儿");
    case PatientType::Neonate:   return QStringLiteral("新生儿");
    case PatientType::Custom:    return QStringLiteral("自定义");
    }
    return QStringLiteral("未知");
}

QString alarmPriorityName(AlarmPriority p)
{
    switch (p) {
    case AlarmPriority::Low:    return QStringLiteral("低");
    case AlarmPriority::Medium: return QStringLiteral("中");
    case AlarmPriority::High:   return QStringLiteral("高");
    }
    return QStringLiteral("中");
}

// ---------------------------------------------------------------------------
// 单位与量纲校验（优化文档 9.1 第 3 项）
// ---------------------------------------------------------------------------
namespace {

/// 已知单位 → 物理上可能的数值区间（含留白，只用于识别「明显不可能」）。
struct UnitRange { const char *unit; double lo; double hi; };

const UnitRange kUnitRanges[] = {
    { "bpm",  0.0,    400.0 },   ///< 心率：成人 ~300 已是极限
    { "rpm",  0.0,    120.0 },   ///< 呼吸率
    { "%",    0.0,    100.0 },   ///< 血氧饱和度
    { "℃",    -20.0,  50.0  },   ///< 摄氏度
    { "°C",   -20.0,  50.0  },
    { "C",    -20.0,  50.0  },
    { "℉",    0.0,    130.0 },   ///< 华氏度（人体不可能低于 0 ℉ 量程起点太多）
    { "°F",   0.0,    130.0 },
    { "F",    0.0,    130.0 },
    { "mmHg", 0.0,    300.0 },   ///< 无创血压
    { "mV",   -10.0,  10.0  },   ///< 心电幅度
};

/// 按通道名推断「应该」使用的量纲族（用于检出「体温配 bpm」这类错配）。
/// 返回空串表示无法从名称推断。
QString expectedDimensionOf(const QString &name)
{
    if (name.contains(QStringLiteral("心率")) || name.contains(QStringLiteral("HR"))
        || name.contains(QStringLiteral("脉率")))
        return QStringLiteral("rate");
    if (name.contains(QStringLiteral("呼吸")))
        return QStringLiteral("resp");
    if (name.contains(QStringLiteral("血氧")) || name.contains(QStringLiteral("SpO")))
        return QStringLiteral("percent");
    if (name.contains(QStringLiteral("体温")) || name.contains(QStringLiteral("Temp")))
        return QStringLiteral("temperature");
    if (name.contains(QStringLiteral("血压")) || name.contains(QStringLiteral("NIBP")))
        return QStringLiteral("pressure");
    return QString();
}

QString dimensionOfUnit(const QString &unit)
{
    if (unit == QStringLiteral("bpm") || unit == QStringLiteral("rpm"))
        return QStringLiteral("rate");
    if (unit == QStringLiteral("%"))
        return QStringLiteral("percent");
    if (unit == QStringLiteral("℃") || unit == QStringLiteral("°C")
        || unit == QStringLiteral("C") || unit == QStringLiteral("℉")
        || unit == QStringLiteral("°F") || unit == QStringLiteral("F"))
        return QStringLiteral("temperature");
    if (unit == QStringLiteral("mmHg"))
        return QStringLiteral("pressure");
    if (unit == QStringLiteral("mV"))
        return QStringLiteral("voltage");
    return QString();
}

/// 按 Unicode 码点精确比较单位符号。
///
/// @note 两个坑必须同时避开：
///  1. **不要用 `QLatin1String`** 承载非 ASCII 单位（`℃` / `℉`）：
///     它只取每个字符的低字节，`℃`(UTF-8 E2 84 83) 会变成单字节 `0xE2`
///     （qDebug 显示为 "?"），与 QString 比较永远不相等 —— 实测已复现
///     「体温 + ℃ 被判为未登记单位」；
///  2. **不要用 `Qt::CaseInsensitive`**：Qt 会做 Unicode 大小写折叠，
///     `℃`(U+2103) 是 `°C` 的兼容字符，折叠后与 `"C"` 等价，`℉`(U+2109)
///     同理等价于 `"F"`，摄氏度与华氏度会互相匹配。
/// 单位是符号而非自然语言，精确（区分大小写）比较才是正确语义。
bool unitEquals(const QString &lhs, const char *rhs)
{
    return lhs.compare(QString::fromUtf8(rhs), Qt::CaseSensitive) == 0;
}

} // namespace

QString unitIssueName(UnitIssue i)
{
    switch (i) {
    case UnitIssue::Ok:         return QStringLiteral("单位正常");
    case UnitIssue::Unknown:    return QStringLiteral("单位未登记，无法校验");
    case UnitIssue::NotInRange: return QStringLiteral("量程在该单位下不可能");
    case UnitIssue::Mismatch:   return QStringLiteral("单位与通道量纲不符");
    }
    return QStringLiteral("未知");
}

bool isKnownUnit(const QString &unit)
{
    const QString u = unit.trimmed();
    // 注意：必须精确匹配（原因见 unitEquals 的注释）。
    for (const UnitRange &r : kUnitRanges) {
        if (unitEquals(u, r.unit))
            return true;
    }
    return false;
}

bool isValuePlausibleInUnit(const QString &unit, double value)
{
    const QString u = unit.trimmed();
    for (const UnitRange &r : kUnitRanges) {
        if (unitEquals(u, r.unit))
            return value >= r.lo && value <= r.hi;
    }
    // 未登记单位：不做判断（返回 true，避免误伤自定义单位）。
    return true;
}

UnitIssue ChannelConfig::validateUnit() const
{
    const QString u = unit.trimmed();

    // 1) 空单位 / 未登记单位：无法校验。
    if (u.isEmpty() || !isKnownUnit(u))
        return UnitIssue::Unknown;

    // 2) 单位族与通道名暗示的量纲矛盾（典型：体温通道配 bpm）。
    const QString expected = expectedDimensionOf(name);
    if (!expected.isEmpty()) {
        const QString actual = dimensionOfUnit(u);
        // 心率与呼吸率同属「频率」族，可以互换；其余必须一致。
        if (!actual.isEmpty() && actual != expected)
            return UnitIssue::Mismatch;
    }

    // 3) 量程端点在物理上不可能（如 % 单位量程到 1000）。
    if (!isValuePlausibleInUnit(u, minValue) || !isValuePlausibleInUnit(u, maxValue))
        return UnitIssue::NotInRange;

    return UnitIssue::Ok;
}


ChannelConfigManager::ChannelConfigManager()
{
    loadDefaults();
}

void ChannelConfigManager::loadDefaults()
{
    m_channels.clear();

    // 1) 心率 HR：60~120 bpm 常见，上下限告警
    {
        ChannelConfig c;
        c.id = 0;
        c.name = QStringLiteral("心率 HR");
        c.unit = QStringLiteral("bpm");
        c.color = QColor(0xE7, 0x4C, 0x3C);   // 红
        c.minValue = 40.0;
        c.maxValue = 180.0;
        c.alarmLow = 60.0;
        c.alarmHigh = 110.0;
        c.hasLowAlarm = true;
        c.hasHighAlarm = true;
        // 一般越限为中优先级；低于 50 / 高于 150 危及生命 → 高优先级。
        c.alarmPriority   = AlarmPriority::Medium;
        c.alarmLow2       = 50.0;
        c.alarmHigh2      = 150.0;
        c.hasSecondaryLow  = true;
        c.hasSecondaryHigh = true;
        m_channels.append(c);
    }

    // 2) 血氧 SpO2：95~100%，通常只设下限告警
    {
        ChannelConfig c;
        c.id = 1;
        c.name = QStringLiteral("血氧 SpO2");
        c.unit = QStringLiteral("%");
        c.color = QColor(0x2E, 0x86, 0xDE);   // 蓝
        c.minValue = 85.0;
        c.maxValue = 100.0;
        c.alarmLow = 94.0;
        c.alarmHigh = 100.0;
        c.hasLowAlarm = true;
        c.hasHighAlarm = false;
        // 血氧是最关键的生命参数：一级即高优先级，< 88% 为危及生命。
        c.alarmPriority  = AlarmPriority::High;
        c.alarmLow2      = 88.0;
        c.hasSecondaryLow = true;
        // 报警延时更短。
        c.alarmDelayMs = 500;
        c.alarmEscalateMs = 5000;
        m_channels.append(c);
    }

    // 3) 呼吸率 RR：12~20 次/分
    {
        ChannelConfig c;
        c.id = 2;
        c.name = QStringLiteral("呼吸率 RR");
        c.unit = QStringLiteral("rpm");
        c.color = QColor(0x27, 0xAE, 0x60);   // 绿
        c.minValue = 0.0;
        c.maxValue = 40.0;
        c.alarmLow = 8.0;
        c.alarmHigh = 24.0;
        c.hasLowAlarm = true;
        c.hasHighAlarm = true;
        // 呼吸暂停（< 5）/ 严重急促（> 32）危及生命。
        c.alarmPriority   = AlarmPriority::Medium;
        c.alarmLow2       = 5.0;
        c.alarmHigh2      = 32.0;
        c.hasSecondaryLow  = true;
        c.hasSecondaryHigh = true;
        m_channels.append(c);
    }

    // 4) 体温 Temp：36~37.5 ℃
    {
        ChannelConfig c;
        c.id = 3;
        c.name = QStringLiteral("体温 Temp");
        c.unit = QStringLiteral("℃");
        c.color = QColor(0xF3, 0x9C, 0x12);   // 橙
        c.minValue = 34.0;
        c.maxValue = 42.0;
        c.alarmLow = 35.5;
        c.alarmHigh = 37.5;
        c.hasLowAlarm = true;
        c.hasHighAlarm = true;
        // 体温变化缓慢，一般越限为低优先级；< 35 / > 39 才危及生命。
        c.alarmPriority   = AlarmPriority::Low;
        c.alarmLow2       = 35.0;
        c.alarmHigh2      = 39.0;
        c.hasSecondaryLow  = true;
        c.hasSecondaryHigh = true;
        // 体温变化缓慢，延时放宽以抑制噪声误报。
        c.alarmDelayMs = 3000;
        c.alarmEscalateMs = 20000;
        m_channels.append(c);
    }
}

// ---------------------------------------------------------------------------
// 患者类型报警限剖面
// ---------------------------------------------------------------------------
namespace {

/// 单通道在某患者类型下的量程与报警限。
struct ProfileEntry {
    double minValue;
    double maxValue;
    double alarmLow;
    double alarmHigh;
    bool   hasLow;
    bool   hasHigh;
    /// 报警优先级（IEC 60601-1-8 高 / 中 / 低）。
    AlarmPriority priority;
};

/// (patientType, channelId) -> ProfileEntry
const QHash<QString, ProfileEntry> &profileTable()
{
    // 数据来源参考常见监护仪出厂默认报警限（成人 / 小儿 / 新生儿）。
    // 二级（危及生命）阈值由 loadProfile() 按「一级阈值 + 量程比例」派生，
    // 保证不同患者类型的危险界值随其基线一起平移。
    static const QHash<QString, ProfileEntry> table = {
        // ---- 成人 Adult ----
        { QStringLiteral("0/0"), {  40, 180,  60, 110, true,  true,  AlarmPriority::Medium } }, // HR
        { QStringLiteral("0/1"), {  85, 100,  94, 100, true,  false, AlarmPriority::High   } }, // SpO2
        { QStringLiteral("0/2"), {   0,  40,   8,  24, true,  true,  AlarmPriority::Medium } }, // RR
        { QStringLiteral("0/3"), {  34,  42, 35.5, 37.5, true, true,  AlarmPriority::Low    } }, // Temp

        // ---- 小儿 Pediatric ----
        { QStringLiteral("1/0"), {  40, 200,  70, 150, true,  true,  AlarmPriority::Medium } }, // HR 更快
        { QStringLiteral("1/1"), {  85, 100,  94, 100, true,  false, AlarmPriority::High   } },
        { QStringLiteral("1/2"), {   0,  50,  10,  30, true,  true,  AlarmPriority::Medium } }, // RR 更快
        { QStringLiteral("1/3"), {  34,  42, 35.5, 37.8, true, true,  AlarmPriority::Low    } },

        // ---- 新生儿 Neonate ----
        { QStringLiteral("2/0"), {  40, 240, 100, 180, true,  true,  AlarmPriority::Medium } }, // HR 100~180
        { QStringLiteral("2/1"), {  85, 100,  90, 100, true,  false, AlarmPriority::High   } }, // SpO2 下限 90
        { QStringLiteral("2/2"), {   0,  80,  30,  60, true,  true,  AlarmPriority::Medium } }, // RR 30~60
        { QStringLiteral("2/3"), {  34,  42, 36.0, 37.5, true, true,  AlarmPriority::Low    } },
    };
    return table;
}

} // namespace

void ChannelConfigManager::loadProfile(PatientType type, bool overwriteCustom)
{
    if (type == PatientType::Custom) {
        // 切到「自定义」只更新标记，不改数值。
        m_patientType = type;
        return;
    }

    const QHash<QString, ProfileEntry> &table = profileTable();
    const bool overwrite = overwriteCustom || (m_patientType != PatientType::Custom);

    for (ChannelConfig &c : m_channels) {
        // 用户已手工设定为自定义范围时，默认不覆盖，避免抹掉医生设置。
        if (!overwrite && c.visible == false)
            continue;

        const QString key = QStringLiteral("%1/%2")
                                .arg(static_cast<int>(type))
                                .arg(c.id);
        const auto it = table.constFind(key);
        if (it == table.constEnd())
            continue;

        const ProfileEntry &e = it.value();
        c.minValue    = e.minValue;
        c.maxValue    = e.maxValue;
        c.alarmLow    = e.alarmLow;
        c.alarmHigh   = e.alarmHigh;
        c.hasLowAlarm = e.hasLow;
        c.hasHighAlarm= e.hasHigh;
        c.alarmPriority = e.priority;

        // 二级（危及生命）阈值随患者类型基线一起平移：
        // 取「一级阈值到量程端点距离」的一定比例，保证新生儿与成人的
        // 危险界值随其生理基线同步变化，而不是套用同一绝对数值。
        const double span = qMax(1.0, e.maxValue - e.minValue);
        if (e.hasLow) {
            c.alarmLow2       = qMax(e.minValue, e.alarmLow - span * 0.10);
            c.hasSecondaryLow = true;
        } else {
            c.hasSecondaryLow = false;
        }
        if (e.hasHigh) {
            c.alarmHigh2       = qMin(e.maxValue, e.alarmHigh + span * 0.20);
            c.hasSecondaryHigh = true;
        } else {
            c.hasSecondaryHigh = false;
        }
    }

    m_patientType = type;
}

ChannelConfig ChannelConfigManager::channelAt(int index) const
{
    if (index < 0 || index >= m_channels.size())
        return ChannelConfig();
    return m_channels.at(index);
}

bool ChannelConfigManager::channelById(int id, ChannelConfig &out) const
{
    for (const ChannelConfig &c : m_channels) {
        if (c.id == id) {
            out = c;
            return true;
        }
    }
    return false;
}

void ChannelConfigManager::updateChannel(const ChannelConfig &cfg)
{
    for (ChannelConfig &c : m_channels) {
        if (c.id == cfg.id) {
            c = cfg;
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// 进程级患者类型单点真源
// ---------------------------------------------------------------------------
namespace {
/// 独一无二的静态存储，供两个静态访问器共享读写。
PatientType &globalTypeStorage()
{
    static PatientType s_type = PatientType::Adult;
    return s_type;
}
} // namespace

PatientType ChannelConfigManager::globalPatientType()
{
    return globalTypeStorage();
}

void ChannelConfigManager::setGlobalPatientType(PatientType type)
{
    globalTypeStorage() = type;
}

