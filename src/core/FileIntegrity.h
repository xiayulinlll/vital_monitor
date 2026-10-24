#ifndef FILEINTEGRITY_H
#define FILEINTEGRITY_H

#include <QString>
#include <QStringList>

/**
 * @brief 文件完整性校验与输入安全（对应优化文档 8.2 第 1、5 项）。
 *
 * 解决的现存问题：
 *  - 「CSV 文件明文导入导出，无完整性校验，可被篡改」——
 *    导出时在文件旁生成伴生清单 `<file>.sha256`，导入时校验；
 *  - 「对导入文件做输入校验（防路径穿越、超大文件、畸形数据）」——
 *    导入前检查文件大小、行数上限，并拒绝含 NUL / 控制字符的畸形内容。
 *
 * 设计取舍（为什么用 SHA-256 而不是签名）：
 *  - 本项目没有密钥管理与证书体系，做「数字签名」只能造一个假的安全感；
 *  - SHA-256 清单可以检出**意外损坏 / 传输截断 / 随手篡改**，
 *    这是演示层面可验证、可测试的能力；
 *  - 真正的抗恶意篡改需要 HMAC（带密钥）或只写一次存储（WORM），
 *    属生产环境密钥管理体系，超出演示范围。
 *
 * @note 常量时间比较（timing-safe）纯属良好习惯：diff 校验的耗时不应
 *       泄漏「前几个字符猜对了」。
 */
class FileIntegrity
{
public:
    /// 伴生清单文件的后缀（完整文件名 = 原文件名 + 该后缀）。
    static QString manifestSuffix() { return QStringLiteral(".sha256"); }

    /// 计算文件的 SHA-256（小写十六进制）；失败返回空字符串并写 errorMsg。
    static QString sha256OfFile(const QString &filePath,
                                QString *errorMsg = nullptr);

    /**
     * @brief 为数据文件写出伴生清单。
     * @param filePath 已导出的数据文件
     * @param errorMsg 输出错误
     * @return 是否成功（失败不阻断导出本身，调用方仅记录告警）。
     */
    static bool writeManifest(const QString &filePath,
                              QString *errorMsg = nullptr);

    /// 是否存在伴生清单。
    static bool hasManifest(const QString &filePath);

    /**
     * @brief 校验数据文件与其伴生清单是否一致。
     *
     * @param filePath   数据文件
     * @param problems   输出：人类可读的问题（缺失清单 / 哈希不一致）
     * @param errorMsg   输出：IO 错误
     * @return true = 文件存在清单且哈希一致；
     *         false = 不存在清单（此时 problems 会说明，调用方自行决定
     *                 是否阻断 —— 演示程序允许导入无清单的旧文件，
     *                 但必须在界面与审计日志中提示「未校验」）。
     */
    static bool verify(const QString &filePath,
                       QStringList *problems = nullptr,
                       QString *errorMsg = nullptr);

    /**
     * @brief 导入前的输入安全校验（8.2 第 5 项）。
     *
     * 检查内容：
     *  - 文件必须存在且是普通文件（拒绝目录 / 设备文件）；
     *  - 大小不得超过 maxBytes（默认 256 MB），防止一次性读爆内存；
     *  - 不得包含 NUL 字节（文本文件出现 NUL 基本等于二进制或攻击载荷）。
     *
     * @param filePath 待导入文件
     * @param reason   输出：拒绝原因
     * @param maxBytes 大小上限（字节）
     * @return true = 通过校验
     */
    static bool validateImportFile(const QString &filePath,
                                   QString *reason = nullptr,
                                   qint64 maxBytes = 256LL * 1024 * 1024);

    /// 导出时的安全文件名（替换路径分隔符与非法字符，防路径穿越）。
    static QString sanitizeFileName(const QString &name);
};

#endif // FILEINTEGRITY_H
