#ifndef CHANNELCONFIGPANEL_H
#define CHANNELCONFIGPANEL_H

#include <QWidget>
#include <QVector>

#include "core/ChannelConfig.h"
#include "core/UserRole.h"

class QTableWidget;
class QCheckBox;
class QComboBox;
class QPushButton;

/**
 * @brief 参数配置面板（对应架构 2.4 第 2 点）。
 *
 * 以表格形式展示每路通道的：可见性、颜色、量程下限/上限、告警下限/上限。
 * 用户修改后点击「应用」触发配置下发信号。
 *
 * 第二梯队增强：
 *  - 顶部提供「患者类型」下拉框，切换后按预设剖面填充报警限；
 *  - 应用配置时同步发出审计所需的新旧值差异（由 MainWindow 落盘）。
 *
 * 合规增强（对应优化文档 6.1 第 3、5 项 / 9.1 第 3 项）：
 *  - **二次确认**：改动涉及报警限 / 量程 / 患者类型时，必须弹出
 *    「旧值 → 新值」清单并确认后才下发（误改报警限是已知重大风险源）；
 *  - **权限锁定**：非管理员角色下整表只读，提示由 MainWindow 处理；
 *  - **单位校验**：单位与量纲矛盾时在行内标注并在提示中列出；
 *  - **操作锁定**：「锁定配置」开关可防止误触，锁定期间无法修改。
 */
class ChannelConfigPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ChannelConfigPanel(QWidget *parent = nullptr);

    /// 载入配置到表格。
    void setChannels(const QVector<ChannelConfig> &channels);

    /// 读取表格中当前编辑的配置。
    QVector<ChannelConfig> channels() const { return m_channels; }

    /// 当前选中的患者类型。
    PatientType patientType() const;

    /**
     * @brief 设置当前用户角色（8.2 第 2 项）。
     *
     * 非 Admin 时禁用整表与「应用配置」按钮 —— 这是**界面层**的防护，
     * MainWindow 侧仍会再校验一次权限（不信任 UI 状态）。
     */
    void setUserRole(UserRole role);

    /**
     * @brief 关键参数二次确认（6.1 第 3 项）。
     *
     * 由面板自己完成交互（贴近控件、便于展示差异），MainWindow 只负责授权。
     * 若不需要确认则立即返回 true。
     * @param changedSummary 输出：需要确认的变更清单（用于审计 / 提示）
     */
    bool confirmCriticalChanges(QString *changedSummary = nullptr);

signals:
    /// 用户点击应用后发出更新后的配置。
    void configApplied(const QVector<ChannelConfig> &channels);

    /**
     * @brief 用户切换患者类型（第二梯队：患者类型预设）。
     *
     * MainWindow 收到后调用 ChannelConfigManager::loadProfile()，
     * 再把结果回灌到面板（setChannels）。
     */
    void patientTypeChangeRequested(PatientType type);

    /// 用户请求取消（拒绝二次确认 / 取消患者类型切换时发出）。
    void changeRejected(const QString &reason);

public slots:
    /// 回滚患者类型下拉框到指定值（拒绝切换时使用，避免界面与配置不一致）。
    void revertPatientType(PatientType type);

private slots:
    void onApplyClicked();
    void onRowChanged(int row, int column);
    void onColorButtonClicked();
    void onLockToggled(bool locked);

private:
    void rebuildTable();
    void refreshRow(int row);

    /// 对比 m_channels 与 m_appliedChannels，返回「字段: 旧值 -> 新值」清单。
    QString diffSummary() const;

    /// 是否存在需要二次确认的关键变更（报警限 / 量程 / 患者类型）。
    bool hasCriticalChange() const;

    /// 应用当前角色与锁定状态到各控件。
    void applyEditability();

    QTableWidget *m_table = nullptr;
    QComboBox    *m_patientCombo = nullptr;
    QPushButton  *m_applyBtn = nullptr;
    QPushButton  *m_lockBtn = nullptr;
    QVector<ChannelConfig> m_channels;

    /// 上次成功下发的配置（用于生成差异与二次确认清单）。
    QVector<ChannelConfig> m_appliedChannels;

    QVector<bool> m_visible;   ///< 每行的可见性勾选状态
    UserRole m_role = UserRole::Operator;
    bool     m_locked = false;
    bool     m_revertingPatientCombo = false;
};

#endif // CHANNELCONFIGPANEL_H
