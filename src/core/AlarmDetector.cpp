#include "AlarmDetector.h"

#include <QDateTime>
#include <QtGlobal>

namespace {

/// 信号质量低于此阈值时，生理报警被抑制、改报技术报警。
constexpr double kLowQualityThreshold = 0.35;

/// 数据丢帧达到此数量时上报技术报警。
constexpr quint64 kDropAlertThreshold = 3;

/// 技术报警最小重复间隔（毫秒），避免刷屏。
constexpr qint64 kTechRepeatMs = 10000;

/// 用于表述方向的字符串。
QString dirText(bool high) { return high ? QStringLiteral("超上限") : QStringLiteral("低于下限"); }

/**
 * @brief 把「通道配置的报警优先级」映射为报警等级。
 *
 * IEC 60601-1-8 要求报警等级由临床优先级决定，而非数值距离：
 *  - 低优先级              → Info
 *  - 中优先级              → Warning
 *  - 高优先级              → Critical
 *  - 突破二级阈值（危及生命）→ 至少 Critical
 *
 * @param priority        通道配置的优先级
 * @param lifeThreatening 是否已突破二级（危及生命）阈值
 */
AlarmEvent::Level levelForPriority(AlarmPriority priority, bool lifeThreatening)
{
    if (lifeThreatening)
        return AlarmEvent::Critical;

    switch (priority) {
    case AlarmPriority::High:   return AlarmEvent::Critical;
    case AlarmPriority::Medium: return AlarmEvent::Warning;
    case AlarmPriority::Low:    return AlarmEvent::Info;
    }
    return AlarmEvent::Warning;
}

} // namespace

AlarmDetector::AlarmDetector() = default;

void AlarmDetector::setDebounceFrames(int frames)
{
    // 兼容旧接口：旧版本以「帧数」表达去抖窗口。
    // 现在延时统一由 ChannelConfig::alarmDelayMs（毫秒）承担，
    // 此接口仅保留签名以避免破坏历史调用方。
    Q_UNUSED(frames);
}

void AlarmDetector::setDefaultSampleIntervalMs(int ms)
{
    m_defaultSampleIntervalMs = qMax(1, ms);
}

AlarmDetector::Track &AlarmDetector::trackFor(const QString &key)
{
    auto it = m_tracks.find(key);
    if (it == m_tracks.end())
        it = m_tracks.insert(key, Track());
    // 键必须随 Track 一起保存：技术报警的键（"1/2/SQI"）无法由
    // AlarmEvent::conditionKey() 反推出来，下游回查持续时长会失败。
    if (it.value().key != key)
        it.value().key = key;
    return it.value();
}

AlarmEvent AlarmDetector::makeEvent(const Track &t, const SampleFrame &frame,
                                    AlarmEvent::State state) const
{
    AlarmEvent e;
    e.sampleIndex = frame.sampleIndex;
    e.timestampMs = frame.timestampMs;
    e.channelId   = t.channelId;
    e.level       = t.level;
    e.category    = t.category;
    e.state       = state;
    e.highLimit   = t.highLimit;
    e.value       = t.lastValue;
    e.limit       = t.limit;
    // 显式传递跟踪键：下游（报警列表 / 审计日志）靠它回查持续时长，
    // 不能用 conditionKey() 反推（技术报警的键不在那个编码空间里）。
    e.trackKey    = t.key;

    // 描述文本按类别 / 方向 / 严重度组装；技术报警单独措辞。
    if (t.category == AlarmEvent::Technical) {
        e.text = t.text;
    } else {
        e.text = QStringLiteral("%1：%2 %3 %4（阈值 %5）")
                     .arg(state == AlarmEvent::Cleared ? QStringLiteral("恢复")
                                                       : QStringLiteral("越限"))
                     .arg(QStringLiteral("通道%1").arg(t.channelId))
                     .arg(t.lastValue, 0, 'f', 1)
                     .arg(t.highLimit ? QStringLiteral("高于上限") : QStringLiteral("低于下限"))
                     .arg(t.limit, 0, 'f', 1);
    }

    // 依据状态补充前缀，便于日志与横幅区分「首次 / 重报 / 升级 / 解除」。
    switch (state) {
    case AlarmEvent::Active:
        break;
    case AlarmEvent::Acknowledged:
        e.text.prepend(QStringLiteral("[已确认] "));
        break;
    case AlarmEvent::Cleared:
        e.text.prepend(QStringLiteral("[解除] "));
        break;
    case AlarmEvent::LatchedSilent:
        e.text.prepend(QStringLiteral("[持续告警] "));
        break;
    }

    if (t.level == AlarmEvent::Escalated)
        e.text.prepend(QStringLiteral("[升级] "));

    return e;
}

bool AlarmDetector::updateCondition(const QString &key,
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
                                    QVector<AlarmEvent> &out)
{
    // 条件从未出现过且本次也不成立 → 无需建立跟踪，直接返回（避免状态膨胀）。
    auto existing = m_tracks.find(key);
    if (existing == m_tracks.end() && !conditionMet)
        return false;

    Track &t = trackFor(key);

    // 首次建立跟踪时补齐不可变字段。
    t.channelId = channelId;
    t.highLimit = highLimit;
    t.category  = category;

    if (!conditionMet) {
        // ---- 条件已不成立 ----
        if (t.active) {
            // 曾经活跃 → 现在解除，产生 Cleared 事件，供界面撤下报警条。
            t.active       = false;
            t.announced    = false;
            t.acknowledged = false;
            t.escalated    = false;
            t.onsetMs      = 0;
            t.escalateAnchorMs = 0;
            t.lastReportMs = nowMs;
            t.lastValue    = value;
            t.text         = QStringLiteral("%1 %2已恢复（当前 %3）")
                                 .arg(textIfNew.section(QStringLiteral(" "), 0, 0),
                                      dirText(highLimit))
                                 .arg(value, 0, 'f', 1);
            out.append(makeEvent(t, frame, AlarmEvent::Cleared));
            return true;
        }
        // 未成立：重置延时计时。
        t.onsetMs = 0;
        t.lastValue = value;
        return false;
    }

    // ---- 条件成立 ----
    t.lastValue = value;
    t.limit     = limit;

    // 1) 尚未激活：做毫秒制延时判定（第一梯队第 4 项）。
    if (!t.active) {
        if (t.onsetMs == 0) {
            t.onsetMs = nowMs;      // 本帧开始计时，不上报
            return false;
        }
        if (nowMs - t.onsetMs < delayMs)
            return false;           // 延时窗口内：抑制瞬时毛刺

        // 延时满足 → 正式激活并首次上报。
        t.active       = true;
        t.announced    = true;
        t.acknowledged = false;
        t.escalated    = false;
        t.level        = levelIfNew;
        t.text         = textIfNew;
        t.lastReportMs = nowMs;
        // 升级倒计时从「首次上报」起算，与重复提示计时相互独立。
        t.escalateAnchorMs = nowMs;
        out.append(makeEvent(t, frame, AlarmEvent::Active));
        return true;
    }

    // 2) 已是活跃告警：以下分支保证「绝不永久静默」。

    // 2a) 数值继续恶化 → 就地升级严重度。
    if (levelIfNew > t.level) {
        t.level        = levelIfNew;
        t.escalated    = true;
        t.text         = textIfNew;
        t.lastReportMs = nowMs;
        out.append(makeEvent(t, frame,
                             t.acknowledged ? AlarmEvent::LatchedSilent
                                            : AlarmEvent::Active));
        return true;
    }

    // 2b) 未确认且超时未处理 → 升级提醒（不可被静默）。
    //     注意：这里用独立的 escalateAnchorMs 计时，不能用 lastReportMs，
    //     否则重复提示会不断刷新计时器而使升级永不发生。
    if (!t.acknowledged && !t.escalated && escalateMs > 0
        && (nowMs - t.escalateAnchorMs) >= escalateMs) {
        t.escalated    = true;
        t.level        = AlarmEvent::Escalated;
        t.text         = QStringLiteral("%1（超时未确认）")
                             .arg(textIfNew.section(QStringLiteral("（"), 0, 0));
        t.lastReportMs = nowMs;
        out.append(makeEvent(t, frame, AlarmEvent::Active));
        return true;
    }

    // 2c) 已确认（静音）但条件仍成立 → 按 repeatMs 再次提醒。
    //     这是修复「只报一次」缺陷的核心：确认不等于永久关闭。
    if (t.acknowledged && repeatMs > 0
        && (nowMs - t.lastReportMs) >= repeatMs) {
        t.lastReportMs = nowMs;
        t.text         = QStringLiteral("%1（仍未恢复）")
                             .arg(textIfNew.section(QStringLiteral("（"), 0, 0));
        out.append(makeEvent(t, frame, AlarmEvent::LatchedSilent));
        return true;
    }

    // 2d) 未确认 → 按 repeatMs 周期重复提示，持续越限持续提示。
    if (!t.acknowledged && repeatMs > 0
        && (nowMs - t.lastReportMs) >= repeatMs) {
        t.lastReportMs = nowMs;
        t.text         = QStringLiteral("%1（持续 %2 秒）")
                             .arg(textIfNew.section(QStringLiteral("（"), 0, 0))
                             .arg((nowMs - t.onsetMs) / 1000);
        out.append(makeEvent(t, frame, AlarmEvent::Active));
        return true;
    }

    // 2e) 本轮无需上报：仅更新内部数值，保持状态机静默但不丢失条件。
    return false;
}

QVector<AlarmEvent> AlarmDetector::detect(const SampleFrame &frame,
                                          const QVector<ChannelConfig> &configs)
{
    QVector<AlarmEvent> events;
    // 统一时基：优先使用单调时钟（见 SampleData::SampleFrame::monotonicMs）。
    const qint64 nowMs = frameTimeMs(frame);
    if (nowMs > 0)
        m_lastTickMs = nowMs;

    for (const ChannelSample &s : frame.channels) {
        // 找到通道在配置中的索引。
        int idx = -1;
        for (int i = 0; i < configs.size(); ++i) {
            if (configs.at(i).id == s.channelId) { idx = i; break; }
        }
        if (idx < 0)
            continue;

        const ChannelConfig &cfg = configs.at(idx);

        // 毫秒制延时（第一梯队第 4 项）：未显式配置时按 3 个采样周期折算。
        const qint64 delayMs = (cfg.alarmDelayMs > 0)
                                   ? cfg.alarmDelayMs
                                   : qMax<qint64>(100, m_defaultSampleIntervalMs * 3);
        const qint64 repeatMs   = qMax<qint64>(0, cfg.alarmRepeatMs);
        const qint64 escalateMs = qMax<qint64>(0, cfg.alarmEscalateMs);

        // 条件键格式必须与 AlarmEvent::conditionKey() 完全一致：
        //   「类别/通道/方向」，例如 "0/2/HI"（生理 / 通道2 / 上限）。
        //
        // 早期此处误写成「通道/类别」（"%1/%2".arg(cfg.id).arg(0)），
        // 导致同一条件在检测器内部与界面侧得到两套键：
        // 日志里出现 "1/0/LO"（实际是通道1 的下限），按通道筛选、
        // 跨模块比对全部错位。键是跨模块契约，顺序不得各写各的。
        const QString baseKey = QStringLiteral("%1/%2")
                                    .arg(static_cast<int>(AlarmEvent::Physiological))
                                    .arg(cfg.id);

        // ---- 信号质量门控（第二梯队第 7 项）----
        // 数值无效或 SQI 过低时，禁止基于该数据做生理判断，改发技术报警。
        const bool poorQuality = s.invalid || (s.quality < kLowQualityThreshold);

        // ---- 生理合理性校验（优化文档 3.2 第 4 项：伪迹标记）----
        // 上层的合理性校验已把「物理上不可能」的跳变标记为 artifact；
        // 这类样本同样不得参与生理报警判断，否则一次运动伪迹就会触发
        // 一条看起来「危及生命」的误报，反而稀释真实报警的可信度。
        //
        // 注意：这里**不裁剪数值**，原始值照常显示与存档，
        // 保证「异常值不被 clamp 静默掩盖」。
        const bool artifact = s.artifact && !s.invalid;

        if (poorQuality || artifact) {
            // 主动解除仍在活跃的生理告警，避免用不可信数据持续报假警。
            updateCondition(baseKey + QStringLiteral("/HI"), false, true, cfg.id,
                            s.value, cfg.alarmHigh, AlarmEvent::Physiological,
                            QString(), AlarmEvent::Warning, nowMs, delayMs,
                            repeatMs, escalateMs, frame, events);
            updateCondition(baseKey + QStringLiteral("/LO"), false, false, cfg.id,
                            s.value, cfg.alarmLow, AlarmEvent::Physiological,
                            QString(), AlarmEvent::Warning, nowMs, delayMs,
                            repeatMs, escalateMs, frame, events);

            // 技术报警：信号质量不足 / 数值无效 / 数据不可信（第二梯队第 9 项）。
            const QString techKey = QStringLiteral("%1/%2/SQI")
                                        .arg(static_cast<int>(AlarmEvent::Technical))
                                        .arg(cfg.id);
            const QString techText = s.invalid
                ? QStringLiteral("%1 测量无效（传感器异常 / 导联脱落）").arg(cfg.name)
                : (artifact
                       ? QStringLiteral("%1 数据不可信：检测到超出变化率的伪迹（%2），已暂停生理报警")
                             .arg(cfg.name)
                             .arg(s.value, 0, 'f', 1)
                       : QStringLiteral("%1 信号质量不足（SQI %2%）")
                             .arg(cfg.name)
                             .arg(qRound(s.quality * 100.0)));

            const auto it = m_lastTechReportMs.constFind(techKey);
            if (it == m_lastTechReportMs.constEnd() || (nowMs - it.value()) >= kTechRepeatMs) {
                m_lastTechReportMs.insert(techKey, nowMs);
                Track &tt = trackFor(techKey);
                tt.active    = true;
                tt.channelId = cfg.id;
                tt.category  = AlarmEvent::Technical;
                tt.highLimit = true;
                tt.level     = AlarmEvent::Warning;
                tt.lastValue = s.value;
                tt.limit     = 0.0;
                tt.text      = techText;
                tt.onsetMs   = tt.onsetMs ? tt.onsetMs : nowMs;
                events.append(makeEvent(tt, frame, AlarmEvent::Active));
            }
            continue;
        }

        // 信号恢复正常 → 解除该通道的技术报警。
        {
            const QString techKey = QStringLiteral("%1/%2/SQI")
                                        .arg(static_cast<int>(AlarmEvent::Technical))
                                        .arg(cfg.id);
            auto tit = m_tracks.find(techKey);
            if (tit != m_tracks.end() && tit.value().active) {
                Track &tt = tit.value();
                tt.active    = false;
                tt.lastValue = s.value;
                tt.text      = QStringLiteral("%1 信号质量已恢复（SQI %2%）")
                                   .arg(cfg.name)
                                   .arg(qRound(s.quality * 100.0));
                events.append(makeEvent(tt, frame, AlarmEvent::Cleared));
                m_lastTechReportMs.remove(techKey);
            }
        }

        // ---- 上限判断 ----
        // 严重度来自「通道显式配置的优先级 + 二级阈值」，
        // 不再用「超出阈值半个量程」折算（优化文档 2.1 第 1 项：
        // 量程由显示设置决定，与临床严重度无关，折算会掩盖真实病情）。
        {
            const bool met = cfg.hasHighAlarm && (s.value > cfg.alarmHigh);
            // 原始值与二级阈值直接比较（不经过 clamp，见 isLifeThreatening）。
            const bool lifeThreatening = met && isLifeThreatening(cfg, true, s.value);
            const AlarmEvent::Level lvl = levelForPriority(cfg.alarmPriority,
                                                           lifeThreatening);
            updateCondition(baseKey + QStringLiteral("/HI"), met, true, cfg.id,
                            s.value, cfg.alarmHigh, AlarmEvent::Physiological,
                            QStringLiteral("%1 超上限：%2 %3（上限 %4）")
                                .arg(cfg.name)
                                .arg(s.value, 0, 'f', 1)
                                .arg(cfg.unit)
                                .arg(cfg.alarmHigh, 0, 'f', 1),
                            lvl, nowMs, delayMs, repeatMs, escalateMs, frame, events);
        }

        // ---- 下限判断 ----
        {
            const bool met = cfg.hasLowAlarm && (s.value < cfg.alarmLow);
            const bool lifeThreatening = met && isLifeThreatening(cfg, false, s.value);
            const AlarmEvent::Level lvl = levelForPriority(cfg.alarmPriority,
                                                           lifeThreatening);
            updateCondition(baseKey + QStringLiteral("/LO"), met, false, cfg.id,
                            s.value, cfg.alarmLow, AlarmEvent::Physiological,
                            QStringLiteral("%1 低于下限：%2 %3（下限 %4）")
                                .arg(cfg.name)
                                .arg(s.value, 0, 'f', 1)
                                .arg(cfg.unit)
                                .arg(cfg.alarmLow, 0, 'f', 1),
                            lvl, nowMs, delayMs, repeatMs, escalateMs, frame, events);
        }
    }

    // ---- 数据完整性技术报警（第二梯队第 8 项）----
    // 丢帧意味着时间轴出现空洞，必须显式告知而非静默忽略。
    //
    // 注意：`frame.droppedFrames` 是**相对上一帧**的增量而非累计值，
    // 因此这里必须自己累加。若逐帧比较单帧增量（每帧仅 1 帧），
    // 稀疏丢帧将永远达不到阈值而不被上报——这正是需要避免的漏报。
    m_accumulatedDrops += frame.droppedFrames;
    if (m_accumulatedDrops >= kDropAlertThreshold) {
        const QString key = QStringLiteral("%1/-1/DROP")
                                .arg(static_cast<int>(AlarmEvent::Technical));
        const auto it = m_lastTechReportMs.constFind(key);
        if (it == m_lastTechReportMs.constEnd() || (nowMs - it.value()) >= kTechRepeatMs) {
            m_lastTechReportMs.insert(key, nowMs);
            Track &t = trackFor(key);
            t.active    = true;
            t.channelId = -1;
            t.category  = AlarmEvent::Technical;
            t.highLimit = true;
            t.level     = AlarmEvent::Warning;
            t.lastValue = static_cast<double>(m_accumulatedDrops);
            t.limit     = 0.0;
            t.onsetMs   = t.onsetMs ? t.onsetMs : nowMs;
            t.text      = QStringLiteral("数据不连续：累计丢失 %1 帧（序号 %2）")
                              .arg(m_accumulatedDrops)
                              .arg(frame.sampleIndex);
            events.append(makeEvent(t, frame, AlarmEvent::Active));
            // 已上报 → 清零重新累计，下一轮再满阈值时再报。
            m_accumulatedDrops = 0;
        }
    }

    return events;
}

QVector<AlarmEvent> AlarmDetector::acknowledge(const QString &conditionKey,
                                               qint64 nowMs)
{
    QVector<AlarmEvent> acked;
    const SampleFrame dummy;   // 确认事件不带新数据，仅承载状态
    // 关键：确认时刻必须与 detect() 使用同一时基，否则 lastReportMs 会
    // 与后续帧的 timestampMs 相差一个量级，导致重复提示 / 升级永不触发。
    // 传 0 时回退到「最近一次 detect() 的时基」，而不是墙钟时间——
    // 墙钟与单调时钟混用正是最初那处时基 Bug 的根源。
    if (nowMs <= 0)
        nowMs = m_lastTickMs;
    if (nowMs <= 0)
        nowMs = QDateTime::currentMSecsSinceEpoch();

    for (auto it = m_tracks.begin(); it != m_tracks.end(); ++it) {
        Track &t = it.value();
        if (!t.active)
            continue;
        if (!conditionKey.isEmpty() && it.key() != conditionKey)
            continue;

        // 幂等：已经确认过的告警不再重复返回。
        // 「确认」是一次事件，不是可反复触发的动作——医护在报警高峰期
        // 连点按钮是常态，若每次点击都写一条审计，审计日志会被同一批
        // 告警淹没（实测连点 7 次 = 21 条重复记录），真正的事件反而被埋掉。
        // 条件仍成立时，重复提示由 updateCondition() 的 repeatMs 负责，
        // 不依赖 acknowledge() 的重复调用。
        if (t.acknowledged)
            continue;

        t.acknowledged = true;
        // 确认后重新计时，避免「刚确认就立刻重复提示」。
        t.lastReportMs = nowMs;
        acked.append(makeEvent(t, dummy, AlarmEvent::Acknowledged));
    }
    return acked;
}

int AlarmDetector::activeCount() const
{
    int n = 0;
    for (auto it = m_tracks.constBegin(); it != m_tracks.constEnd(); ++it)
        if (it.value().active)
            ++n;
    return n;
}

AlarmEvent::Level AlarmDetector::highestPendingLevel() const
{
    AlarmEvent::Level best = AlarmEvent::Info;
    bool found = false;
    for (auto it = m_tracks.constBegin(); it != m_tracks.constEnd(); ++it) {
        const Track &t = it.value();
        if (!t.active || t.acknowledged)
            continue;
        if (!found || t.level > best) {
            best  = t.level;
            found = true;
        }
    }
    return found ? best : AlarmEvent::Info;
}

bool AlarmDetector::hasPendingAlarm() const
{
    for (auto it = m_tracks.constBegin(); it != m_tracks.constEnd(); ++it) {
        const Track &t = it.value();
        if (t.active && !t.acknowledged)
            return true;
    }
    return false;
}

QVector<AlarmEvent> AlarmDetector::activeAlarms() const
{
    QVector<AlarmEvent> list;
    const SampleFrame dummy;
    for (auto it = m_tracks.constBegin(); it != m_tracks.constEnd(); ++it) {
        const Track &t = it.value();
        if (!t.active)
            continue;
        list.append(makeEvent(t, dummy,
                              t.acknowledged ? AlarmEvent::Acknowledged
                                             : AlarmEvent::Active));
    }
    return list;
}

QHash<QString, qint64> AlarmDetector::activeDurationsMs(qint64 nowMs) const
{
    if (nowMs <= 0)
        nowMs = m_lastTickMs;

    QHash<QString, qint64> durations;
    for (auto it = m_tracks.constBegin(); it != m_tracks.constEnd(); ++it) {
        const Track &t = it.value();
        if (!t.active)
            continue;
        // 兜底：onsetMs 理论上在激活时必定 > 0，但若为 0（历史数据 / 状态迁移
        // 遗漏），仍然返回 0 而不是把整条记录漏掉。漏掉会让上层
        // durations.value() 落空，报警列表显示「—」、审计日志出现「?s」，
        // 恰恰在最需要「已持续多久」的时候丢失信息。
        const qint64 onset = (t.onsetMs > 0) ? t.onsetMs : nowMs;
        const qint64 d = nowMs - t.pauseOffsetMs - onset;
        durations.insert(it.key(), qMax<qint64>(0, d));
    }
    return durations;
}

void AlarmDetector::reset()
{
    m_tracks.clear();
    m_lastTechReportMs.clear();
    m_accumulatedDrops = 0;
    m_lastTickMs = 0;
}

