#ifndef AUDITLOGGER_H
#define AUDITLOGGER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QFile>
#include <QTextStream>
#include <QDateTime>

/**
 * @brief 审计追踪与事件持久化模块（对应优化文档第四章）。
 *
 * 医疗器械要求：
 *  - 配置变更（量程 / 告警限 / 可见性等）必须留下「谁 / 何时 / 改了什么」的可追溯记录；
 *  - 告警事件（含确认记录）必须落盘，供事后审查；
 *  - 记录应追加写入、不可随意篡改（此处采用追加写 + 行内校验和）。
 *
 * 设计：
 *  - 采用单例（进程内唯一），任意模块可通过 AuditLogger::instance() 记录；
 *  - 每条记录一行、制表符分隔、行尾附 CRC32（用于检测篡改 / 损坏）；
 *  - 兼容 ISO 8601 带时区时间戳（便于与医院系统对齐）。
 *
 * @note 本项目为演示用途，校验和用于示范「完整性校验」思路，
 *       并非密码学级别的防篡改方案。
 */
class AuditLogger : public QObject
{
    Q_OBJECT

public:
    /// 审计事件类别。
    enum Category {
        System  = 0,   ///< 系统事件（启动 / 停止 / 自检）
        Config,        ///< 配置变更（量程 / 告警限 / 可见性）
        Alarm,         ///< 告警事件（产生 / 确认 / 静音 / 解除）
        TechAlarm,     ///< 技术报警（导联脱落 / 传感器异常）
        Data           ///< 数据操作（导入 / 导出 / 清空）
    };

    /// 审计事件优先级 / 结果。
    enum Severity {
        Info = 0,
        Warning,
        Critical
    };

    static AuditLogger &instance();

    /**
     * @brief 打开（或创建）审计日志文件，开始记录。
     * @param filePath 日志文件路径；为空则使用默认路径。
     * @return 是否成功。
     */
    bool openFile(const QString &filePath);

    /// 关闭日志文件。
    void close();

    /// 当前是否已启用落盘。
    bool isEnabled() const { return m_file.isOpen(); }

    /// 当前日志文件路径。
    QString filePath() const { return m_filePath; }

    /**
     * @brief 记录一条审计事件。
     * @param category 类别
     * @param severity 严重度
     * @param actor    操作者（演示环境用 "operator"）
     * @param action   动作描述
     * @param detail   详细内容（如 "alarmHigh: 110.0 -> 100.0"）
     */
    void log(Category category,
             Severity severity,
             const QString &action,
             const QString &detail = QString(),
             const QString &actor = QStringLiteral("operator"));

    /**
     * @brief 记录配置变更（自动生成 "字段: 旧值 -> 新值" 明细）。
     */
    void logConfigChange(const QString &channelName,
                         const QString &field,
                         const QString &oldValue,
                         const QString &newValue);

    /// 类别 / 严重度的字符串表示（用于落盘）。
    static QString categoryName(Category c);
    static QString severityName(Severity s);

    /// 计算一行内容的 CRC32 校验和（十六进制字符串）。
    static QString crc32Hex(const QString &text);

    /**
     * @brief 校验一份审计日志的完整性（对应优化文档第四章「可追溯」）。
     *
     * @details
     * 只在写的方向上附 CRC 是**无法证明**日志未被篡改的：没有校验方，
     * 「防篡改」就只是注释里的一句话。本函数提供读回校验：
     * 逐行重算 CRC32 并与行尾记录比对，定位出所有被修改 / 截断的行。
     *
     * @param filePath  待校验的日志文件
     * @param problems  输出：每个问题一行的可读描述（行号 + 原因）
     * @param errorMsg  输出：文件无法打开等 IO 错误
     * @return 全部行校验通过则返回 true。
     */
    static bool verifyFile(const QString &filePath,
                           QStringList *problems = nullptr,
                           QString *errorMsg = nullptr);

    /// 审计日志条目（解析结果，供界面 / 导出使用）。
    struct Entry
    {
        QString timestamp;
        QString category;
        QString severity;
        QString actor;
        QString action;
        QString detail;
        bool    checksumOk = false;
    };

    /**
     * @brief 解析审计日志文件为结构化条目。
     *
     * @param filePath 日志文件
     * @param out      输出条目（按文件顺序）
     * @param errorMsg 输出错误
     * @return 是否成功读取（不代表每行 CRC 都正确，请检查 Entry::checksumOk）。
     */
    static bool readEntries(const QString &filePath,
                            QVector<Entry> &out,
                            QString *errorMsg = nullptr);

private:
    explicit AuditLogger(QObject *parent = nullptr);
    ~AuditLogger() override;

    /// 生成 ISO 8601 带时区时间戳。
    static QString iso8601Now();

    QFile       m_file;
    QTextStream m_stream;
    QString     m_filePath;
};

#endif // AUDITLOGGER_H
