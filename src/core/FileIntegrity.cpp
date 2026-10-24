#include "FileIntegrity.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace {

/// 重算指定算法的哈希（小写十六进制）。
QString hashOfFile(const QString &filePath, QCryptographicHash::Algorithm algo,
                   QString *errorMsg)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (errorMsg)
            *errorMsg = QStringLiteral("无法读取文件：%1").arg(f.errorString());
        return QString();
    }

    QCryptographicHash h(algo);
    // 分块读取：避免把整个历史文件读进内存（导出文件可能上百 MB）。
    constexpr qint64 kChunk = 1 << 20;   // 1 MB
    while (!f.atEnd()) {
        const QByteArray chunk = f.read(kChunk);
        if (chunk.isEmpty() && f.error() != QFileDevice::NoError) {
            if (errorMsg)
                *errorMsg = QStringLiteral("读取文件失败：%1").arg(f.errorString());
            return QString();
        }
        h.addData(chunk);
    }
    return QString::fromLatin1(h.result().toHex());
}

/// 常量时间字符串比较（避免逐字符提前返回泄漏信息）。
bool constantTimeEquals(const QString &a, const QString &b)
{
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (int i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a.at(i).toLatin1())
              ^ static_cast<unsigned char>(b.at(i).toLatin1());
    return diff == 0;
}

} // namespace

QString FileIntegrity::sha256OfFile(const QString &filePath, QString *errorMsg)
{
    return hashOfFile(filePath, QCryptographicHash::Sha256, errorMsg);
}

bool FileIntegrity::writeManifest(const QString &filePath, QString *errorMsg)
{
    const QString hash = sha256OfFile(filePath, errorMsg);
    if (hash.isEmpty())
        return false;

    QFile mf(filePath + manifestSuffix());
    if (!mf.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        if (errorMsg)
            *errorMsg = QStringLiteral("无法写入校验清单：%1").arg(mf.errorString());
        return false;
    }

    // 格式与 `sha256sum` 兼容：<hash>  <filename>
    const QString line = QStringLiteral("%1  %2\n")
                             .arg(hash, QFileInfo(filePath).fileName());
    mf.write(line.toUtf8());
    mf.flush();
    return true;
}

bool FileIntegrity::hasManifest(const QString &filePath)
{
    return QFileInfo::exists(filePath + manifestSuffix());
}

bool FileIntegrity::verify(const QString &filePath,
                           QStringList *problems,
                           QString *errorMsg)
{
    if (!hasManifest(filePath)) {
        if (problems)
            problems->append(QStringLiteral(
                "未找到校验清单（%1）：无法确认文件自导出后是否被修改")
                                     .arg(QFileInfo(filePath).fileName()
                                          + manifestSuffix()));
        return false;
    }

    const QString actual = sha256OfFile(filePath, errorMsg);
    if (actual.isEmpty())
        return false;

    QFile mf(filePath + manifestSuffix());
    if (!mf.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMsg)
            *errorMsg = QStringLiteral("无法读取校验清单：%1").arg(mf.errorString());
        return false;
    }

    // 只取第一行的第一个字段（忽略文件名比较，便于文件改名后仍可校验）。
    const QString first = QString::fromUtf8(mf.readLine()).trimmed();
    const QString expected = first.section(QRegularExpression(QStringLiteral("\\s+")),
                                           0, 0).toLower();
    if (expected.isEmpty()) {
        if (problems)
            problems->append(QStringLiteral("校验清单为空或格式错误"));
        return false;
    }

    if (!constantTimeEquals(actual, expected)) {
        if (problems)
            problems->append(QStringLiteral("SHA-256 不匹配：清单记录 %1…，实际 %2…")
                                 .arg(expected.left(12), actual.left(12)));
        return false;
    }
    return true;
}

bool FileIntegrity::validateImportFile(const QString &filePath,
                                       QString *reason,
                                       qint64 maxBytes)
{
    const QFileInfo fi(filePath);

    if (!fi.exists()) {
        if (reason)
            *reason = QStringLiteral("文件不存在：%1").arg(filePath);
        return false;
    }
    if (!fi.isFile()) {
        if (reason)
            *reason = QStringLiteral("目标不是普通文件：%1").arg(filePath);
        return false;
    }
    if (fi.size() <= 0) {
        if (reason)
            *reason = QStringLiteral("文件为空：%1").arg(fi.fileName());
        return false;
    }
    if (fi.size() > maxBytes) {
        if (reason)
            *reason = QStringLiteral("文件过大（%1 MB，上限 %2 MB），已拒绝导入")
                          .arg(fi.size() / (1024.0 * 1024.0), 0, 'f', 1)
                          .arg(maxBytes / (1024 * 1024));
        return false;
    }

    // NUL 字节探测：合法文本文件不应出现，出现即视为畸形 / 二进制载荷。
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (reason)
            *reason = QStringLiteral("无法打开文件：%1").arg(f.errorString());
        return false;
    }
    constexpr qint64 kProbe = 64 * 1024;   // 探测前 64 KB 足够
    const QByteArray head = f.read(kProbe);
    if (head.contains('\0')) {
        if (reason)
            *reason = QStringLiteral("文件包含 NUL 字节，不是合法的文本历史文件");
        return false;
    }
    return true;
}

QString FileIntegrity::sanitizeFileName(const QString &name)
{
    QString s = name;
    // 路径分隔符与 Windows 保留字符一律替换为下划线。
    static const QRegularExpression illegal(QStringLiteral(R"([\\/:*?"<>|\x00-\x1F])"));
    s.replace(illegal, QStringLiteral("_"));
    s = s.trimmed();
    // 拒绝 "." / ".." 这类可穿越目录的名字。
    if (s.isEmpty() || s == QStringLiteral(".") || s == QStringLiteral(".."))
        s = QStringLiteral("unnamed");
    return s;
}
