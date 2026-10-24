#ifndef HISTORYRECORDER_H
#define HISTORYRECORDER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include "data/SampleData.h"

/**
 * @brief 历史数据存取模块（支撑流程 3 历史回放、功能清单第 6 条）。
 *
 * 文件格式：自定义轻量文本格式（CSV 变体），首行为列头，
 * 每行格式： sampleIndex,timestampMs,monotonicMs,channelId,value,quality,invalid,artifact
 *
 * 之所以不用二进制，是为了便于人工查看与调试；
 * 对医疗回放场景数据量有限，文本格式足够。
 */
class HistoryRecorder
{
public:
    /**
     * @brief 加载结果统计（对应优化文档 7.1 第 2 项「文件损坏无容错」）。
     *
     * 旧实现遇到坏行要么整文件失败、要么静默跳过 —— 前者让「部分可用的
     * 数据」完全无法回放，后者让操作者不知道数据不完整。两者在临床复查
     * 场景下都不可接受，因此这里**按行容错 + 明确上报**。
     */
    struct LoadStats
    {
        int  goodRows        = 0;   ///< 成功解析的数据行
        int  badRows         = 0;   ///< 解析失败 / 字段缺失的行
        int  truncatedBytes  = 0;   ///< 文件末尾截断（非整数行）丢弃的字节
        bool reachedRowLimit = false; ///< 触发行数上限（防超大文件）

        /// 是否发生过任何数据丢失（调用方据此给出「部分恢复」提示）。
        bool hasLoss() const
        {
            return badRows > 0 || truncatedBytes > 0 || reachedRowLimit;
        }

        /// 供界面直接显示的一行摘要。
        QString summary() const;
    };

    /// 把采样帧序列保存到文件。返回是否成功。
    static bool saveFrames(const QString &filePath,
                           const QVector<SampleFrame> &frames,
                           QString *errorMsg = nullptr);

    /**
     * @brief 从文件加载采样帧序列（容错模式）。
     *
     * @param filePath  文件路径
     * @param outFrames 输出帧序列（按 sampleIndex 合并）
     * @param errorMsg  输出错误（仅在**完全无法使用**时返回 false）
     * @param stats     输出：坏行 / 截断 / 上限统计（可为 nullptr）
     * @param maxRows   行数上限，防止畸形超大文件耗尽内存
     * @return true = 至少解析出一帧可用数据
     */
    static bool loadFrames(const QString &filePath,
                           QVector<SampleFrame> &outFrames,
                           QString *errorMsg = nullptr,
                           LoadStats *stats = nullptr,
                           int maxRows = 5000000);
};

#endif // HISTORYRECORDER_H
