// 单元测试：用确定性数据验证 AlarmDetector 的状态机行为。
//
// 由 CMake 目标 alarm_state_test 构建（对应优化文档第三梯队第 15 项：
// 自动化测试与 V&V）；不依赖 GUI，退出码 = 失败用例数（0 表示全部通过）。
//
// 用例覆盖：
//   1  持续越限反复上报            2  确认后按 repeatMs 重报
//   3  超时未确认 → 升级            4  毫秒级去抖
//   5  条件恢复 → 解除              6  SQI 门控
//   7  丢帧 → 技术报警              8  优先级语义（非量程折算）
//   9  伪迹门控 + 技术报警清除      10 单调时钟时间基准
//  11  确认与检测共用同一时基        12 技术报警持续时长可查（trackKey）
//  13  条件键格式统一（类别/通道/方向）
//
// 运行：
//   cmake --build --preset mingw-debug --target alarm_state_test
//   build\vscode-mingw-debug\alarm_state_test.exe
#include "core/AlarmDetector.h"
#include "core/ChannelConfig.h"

#include <QCoreApplication>
#include <QDebug>

namespace {

SampleFrame makeFrame(quint64 idx, qint64 tsMs, int ch, double v,
                      double quality = 1.0, quint64 dropped = 0)
{
    SampleFrame f;
    f.sampleIndex = idx;
    f.timestampMs = tsMs;
    f.droppedFrames = dropped;
    ChannelSample s;
    s.channelId = ch;
    s.value = v;
    s.quality = quality;
    f.channels.append(s);
    return f;
}

/// 与 makeFrame 相同，但显式设置单调时钟（告警计时以此为准）。
SampleFrame makeFrameMono(quint64 idx, qint64 monoMs, int ch, double v,
                          double quality = 1.0)
{
    SampleFrame f = makeFrame(idx, /*tsMs=*/1700000000000LL + monoMs, ch, v, quality);
    f.monotonicMs = monoMs;
    return f;
}

int g_fail = 0;

void check(bool cond, const QString &what)
{
    if (!cond) {
        ++g_fail;
        qWarning().noquote() << "  [FAIL]" << what;
    } else {
        qInfo().noquote() << "  [ OK ]" << what;
    }
}

} // namespace

QVector<ChannelConfig> testCfgs()
{
    ChannelConfigManager mgr;
    QVector<ChannelConfig> cfgs = mgr.channels();
    for (ChannelConfig &c : cfgs) {
        if (c.id == 0) {
            c.alarmLow = 60.0;
            c.alarmHigh = 110.0;
            c.hasLowAlarm = true;
            c.hasHighAlarm = true;
            c.alarmDelayMs = 1000;
            c.alarmRepeatMs = 2000;
            c.alarmEscalateMs = 3000;
            // 明确优先级与二级阈值（取代旧的「半量程折算」）。
            c.alarmPriority    = AlarmPriority::Medium;
            c.alarmLow2        = 50.0;
            c.alarmHigh2       = 150.0;
            c.hasSecondaryLow  = true;
            c.hasSecondaryHigh = true;
        }
    }
    return cfgs;
}

static void case1_persistentRepeat()
{
    qInfo().noquote() << "== 用例 1：持续越限必须反复上报（修复“只报一次”）==";
    AlarmDetector det;
    det.setDefaultSampleIntervalMs(100);
    const QVector<ChannelConfig> cfgs = testCfgs();
    qint64 t = 1000;
    int reports = 0;
    for (int i = 0; i < 100; ++i) {
        const auto evts = det.detect(makeFrame(i, t, 0, 150.0), cfgs);
        for (const AlarmEvent &e : evts)
            if (e.channelId == 0 && e.highLimit && e.category == AlarmEvent::Physiological
                && (e.state == AlarmEvent::Active || e.state == AlarmEvent::LatchedSilent))
                ++reports;
        t += 100;
    }
    qInfo().noquote() << "    持续 10s 超限，共上报" << reports << "次";
    check(reports >= 4, "持续越限出现多次上报（>=4）");
    check(det.activeCount() == 1, "活动告警数为 1（未重复新增）");
    check(det.hasPendingAlarm(), "存在待处理告警");
}

static void case2_ackThenRepeat()
{
    qInfo().noquote() << "== 用例 2：确认后条件仍成立 → 按 repeatMs 重报 ==";
    AlarmDetector det;
    det.setDefaultSampleIntervalMs(100);
    const QVector<ChannelConfig> cfgs = testCfgs();
    qint64 t = 1000;
    for (int i = 0; i < 15; ++i) { det.detect(makeFrame(i, t, 0, 150.0), cfgs); t += 100; }
    const int before = det.activeCount();
    // 传入与 detect() 相同的时基（t），否则确认时间与帧时间戳不同源，
    // 会让后续「重复提示」的计时全部失效。
    const auto acked = det.acknowledge(QString(), t);
    check(before == 1 && acked.size() == 1, "确认前有 1 条活动告警且被确认");
    check(!det.hasPendingAlarm(), "确认后无待处理告警（声音停止）");

    int reReports = 0;
    // 确认后推进 5 秒：即使已确认，只要条件仍成立就必须按 repeatMs 再次提示。
    // t 已指向第 16 帧，故循环内每次自增即可覆盖到 t==6500（确认点 +5000）。
    for (int i = 15; i < 65; ++i) {
        const auto evts = det.detect(makeFrame(i, t, 0, 150.0), cfgs);
        for (const AlarmEvent &e : evts)
            if (e.state == AlarmEvent::LatchedSilent) ++reReports;
        t += 100;
    }
    qInfo().noquote() << "    确认后 5s 内重报" << reReports << "次";
    check(reReports >= 1, "确认后条件仍成立会重新提示（非永久静默）");
    check(det.activeCount() == 1, "重报不新增告警条数");
}

static void case3_escalation()
{
    qInfo().noquote() << "== 用例 3：超时未确认 → 升级 ==";
    AlarmDetector det;
    det.setDefaultSampleIntervalMs(100);
    const QVector<ChannelConfig> cfgs = testCfgs();
    qint64 t = 1000;
    bool escalated = false;
    for (int i = 0; i < 100; ++i) {
        const auto evts = det.detect(makeFrame(i, t, 0, 150.0), cfgs);
        for (const AlarmEvent &e : evts)
            if (e.level == AlarmEvent::Escalated) escalated = true;
        t += 100;
    }
    check(escalated, "超时未确认会升级严重度");
}

static void case4_debounce()
{
    qInfo().noquote() << "== 用例 4：延时去抖（毫秒制）抑制瞬时毛刺 ==";
    AlarmDetector det;
    det.setDefaultSampleIntervalMs(100);
    const QVector<ChannelConfig> cfgs = testCfgs();
    qint64 t = 1000;
    int reports = 0;
    for (int i = 0; i < 5; ++i) {
        reports += det.detect(makeFrame(i, t, 0, 150.0), cfgs).size();
        t += 100;
    }
    check(reports == 0, "500ms 毛刺被延时去抖抑制");
    check(det.activeCount() == 0, "毛刺后无活动告警");
}

static void case5_cleared()
{
    qInfo().noquote() << "== 用例 5：条件恢复 → 解除 ==";
    AlarmDetector det;
    det.setDefaultSampleIntervalMs(100);
    const QVector<ChannelConfig> cfgs = testCfgs();
    qint64 t = 1000;
    for (int i = 0; i < 20; ++i) { det.detect(makeFrame(i, t, 0, 150.0), cfgs); t += 100; }
    check(det.activeCount() == 1, "越限期间有 1 条活动告警");
    bool cleared = false;
    for (int i = 20; i < 40; ++i) {
        const auto evts = det.detect(makeFrame(i, t, 0, 80.0), cfgs);
        for (const AlarmEvent &e : evts)
            if (e.state == AlarmEvent::Cleared) cleared = true;
        t += 100;
    }
    check(cleared, "回到正常范围产生解除事件");
    check(det.activeCount() == 0, "解除后活动告警清零");
}

static void case6_sqiGating()
{
    qInfo().noquote() << "== 用例 6：SQI 过低 → 抑制生理报警，改发技术报警 ==";
    AlarmDetector det;
    det.setDefaultSampleIntervalMs(100);
    const QVector<ChannelConfig> cfgs = testCfgs();
    qint64 t = 1000;
    int phys = 0, tech = 0;
    for (int i = 0; i < 30; ++i) {
        const auto evts = det.detect(makeFrame(i, t, 0, 150.0, /*quality=*/0.10), cfgs);
        for (const AlarmEvent &e : evts) {
            if (e.category == AlarmEvent::Technical) ++tech;
            else if (e.channelId == 0 && e.highLimit) ++phys;
        }
        t += 100;
    }
    check(phys == 0, "低 SQI 下不产生生理越限报警");
    check(tech >= 1, "低 SQI 产生技术报警");
}

static void case7_droppedFrames()
{
    qInfo().noquote() << "== 用例 7：丢帧 → 技术报警 ==";
    AlarmDetector det;
    det.setDefaultSampleIntervalMs(100);
    const QVector<ChannelConfig> cfgs = testCfgs();
    qint64 t = 1000;
    int dropReports = 0;
    for (int i = 0; i < 10; ++i) {
        const auto evts = det.detect(makeFrame(i, t, -1, 0.0, 1.0, /*dropped=*/1), cfgs);
        for (const AlarmEvent &e : evts)
            if (e.category == AlarmEvent::Technical && e.channelId == -1) ++dropReports;
        t += 100;
    }
    check(dropReports >= 1, "丢帧累计超阈值后产生技术报警");
}

static void case8_prioritySemantics()
{
    qInfo().noquote() << "== 用例 8：报警优先级来自配置，而非「超出量程一半」==";
    AlarmDetector det;
    det.setDefaultSampleIntervalMs(100);
    const QVector<ChannelConfig> cfgs = testCfgs();

    // 8a) 中优先级 + 未越过二级阈值 → 应为 Warning（不是 Critical）。
    {
        AlarmDetector d;
        d.setDefaultSampleIntervalMs(100);
        qint64 t = 1000;
        AlarmEvent::Level lvl = AlarmEvent::Info;
        for (int i = 0; i < 30; ++i) {
            const auto evts = d.detect(makeFrame(i, t, 0, 120.0), cfgs);
            for (const AlarmEvent &e : evts)
                if (e.state == AlarmEvent::Active && e.channelId == 0 && e.highLimit
                    && e.category == AlarmEvent::Physiological)
                    lvl = e.level;
            t += 100;
        }
        check(lvl == AlarmEvent::Warning, "120 bpm（一级越限）→ Warning 而非 Critical");
    }

    // 8b) 越过二级阈值(>150) → 应为 Critical。
    {
        AlarmDetector d;
        d.setDefaultSampleIntervalMs(100);
        qint64 t = 1000;
        AlarmEvent::Level lvl = AlarmEvent::Info;
        for (int i = 0; i < 30; ++i) {
            const auto evts = d.detect(makeFrame(i, t, 0, 170.0), cfgs);
            for (const AlarmEvent &e : evts)
                if (e.state == AlarmEvent::Active && e.channelId == 0 && e.highLimit
                    && e.category == AlarmEvent::Physiological)
                    lvl = e.level;
            t += 100;
        }
        check(lvl == AlarmEvent::Critical, "170 bpm（越过二级阈值）→ Critical");
    }

    // 8c) 低优先级通道即使超到量程极端，也不应变成 Critical。
    {
        QVector<ChannelConfig> c2 = testCfgs();
        for (ChannelConfig &c : c2)
            if (c.id == 0) {
                c.alarmPriority = AlarmPriority::Low;
                c.hasSecondaryHigh = false;   // 无二级阈值
            }
        AlarmDetector d;
        d.setDefaultSampleIntervalMs(100);
        qint64 t = 1000;
        AlarmEvent::Level lvl = AlarmEvent::Info;
        for (int i = 0; i < 30; ++i) {
            const auto evts = d.detect(makeFrame(i, t, 0, 179.0), c2);
            for (const AlarmEvent &e : evts)
                if (e.state == AlarmEvent::Active && e.channelId == 0 && e.highLimit
                    && e.category == AlarmEvent::Physiological)
                    lvl = e.level;
            t += 100;
        }
        check(lvl == AlarmEvent::Info, "低优先级通道超到量程极端仍为 Info（不折算成 Critical）");
    }
}

static void case9_artifactGating()
{
    qInfo().noquote() << "== 用例 9：伪迹样本不触发生理报警，但产生技术报警 ==";
    const QVector<ChannelConfig> cfgs = testCfgs();

    // 9a) 正常信号下，180 bpm 会立即产生 Critical 生理报警（作为对照组）。
    {
        AlarmDetector d;
        d.setDefaultSampleIntervalMs(100);
        int critical = 0;
        for (int i = 0; i < 20; ++i) {
            for (const AlarmEvent &e : d.detect(makeFrame(i, 1000 + i * 100, 0, 180.0), cfgs))
                if (e.category == AlarmEvent::Physiological && e.level == AlarmEvent::Critical)
                    ++critical;
        }
        check(critical > 0, "对照组：真实 180 bpm 触发 Critical 生理报警");
    }

    // 9b) 同样 180 bpm，但被标记为伪迹 → 不得产生生理报警。
    {
        AlarmDetector d;
        d.setDefaultSampleIntervalMs(100);
        int physio = 0;
        int tech   = 0;
        for (int i = 0; i < 20; ++i) {
            SampleFrame f = makeFrame(i, 1000 + i * 100, 0, 180.0);
            f.channels[0].artifact = true;
            for (const AlarmEvent &e : d.detect(f, cfgs)) {
                if (e.category == AlarmEvent::Physiological)
                    ++physio;
                if (e.category == AlarmEvent::Technical && e.text.contains(QStringLiteral("伪迹")))
                    ++tech;
            }
        }
        check(physio == 0, "伪迹样本不产生生理报警（避免伪迹被当成危重病情）");
        check(tech > 0, "伪迹样本产生技术报警，提示数据不可信");
        // 伪迹持续存在期间，技术报警应保持活动（这正是「不静默」的要求）；
        // 但不能残留任何生理告警。
        int activePhysio = 0;
        for (const AlarmEvent &e : d.activeAlarms())
            if (e.category == AlarmEvent::Physiological)
                ++activePhysio;
        check(activePhysio == 0, "伪迹样本不留下活动生理告警");
    }

    // 9b-2) 伪迹消失 → 技术报警必须被清除，否则会长期霸占报警条。
    {
        AlarmDetector d;
        d.setDefaultSampleIntervalMs(100);
        for (int i = 0; i < 10; ++i) {
            SampleFrame f = makeFrame(i, 1000 + i * 100, 0, 180.0);
            f.channels[0].artifact = true;
            d.detect(f, cfgs);
        }
        check(d.activeCount() == 1, "伪迹期间存在 1 条技术报警");

        bool cleared = false;
        for (int i = 10; i < 20; ++i) {
            for (const AlarmEvent &e : d.detect(makeFrame(i, 1000 + i * 100, 0, 80.0), cfgs))
                if (e.category == AlarmEvent::Technical && e.state == AlarmEvent::Cleared)
                    cleared = true;
        }
        check(cleared, "信号恢复后技术报警被清除");
        check(d.activeCount() == 0, "信号恢复后无活动告警");
    }

    // 9c) 伪迹期间的原始值不得被裁剪：告警判定与显示都应看到 180。
    {
        AlarmDetector d;
        d.setDefaultSampleIntervalMs(100);
        // 即便帧里带着 250（远超量程上限 180），伪迹标记也应让生理报警静默，
        // 而不是先把值裁到 180 再判定（那会丢失"超出量程"这一事实）。
        SampleFrame f = makeFrame(0, 1000, 0, 250.0);
        f.channels[0].artifact = true;
        for (const AlarmEvent &e : d.detect(f, cfgs))
            check(e.category != AlarmEvent::Physiological,
                  "超量程伪迹值（250）不产生生理报警");
    }
}

static void case10_monotonicTimebase()
{
    qInfo().noquote() << "== 用例 10：告警计时使用单调时钟（不受系统校时影响）==";
    const QVector<ChannelConfig> cfgs = testCfgs();

    // 墙钟在运行中回拨 1 小时，但单调时钟正常前进。
    // 期望：去抖仍然按 1 秒生效，重复提示仍然按 2 秒触发。
    AlarmDetector d;
    d.setDefaultSampleIntervalMs(100);

    int reports = 0;
    for (int i = 0; i < 60; ++i) {           // 单调时间 100..6000 ms
        SampleFrame f = makeFrameMono(i, 100 + i * 100, 0, 150.0);
        // 第 30 帧起把墙钟往回拨 1 小时（模拟 NTP 校时）。
        if (i >= 30)
            f.timestampMs -= 3600LL * 1000LL;
        for (const AlarmEvent &e : d.detect(f, cfgs))
            if (e.category == AlarmEvent::Physiological && e.channelId == 0)
                ++reports;
    }
    check(reports > 0, "墙钟回拨后仍按单调时钟正常触发报警");
    check(d.activeCount() == 1, "活动告警数为 1");

    // 持续时长应基于单调时钟（≈ 4.x s），而不是负值或 1 小时。
    const QHash<QString, qint64> dur = d.activeDurationsMs();
    check(!dur.isEmpty(), "activeDurationsMs() 返回持续时长");
    for (auto it = dur.constBegin(); it != dur.constEnd(); ++it) {
        check(it.value() > 0 && it.value() <= 6000,
              QStringLiteral("持续时长合理（%1 ms，未受墙钟回拨影响）").arg(it.value()));
    }
}

static void case11_durationFrozenOnPause()
{
    qInfo().noquote() << "== 用例 11：确认时刻使用与 detect() 相同的时基 ==";
    const QVector<ChannelConfig> cfgs = testCfgs();

    AlarmDetector d;
    d.setDefaultSampleIntervalMs(100);
    // 用单调时钟推进到 5 秒（墙钟是 2023 年的大数）。
    for (int i = 0; i < 50; ++i)
        d.detect(makeFrameMono(i, 100 + i * 100, 0, 150.0), cfgs);

    // 不传时间：acknowledge() 必须回退到内部记录的单调时基，
    // 而不是 QDateTime::currentMSecsSinceEpoch()（否则两者相差数十年，
    // 重复提示 / 升级计时会全部失效）。
    const auto acked = d.acknowledge(QString());
    check(acked.size() == 1, "无参确认成功");

    int reReports = 0;
    for (int i = 50; i < 100; ++i) {         // 再推进 5 秒
        for (const AlarmEvent &e : d.detect(makeFrameMono(i, 100 + i * 100, 0, 150.0), cfgs))
            if (e.state == AlarmEvent::LatchedSilent || e.state == AlarmEvent::Active)
                ++reReports;
    }
    check(reReports > 0, "确认后条件持续 → 按 repeatMs 重新提示（时基一致）");
}

static void case12_technicalAlarmDuration()
{
    qInfo().noquote() << "== 用例 12：技术报警的持续时长必须可查（trackKey 与 conditionKey 不同）==";
    const QVector<ChannelConfig> cfgs = testCfgs();

    // 低 SQI → 技术报警。技术报警在 m_tracks 中的键是 "1/<ch>/SQI"，
    // 而 AlarmEvent::conditionKey() 生成的是 "1/<ch>/HI"（类别/通道/方向）。
    // 若下游用 conditionKey() 回查 activeDurationsMs()，永远查不到，
    // 报警列表显示「—」、审计日志出现「持续时间=?s」。
    AlarmDetector d;
    d.setDefaultSampleIntervalMs(100);

    QVector<AlarmEvent> techEvents;
    for (int i = 0; i < 30; ++i) {
        for (const AlarmEvent &e : d.detect(makeFrameMono(i, 100 + i * 100, 0, 150.0, 0.10), cfgs))
            if (e.category == AlarmEvent::Technical)
                techEvents.append(e);
    }
    check(!techEvents.isEmpty(), "低 SQI 产生技术报警");

    const QHash<QString, qint64> durations = d.activeDurationsMs();
    bool allFound = true;
    for (const AlarmEvent &e : techEvents) {
        if (e.trackKey.isEmpty() || !durations.contains(e.trackKey))
            allFound = false;
    }
    check(allFound, "每条技术报警事件的 trackKey 都能在持续时长表中命中");

    // 反向断言：证明旧实现（用 conditionKey）确实是错的，
    // 避免有人「顺手」把调用方改回去。
    bool anyConditionKeyHit = false;
    for (const AlarmEvent &e : techEvents)
        if (durations.contains(e.conditionKey()))
            anyConditionKeyHit = true;
    check(!anyConditionKeyHit,
          "conditionKey() 不命中技术报警（这正是不能用它回查的原因）");

    // 技术报警确认时同样能拿到时长（审计日志写入用）。
    // 注意：acknowledge() 是幂等的（已确认的不会二次返回），
    // 因此这里只对本次真正被确认的事件做断言。
    const auto acked = d.acknowledge(QString(), /*nowMs=*/100 + 29 * 100);
    check(!acked.isEmpty(), "技术报警可被确认");
    bool ackAllFound = true;
    for (const AlarmEvent &e : acked)
        if (e.trackKey.isEmpty() || !durations.contains(e.trackKey))
            ackAllFound = false;
    check(ackAllFound, "确认后仍能用 trackKey 回查持续时长（审计不再出现 ?s）");

    // 幂等性：紧接着再确认一次，不应再产生任何「新确认」记录，
    // 否则连点按钮会把审计日志刷成同一批告警的副本。
    const auto again = d.acknowledge(QString(), /*nowMs=*/100 + 30 * 100);
    check(again.isEmpty(), "重复确认不产生重复审计记录（幂等）");
    check(d.hasPendingAlarm() == false, "确认后无待处理告警");
}

static void case13_conditionKeyFormat()
{
    qInfo().noquote() << "== 用例 13：条件键格式统一为「类别/通道/方向」（与 conditionKey() 一致）==";
    const QVector<ChannelConfig> cfgs = testCfgs();

    AlarmDetector d;
    d.setDefaultSampleIntervalMs(100);
    // 只在通道 2 越限，通道 0 保持正常：可以检出「类别与通道写反」。
    QVector<AlarmEvent> phys;
    for (int i = 0; i < 20; ++i) {
        SampleFrame f = makeFrameMono(i, 100 + i * 100, 2, 200.0);   // 通道 2 心率上限
        ChannelSample normal;                                        // 通道 0 正常
        normal.channelId = 0;
        normal.value = 80.0;
        normal.quality = 1.0;
        f.channels.append(normal);
        for (const AlarmEvent &e : d.detect(f, cfgs))
            if (e.category == AlarmEvent::Physiological && e.channelId == 2)
                phys.append(e);
    }
    check(!phys.isEmpty(), "通道 2 越限产生生理告警");

    bool allMatch = true;
    for (const AlarmEvent &e : phys) {
        // 检测器内部键必须与对外暴露的 conditionKey() 逐字相同。
        if (e.trackKey != e.conditionKey())
            allMatch = false;
    }
    check(allMatch, "trackKey 与 conditionKey() 完全一致（无两套键空间）");
    if (!phys.isEmpty()) {
        // 「类别/通道/方向」：生理=0、通道=2 → 形如 "0/2/HI"。
        // 若写成「通道/类别」会得到 "2/0/HI"，本断言即失败。
        check(phys.first().conditionKey() == QStringLiteral("0/2/HI"),
              QStringLiteral("键为 \"0/2/HI\"（实际 \"%1\"）").arg(phys.first().conditionKey()));
    }
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    case1_persistentRepeat();
    case2_ackThenRepeat();
    case3_escalation();
    case4_debounce();
    case5_cleared();
    case6_sqiGating();
    case7_droppedFrames();
    case8_prioritySemantics();
    case9_artifactGating();
    case10_monotonicTimebase();
    case11_durationFrozenOnPause();
    case12_technicalAlarmDuration();
    case13_conditionKeyFormat();

    qInfo().noquote() << (g_fail == 0
                              ? QStringLiteral("==== 全部通过 ====")
                              : QStringLiteral("==== 失败 %1 项 ====").arg(g_fail));
    return g_fail;
}
