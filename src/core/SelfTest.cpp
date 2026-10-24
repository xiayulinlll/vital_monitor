#include "SelfTest.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <algorithm>

#include "core/RingBuffer.h"

QString SelfTest::statusName(Status s)
{
    switch (s) {
    case Pass: return QStringLiteral("通过");
    case Warn: return QStringLiteral("告警");
    case Fail: return QStringLiteral("失败");
    }
    return QStringLiteral("未知");
}

SelfTest::Status SelfTest::overall(const QVector<Item> &items)
{
    Status worst = Pass;
    for (const Item &i : items)
        worst = qMax(worst, i.status);
    return worst;
}

SelfTest::Item SelfTest::checkConfig(const QVector<ChannelConfig> &configs)
{
    Item item;
    item.name = QStringLiteral("配置完整性");

    if (configs.isEmpty()) {
        item.status = Fail;
        item.detail = QStringLiteral("通道配置为空：无任何可监护参数");
        return item;
    }

    QStringList issues;
    QSet<int> seenIds;

    for (const ChannelConfig &c : configs) {
        const QString tag = c.name.isEmpty()
                                ? QStringLiteral("通道#%1").arg(c.id)
                                : c.name;

        if (c.id < 0)
            issues.append(QStringLiteral("%1：id 非法").arg(tag));
        if (seenIds.contains(c.id))
            issues.append(QStringLiteral("%1：id=%2 重复，配置会互相覆盖")
                              .arg(tag).arg(c.id));
        seenIds.insert(c.id);

        // 量程必须有效，否则坐标映射会出现除零 / 反向映射。
        if (!(c.maxValue > c.minValue))
            issues.append(QStringLiteral("%1：量程非法（上限 %2 不大于下限 %3）")
                              .arg(tag)
                              .arg(c.maxValue, 0, 'f', 1)
                              .arg(c.minValue, 0, 'f', 1));

        // 一级阈值应落在量程内 —— 否则该告警永远不会触发（静默失效）。
        if (c.hasLowAlarm && c.alarmLow < c.minValue)
            issues.append(QStringLiteral("%1：告警下限 %2 低于量程下限，永不触发")
                              .arg(tag).arg(c.alarmLow, 0, 'f', 1));
        if (c.hasHighAlarm && c.alarmHigh > c.maxValue)
            issues.append(QStringLiteral("%1：告警上限 %2 高于量程上限，永不触发")
                              .arg(tag).arg(c.alarmHigh, 0, 'f', 1));

        // 二级阈值必须比一级阈值更极端，否则「危及生命」判定失真。
        if (c.hasSecondaryLow && c.hasLowAlarm && !(c.alarmLow2 <= c.alarmLow))
            issues.append(QStringLiteral("%1：二级下限应不高于一级下限").arg(tag));
        if (c.hasSecondaryHigh && c.hasHighAlarm && !(c.alarmHigh2 >= c.alarmHigh))
            issues.append(QStringLiteral("%1：二级上限应不低于一级上限").arg(tag));

        // 延时为负会让状态机时间比较失效。
        if (c.alarmDelayMs < 0 || c.alarmRepeatMs < 0 || c.alarmEscalateMs < 0)
            issues.append(QStringLiteral("%1：报警延时 / 重复 / 升级时间存在负值").arg(tag));
    }

    if (issues.isEmpty()) {
        item.status = Pass;
        item.detail = QStringLiteral("%1 个通道配置有效（量程 / 阈值 / 延时）")
                          .arg(configs.size());
    } else {
        // 量程非法属于不可用；其余属于「可用但存在静默失效风险」。
        const bool fatal = issues.size() > 0
                           && std::any_of(issues.cbegin(), issues.cend(),
                                          [](const QString &s) {
                                              return s.contains(QStringLiteral("量程非法"));
                                          });
        item.status = fatal ? Fail : Warn;
        item.detail = issues.join(QStringLiteral("；"));
    }
    return item;
}

SelfTest::Item SelfTest::checkBuffer()
{
    Item item;
    item.name = QStringLiteral("历史缓存");

    RingBuffer<int> probe(4);
    for (int i = 0; i < 6; ++i)      // 故意超容量写入，验证淘汰逻辑
        probe.append(i);

    const QVector<int> snapshot = probe.snapshot();
    const bool ok = (probe.capacity() == 4)
                    && (snapshot.size() == 4)
                    && (snapshot.first() == 2)   // 最旧的两条已被淘汰
                    && (snapshot.last() == 5);

    item.status = ok ? Pass : Fail;
    item.detail = ok
                      ? QStringLiteral("环形缓冲读写正常（容量 %1，超出自动淘汰）")
                            .arg(probe.capacity())
                      : QStringLiteral("环形缓冲读写异常：容量 / 淘汰逻辑不符合预期");
    return item;
}

SelfTest::Item SelfTest::checkFileSystem(const QString &auditDir,
                                         const QString &dataDir)
{
    Item item;
    item.name = QStringLiteral("文件系统（落盘能力）");

    QStringList problems;

    // 审计目录：不存在则尝试创建（mkpath 对已存在路径返回 true）。
    if (!auditDir.isEmpty()) {
        if (!QDir().mkpath(auditDir))
            problems.append(QStringLiteral("审计目录无法创建：%1").arg(auditDir));
        else if (!QFileInfo(auditDir).isWritable())
            problems.append(QStringLiteral("审计目录不可写：%1").arg(auditDir));
    }

    // 导出目录：只需要存在且可写。
    if (!dataDir.isEmpty()) {
        const QFileInfo di(dataDir);
        if (!di.exists())
            problems.append(QStringLiteral("导出目录不存在：%1").arg(dataDir));
        else if (!di.isWritable())
            problems.append(QStringLiteral("导出目录不可写：%1").arg(dataDir));
    }

    if (problems.isEmpty()) {
        item.status = Pass;
        item.detail = QStringLiteral("审计 / 导出目录可写，事件可持久化");
    } else {
        // 落盘失败不会让监护功能停止，但审计追踪失效属于合规风险 → Warn。
        item.status = Warn;
        item.detail = problems.join(QStringLiteral("；"));
    }
    return item;
}

QVector<SelfTest::Item> SelfTest::run(const QVector<ChannelConfig> &configs,
                                      const QString &auditDir,
                                      const QString &dataDir)
{
    QVector<Item> items;
    items.append(checkConfig(configs));
    items.append(checkBuffer());
    items.append(checkFileSystem(auditDir, dataDir));
    return items;
}

QString SelfTest::summaryText(const QVector<Item> &items)
{
    QStringList lines;
    lines.append(QStringLiteral("自检结论：%1")
                     .arg(statusName(overall(items))));
    for (const Item &i : items) {
        lines.append(QStringLiteral("[%1] %2：%3")
                         .arg(statusName(i.status), i.name, i.detail));
    }
    return lines.join(QLatin1Char('\n'));
}
