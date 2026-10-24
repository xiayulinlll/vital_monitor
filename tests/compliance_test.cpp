// 单元测试：合规增强模块（文件完整性 / 权限模型 / 历史文件容错 /
// 单位校验 / 开机自检）。
//
// 对应 docs/医疗器材合规优化方向.md 中本轮补齐的条目：
//   8.2-1  导出哈希校验          8.2-2  用户身份与权限模型
//   8.2-5  导入输入校验          7.1-2  文件损坏容错
//   7.1-5  开机自检              9.1-3  单位与量纲校验
//
// 与 alarm_state_test 一样不依赖 GUI；退出码 = 失败用例数。
//
// 运行：
//   cmake --build --preset mingw-debug --target compliance_test
//   build\vscode-mingw-debug\compliance_test.exe
#include "core/ChannelConfig.h"
#include "core/FileIntegrity.h"
#include "core/HistoryRecorder.h"
#include "core/SelfTest.h"
#include "core/UserRole.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace {

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

/// 写一个测试用历史文件（v2 格式）。
bool writeHistory(const QString &path, const QStringList &dataRows)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    f.write("# vital_monitor history v2\n");
    f.write("sampleIndex,timestampMs,monotonicMs,channelId,value,quality,invalid,artifact\n");
    for (const QString &row : dataRows)
        f.write((row + QStringLiteral("\n")).toUtf8());
    f.close();
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// 8.2 第 1 项：导出文件哈希校验
// ---------------------------------------------------------------------------
static void case1_manifest()
{
    qInfo().noquote() << "== 用例 1：SHA-256 伴生清单（导出 → 校验 → 篡改检出）==";

    QTemporaryDir dir;
    if (!dir.isValid()) {
        check(false, "创建临时目录");
        return;
    }
    const QString data = dir.filePath(QStringLiteral("vital_test.csv"));
    if (!writeHistory(data, {QStringLiteral("0,1700000000000,0,0,72.000,1.0,0,0"),
                             QStringLiteral("1,1700000000100,100,0,73.000,1.0,0,0")})) {
        check(false, "写入测试数据");
        return;
    }

    check(!FileIntegrity::hasManifest(data), "导出后未生成清单前 hasManifest()=false");

    QString err;
    check(FileIntegrity::writeManifest(data, &err), "生成伴生清单");
    check(FileIntegrity::hasManifest(data), "清单文件存在");

    QStringList problems;
    check(FileIntegrity::verify(data, &problems, &err), "未改动时校验通过");

    // 篡改一个数值（模拟「有人改了回放数据」）。
    {
        QFile f(data);
        f.open(QIODevice::ReadOnly | QIODevice::Text);
        QByteArray all = f.readAll();
        f.close();
        all.replace("72.000", "92.000");
        f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate);
        f.write(all);
        f.close();
    }

    problems.clear();
    check(!FileIntegrity::verify(data, &problems, &err), "内容被修改后校验失败");
    check(!problems.isEmpty(), "失败原因可读（供界面显示）");

    // 缺失清单：必须明确「无法确认」，而不是假装通过。
    QFile::remove(data + FileIntegrity::manifestSuffix());
    problems.clear();
    check(!FileIntegrity::verify(data, &problems, &err), "清单缺失时校验不通过");
    check(!problems.isEmpty() && problems.first().contains(QStringLiteral("未找到校验清单")),
          "清单缺失时给出明确说明");
}

// ---------------------------------------------------------------------------
// 8.2 第 5 项：导入输入安全校验
// ---------------------------------------------------------------------------
static void case2_importValidation()
{
    qInfo().noquote() << "== 用例 2：导入输入校验（畸形 / 空 / 超大文件）==";

    QTemporaryDir dir;
    if (!dir.isValid()) {
        check(false, "创建临时目录");
        return;
    }

    QString reason;

    // 1) 不存在的文件
    check(!FileIntegrity::validateImportFile(dir.filePath(QStringLiteral("nope.csv")), &reason),
          "拒绝不存在的文件");

    // 2) 目录不是文件
    check(!FileIntegrity::validateImportFile(dir.path(), &reason), "拒绝目录路径");

    // 3) 空文件
    const QString empty = dir.filePath(QStringLiteral("empty.csv"));
    QFile ef(empty);
    ef.open(QIODevice::WriteOnly);
    ef.close();
    check(!FileIntegrity::validateImportFile(empty, &reason), "拒绝空文件");

    // 4) 含 NUL 字节的「文本」文件（二进制载荷）
    const QString binary = dir.filePath(QStringLiteral("bin.csv"));
    QFile bf(binary);
    bf.open(QIODevice::WriteOnly);
    bf.write("sampleIndex,timestampMs,channelId,value\n");
    bf.write(QByteArray(8, '\0'));
    bf.close();
    check(!FileIntegrity::validateImportFile(binary, &reason), "拒绝含 NUL 字节的文件");

    // 5) 超过大小上限
    const QString big = dir.filePath(QStringLiteral("big.csv"));
    QFile gf(big);
    gf.open(QIODevice::WriteOnly);
    gf.write("x");
    gf.close();
    check(!FileIntegrity::validateImportFile(big, &reason, /*maxBytes=*/0),
          "拒绝超过大小上限的文件");

    // 6) 合法文件通过
    const QString ok = dir.filePath(QStringLiteral("ok.csv"));
    writeHistory(ok, {QStringLiteral("0,1,0,0,70.0,1.0,0,0")});
    check(FileIntegrity::validateImportFile(ok, &reason), "合法文本文件通过校验");

    // 7) 文件名净化（防路径穿越）
    check(!FileIntegrity::sanitizeFileName(QStringLiteral("../../etc/passwd"))
               .contains(QLatin1Char('/')),
          "sanitizeFileName 去除路径分隔符");
    check(FileIntegrity::sanitizeFileName(QStringLiteral("..")) == QStringLiteral("unnamed"),
          "sanitizeFileName 拒绝 .. 目录穿越");
}

// ---------------------------------------------------------------------------
// 8.2 第 2 项：用户身份与权限模型
// ---------------------------------------------------------------------------
static void case3_permissions()
{
    qInfo().noquote() << "== 用例 3：权限模型（查看者 / 操作者 / 管理员）==";

    check(roleHasPermission(UserRole::Viewer, Permission::ViewData), "查看者可查看数据");
    check(!roleHasPermission(UserRole::Viewer, Permission::AcknowledgeAlarm),
          "查看者不能确认告警");
    check(!roleHasPermission(UserRole::Viewer, Permission::ManageData),
          "查看者不能导入导出");
    check(!roleHasPermission(UserRole::Viewer, Permission::ModifyConfig),
          "查看者不能修改报警限");

    check(roleHasPermission(UserRole::Operator, Permission::AcknowledgeAlarm),
          "操作者可确认告警");
    check(roleHasPermission(UserRole::Operator, Permission::ManageData),
          "操作者可管理数据");
    check(!roleHasPermission(UserRole::Operator, Permission::ModifyConfig),
          "操作者不能修改报警限（关键防误用）");

    check(roleHasPermission(UserRole::Admin, Permission::ModifyConfig),
          "管理员可修改报警限");
    check(roleHasPermission(UserRole::Admin, Permission::ManageUsers),
          "管理员可管理用户");
    // 高权限隐含低权限能力
    check(roleHasPermission(UserRole::Admin, Permission::ViewData),
          "管理员隐含查看权限");
}

// ---------------------------------------------------------------------------
// 7.1 第 2 项：历史文件容错解析
// ---------------------------------------------------------------------------
static void case4_historyTolerance()
{
    qInfo().noquote() << "== 用例 4：历史文件损坏容错（坏行跳过 + 明确上报）==";

    QTemporaryDir dir;
    if (!dir.isValid()) {
        check(false, "创建临时目录");
        return;
    }
    const QString path = dir.filePath(QStringLiteral("broken.csv"));

    writeHistory(path, {
        QStringLiteral("0,1700000000000,0,0,72.0,1.0,0,0"),
        QStringLiteral("0,1700000000000,0,1,98.0,1.0,0,0"),
        QStringLiteral("1,1700000000100,100,0,not_a_number,1.0,0,0"),  // 坏：value 非法
        QStringLiteral("2,1700000000200,200,0"),                       // 坏：字段数不足
        QStringLiteral("3,1700000000300,300,0,75.0,0.9,0,1"),          // 好：伪迹标记
    });

    QVector<SampleFrame> frames;
    HistoryRecorder::LoadStats stats;
    QString err;
    const bool ok = HistoryRecorder::loadFrames(path, frames, &err, &stats);

    check(ok, "部分损坏文件仍能载入（不整文件失败）");
    check(stats.goodRows == 3, QStringLiteral("有效行计数正确（%1）").arg(stats.goodRows));
    check(stats.badRows == 2, QStringLiteral("坏行计数正确（%1）").arg(stats.badRows));
    check(stats.hasLoss(), "hasLoss() 反映数据丢失");
    check(stats.summary().contains(QStringLiteral("损坏")), "摘要包含损坏信息（供界面提示）");

    check(frames.size() == 2, QStringLiteral("坏行不产生越界帧（帧数=%1）").arg(frames.size()));

    // 伪迹 / 质量标记必须在回放中保留（v2 的核心价值）。
    bool foundArtifact = false;
    for (const SampleFrame &f : frames)
        for (const ChannelSample &s : f.channels)
            if (s.artifact)
                foundArtifact = true;
    check(foundArtifact, "artifact 标记在载入后保留");

    // 全部是坏行 → 应报失败，而不是返回「0 帧成功」。
    const QString allBad = dir.filePath(QStringLiteral("allbad.csv"));
    writeHistory(allBad, {QStringLiteral("abc"), QStringLiteral("def")});
    QVector<SampleFrame> none;
    HistoryRecorder::LoadStats st2;
    check(!HistoryRecorder::loadFrames(allBad, none, &err, &st2),
          "全是坏行时载入失败并给出原因");
}

// ---------------------------------------------------------------------------
// 9.1 第 3 项：单位与量纲校验
// ---------------------------------------------------------------------------
static void case5_unitValidation()
{
    qInfo().noquote() << "== 用例 5：单位 / 量纲校验（℃ / ℉ 混用风险）==";

    {
        ChannelConfig c;
        c.name = QStringLiteral("体温 Temp");
        c.unit = QStringLiteral("℃");
        c.minValue = 34.0;
        c.maxValue = 42.0;
        check(c.validateUnit() == UnitIssue::Ok, "体温 + ℃ → Ok");
    }
    {
        // 名字是体温、单位却是 bpm：必须判为量纲不符。
        ChannelConfig c;
        c.name = QStringLiteral("体温 Temp");
        c.unit = QStringLiteral("bpm");
        c.minValue = 34.0;
        c.maxValue = 42.0;
        check(c.validateUnit() == UnitIssue::Mismatch, "体温 + bpm → Mismatch");
    }
    {
        // 血氧用 % 但量程到 1000：物理上不可能。
        ChannelConfig c;
        c.name = QStringLiteral("血氧 SpO2");
        c.unit = QStringLiteral("%");
        c.minValue = 0.0;
        c.maxValue = 1000.0;
        check(c.validateUnit() == UnitIssue::NotInRange, "SpO2 量程 0~1000% → NotInRange");
    }
    {
        ChannelConfig c;
        c.name = QStringLiteral("自定义通道");
        c.unit = QStringLiteral("furlong");
        check(c.validateUnit() == UnitIssue::Unknown, "未登记单位 → Unknown");
    }
    {
        // ℉ 是合法单位（人体 ~98.6），不应被误判为不可能。
        ChannelConfig c;
        c.name = QStringLiteral("体温 Temp");
        c.unit = QStringLiteral("℉");
        c.minValue = 90.0;
        c.maxValue = 110.0;
        check(c.validateUnit() == UnitIssue::Ok, "体温 + ℉（90~110）→ Ok");
    }
    check(!isValuePlausibleInUnit(QStringLiteral("bpm"), 1000.0),
          "bpm 下 1000 视为不可能");
}

// ---------------------------------------------------------------------------
// 7.1 第 5 项：开机自检
// ---------------------------------------------------------------------------
static void case6_selfTest()
{
    qInfo().noquote() << "== 用例 6：开机自检（配置 / 缓存 / 文件系统）==";

    QTemporaryDir dir;
    if (!dir.isValid()) {
        check(false, "创建临时目录");
        return;
    }

    ChannelConfigManager mgr;
    QVector<SelfTest::Item> items = SelfTest::run(mgr.channels(), dir.path(), dir.path());
    check(items.size() == 3, "自检包含 3 项检查");
    check(SelfTest::overall(items) == SelfTest::Pass, "默认配置 + 可写目录 → 全部通过");

    // 量程非法属于 Fail（坐标映射会除零）。
    QVector<ChannelConfig> broken = mgr.channels();
    broken[0].maxValue = broken[0].minValue;
    items = SelfTest::run(broken, dir.path(), dir.path());
    check(SelfTest::overall(items) == SelfTest::Fail, "量程非法 → 自检失败");
    check(SelfTest::summaryText(items).contains(QStringLiteral("量程非法")),
          "自检摘要指出具体原因");

    // 报警限落在量程外 → 该告警永不触发（静默失效），至少是 Warn。
    QVector<ChannelConfig> silent = mgr.channels();
    silent[0].hasHighAlarm = true;
    silent[0].alarmHigh = silent[0].maxValue + 50.0;
    items = SelfTest::run(silent, dir.path(), dir.path());
    check(SelfTest::overall(items) >= SelfTest::Warn, "阈值在量程外 → 至少告警");

    // 目录不可写 → Warn（不阻断监护，但审计落盘失效）。
    items = SelfTest::run(mgr.channels(),
                          QStringLiteral("Z:/__no_such_dir__/audit"),
                          dir.path());
    check(SelfTest::overall(items) >= SelfTest::Warn, "审计目录不可用 → 至少告警");
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    case1_manifest();
    case2_importValidation();
    case3_permissions();
    case4_historyTolerance();
    case5_unitValidation();
    case6_selfTest();

    qInfo().noquote() << (g_fail == 0
                              ? QStringLiteral("==== 全部通过 ====")
                              : QStringLiteral("==== 失败 %1 项 ====").arg(g_fail));
    return g_fail;
}
