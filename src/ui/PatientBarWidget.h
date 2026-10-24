#ifndef PATIENTBARWIDGET_H
#define PATIENTBARWIDGET_H

#include <QWidget>

#include "core/ChannelConfig.h"
#include "core/UserRole.h"

class QLineEdit;
class QLabel;
class QComboBox;

/**
 * @brief 患者信息 + 会话状态条（对应优化文档 9.1 第 2、4、5 项）。
 *
 * 解决三个现存问题：
 *  1. 「无患者信息区」—— 缺少患者 ID / 姓名 / 床号，报警时无法确认
 *     「这条报警是谁的」，正式产品必需；
 *  2. 「模拟数据未显著标识」—— Demo 数据可能被误认为真实测量值。
 *     这里用**常驻的橙色「演示模式 · 模拟数据」标识**，而不是只写在
 *     README 里；只要数据源是模拟器，标识就必须一直可见；
 *  3. 「无用户身份与权限」—— 提供角色切换（查看者 / 操作者 / 管理员），
 *     越权操作由 MainWindow 拦截并写审计日志。
 *
 * 设计取舍：患者信息只保存在内存中并写入审计日志，
 * 不在本演示中落盘（涉及隐私数据留存策略，属机构政策范畴）。
 */
class PatientBarWidget : public QWidget
{
    Q_OBJECT

public:
    explicit PatientBarWidget(QWidget *parent = nullptr);

    /// 患者标识（写入审计日志用，形如 "床号=3 / ID=P00123"）。
    QString patientSummary() const;

    /// 当前角色。
    UserRole currentRole() const { return m_role; }

    /// 以编程方式设置角色（不会发出信号，供初始化使用）。
    void setRoleSilently(UserRole role);

    /// 更新「活动报警 / 已确认」计数显示。
    void setAlarmCounts(int active, int acknowledged);

    /// 更新自检结论显示。
    void setSelfTestResult(const QString &text, bool ok);

signals:
    /// 用户在界面上切换了角色。
    void roleChangeRequested(UserRole role);
    /// 患者信息被修改（用于写审计）。
    void patientInfoChanged(const QString &summary);

private:
    QLineEdit *m_bedEdit = nullptr;
    QLineEdit *m_idEdit = nullptr;
    QLineEdit *m_nameEdit = nullptr;
    QComboBox *m_roleCombo = nullptr;
    QLabel    *m_patientTypeLabel = nullptr;
    QLabel    *m_demoLabel = nullptr;
    QLabel    *m_alarmCountLabel = nullptr;
    QLabel    *m_selfTestLabel = nullptr;

    UserRole m_role = UserRole::Operator;
};

#endif // PATIENTBARWIDGET_H
