#include "HistoryRecorder.h"

#include <QFile>
#include <QStringList>
#include <QTextStream>

bool HistoryRecorder::saveFrames(const QString &filePath,
                                 const QVector<SampleFrame> &frames,
                                 QString *errorMsg)
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (errorMsg)
            *errorMsg = QStringLiteral("无法写入文件：%1").arg(file.errorString());
        return false;
    }

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);

    // ---- 文件格式 v2 ----
    // 相对 v1 的变更：新增 monotonicMs / quality / invalid / artifact 四列。
    //
    // 为什么必须记录这四个字段（v1 的缺陷）：
    //  - v1 只存 value，回放时 invalid/artifact 样本会**变成普普通通的数值**；
    //    一段导联脱落期间被标记为无效的数据，在回放里看起来就像真实读数，
    //    这在复查时会造成误判 —— 属于「丢失了数据可信度」的严重缺陷；
    //  - quality 丢失后无法判断趋势曲线上的点是否可信；
    //  - monotonicMs 丢失后无法还原真实的采样间隔（墙钟可能被校时回拨）。
    //
    // 版本号写在首行注释里，loadFrames() 据此兼容旧文件。
    out << "# vital_monitor history v2\n";
    out << "sampleIndex,timestampMs,monotonicMs,channelId,value,quality,invalid,artifact\n";

    for (const SampleFrame &f : frames) {
        for (const ChannelSample &s : f.channels) {
            out << f.sampleIndex << ','
                << f.timestampMs << ','
                << f.monotonicMs << ','
                << s.channelId << ','
                << QString::number(s.value, 'f', 3) << ','
                << QString::number(s.quality, 'f', 3) << ','
                << (s.invalid ? 1 : 0) << ','
                << (s.artifact ? 1 : 0) << '\n';
        }
    }

    out.flush();
    file.close();
    return true;
}

QString HistoryRecorder::LoadStats::summary() const
{
    if (!hasLoss())
        return QStringLiteral("解析 %1 行，无损坏").arg(goodRows);

    QStringList parts;
    parts.append(QStringLiteral("有效 %1 行").arg(goodRows));
    if (badRows > 0)
        parts.append(QStringLiteral("跳过损坏行 %1 行").arg(badRows));
    if (truncatedBytes > 0)
        parts.append(QStringLiteral("末尾截断 %1 字节").arg(truncatedBytes));
    if (reachedRowLimit)
        parts.append(QStringLiteral("已达行数上限，后续数据未载入"));
    return parts.join(QStringLiteral("，"));
}

bool HistoryRecorder::loadFrames(const QString &filePath,
                                 QVector<SampleFrame> &outFrames,
                                 QString *errorMsg,
                                 LoadStats *stats,
                                 int maxRows)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMsg)
            *errorMsg = QStringLiteral("无法读取文件：%1").arg(file.errorString());
        return false;
    }

    outFrames.clear();
    LoadStats local;
    int rowCount = 0;
    bool stoppedByLimit = false;

    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);
    quint64 currentIndex = 0;
    bool hasCurrent = false;
    SampleFrame current;

    // 列语义取决于文件版本：v2 插入了 monotonicMs 列（第 2 列），
    // 其后所有列下标整体右移。用一个偏移量来表达，避免出现两份解析分支。
    int colOffset = 0;   // v1: 0；v2: +1

    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();

        if (line.isEmpty()) {
            // 空行不计为损坏（文件末尾换行也会产生空串）。
            continue;
        }
        if (line.startsWith('#')) {
            // 首行版本标记：v2 及以后带 monotonicMs 列。
            if (line.contains(QStringLiteral("history v2")))
                colOffset = 1;
            continue;
        }
        if (line.startsWith(QStringLiteral("sampleIndex"), Qt::CaseInsensitive))
            continue; // 表头

        if (rowCount >= maxRows) {
            stoppedByLimit = true;
            break;
        }
        ++rowCount;

        const QStringList parts = line.split(',');
        QString rowProblem;
        if (parts.size() < 4 + colOffset) {
            rowProblem = QStringLiteral("字段数不足");
        }

        bool okIdx = false, okTs = false, okCh = false, okVal = false;
        quint64 idx = 0;
        qint64  ts  = 0;
        int     ch  = -1;
        double  val = 0.0;
        if (rowProblem.isEmpty()) {
            idx = parts.at(0).toULongLong(&okIdx);
            ts  = parts.at(1).toLongLong(&okTs);
            ch  = parts.at(2 + colOffset).toInt(&okCh);
            val = parts.at(3 + colOffset).toDouble(&okVal);
            if (!okIdx) rowProblem = QStringLiteral("sampleIndex 非法");
            else if (!okTs) rowProblem = QStringLiteral("timestampMs 非法");
            else if (!okCh) rowProblem = QStringLiteral("channelId 非法");
            else if (!okVal) rowProblem = QStringLiteral("value 非法（NaN / 文本）");
        }

        if (!rowProblem.isEmpty()) {
            // 关键：单行损坏不再让整个文件失败。跳过该行并计入统计，
            // 由调用方在界面上明确告知「已部分恢复」。
            ++local.badRows;
            continue;
        }

        ++local.goodRows;

        // 同一 sampleIndex 的多行合并为一帧。
        //
        // 注意乱序文件：如果 sampleIndex 回退（而非相等），说明文件被拼接
        // 或排序被破坏，直接新开一帧，避免把后面的数据错误并入旧帧。
        if (!hasCurrent || idx != currentIndex) {
            if (hasCurrent)
                outFrames.append(current);
            current = SampleFrame();
            current.sampleIndex = idx;
            current.timestampMs = ts;
            if (colOffset > 0)
                current.monotonicMs = parts.at(2).toLongLong();
            currentIndex = idx;
            hasCurrent = true;
        }

        ChannelSample s;
        s.channelId = ch;
        s.value = val;

        if (colOffset > 0 && parts.size() >= 8) {
            s.quality  = parts.at(5).toDouble();
            // 关键：恢复「数据是否可信」的标记，否则回放会把无效点当成真实读数。
            s.invalid  = (parts.at(6).toInt() != 0);
            s.artifact = (parts.at(7).toInt() != 0);
        } else {
            // v1 文件不含质量信息：按「未知但可用」处理。
            s.quality = 1.0;
        }

        current.channels.append(s);
    }

    if (hasCurrent)
        outFrames.append(current);

    // 末尾截断检测：非精确文本读取时，最后一行可能没有换行符且内容不完整。
    // QTextStream 不会为此报错，因此单独检查文件是否以换行结尾。
    if (file.isOpen()) {
        const qint64 size = file.size();
        if (size > 0) {
            file.seek(size - 1);
            const QByteArray last = file.read(1);
            if (last != "\n" && last != "\r") {
                // 最后一行缺少行结束符：可能仍被完整解析（计入 goodRows），
                // 也可能是写入过程中断电。按「可疑截断」上报 1 字节，
                // 让操作者知道文件可能不完整。
                local.truncatedBytes = 1;
            }
        }
    }

    local.reachedRowLimit = stoppedByLimit;
    file.close();

    if (stats)
        *stats = local;

    if (outFrames.isEmpty()) {
        if (errorMsg) {
            *errorMsg = QStringLiteral("文件中没有可用数据（%1）").arg(local.summary());
        }
        return false;
    }
    return true;
}
