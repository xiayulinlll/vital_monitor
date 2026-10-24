#ifndef SELFTEST_H
#define SELFTEST_H

#include <QString>
#include <QVector>

#include "core/ChannelConfig.h"

/**
 * @brief 开机自检（Power-On Self-Test，对应优化文档 7.1 第 5 项）。
 *
 * 医疗器械通常要求启动时完成自检并**明确显示结果**：
 * 医护需要知道「设备现在能不能用」，而不是在故障设备前面猜测。
 *
 * 检查项（全部为只读检查，不产生副作用）：
 *  1. 配置完整性：通道数、id 唯一、量程有效（min < max）、
 *     报警限落在量程内、二级阈值比一级阈值更极端；
 *  2. 缓存：环形缓冲区可正常读写（容量 > 0，读写一致）；
 *  3. 文件系统：审计目录与导出目录可写（落盘能力可用）；
 *  4. 必需功能：告警侦测器可构造、时间基可用。
 *
 * 结果分级：
 *  - Pass：通过；
 *  - Warn：可用但存在风险（如配置不合理、目录不可写 → 事件无法落盘）；
 *  - Fail：不可用（如配置损坏导致量程非法）。
 */
class SelfTest
{
public:
    enum Status { Pass = 0, Warn, Fail };

    struct Item
    {
        QString name;      ///< 检查项名称
        Status  status = Pass;
        QString detail;    ///< 说明（失败原因 / 修正建议）
    };

    /// 单项状态的中文名（用于界面与日志）。
    static QString statusName(Status s);

    /**
     * @brief 执行全部自检项。
     * @param configs    当前通道配置
     * @param auditDir   审计日志目录（用于可写性检查）
     * @param dataDir    数据导出目录（用于可写性检查）
     * @return 检查项列表（顺序固定，便于界面稳定显示）
     */
    static QVector<Item> run(const QVector<ChannelConfig> &configs,
                             const QString &auditDir,
                             const QString &dataDir);

    /// 整体结论：取最差的单项状态。
    static Status overall(const QVector<Item> &items);

    /// 生成一段可写入日志 / 审计的多行摘要。
    static QString summaryText(const QVector<Item> &items);

private:
    static Item checkConfig(const QVector<ChannelConfig> &configs);
    static Item checkBuffer();
    static Item checkFileSystem(const QString &auditDir, const QString &dataDir);
};

#endif // SELFTEST_H
