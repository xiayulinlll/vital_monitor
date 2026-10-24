#include "PatientBarWidget.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>

PatientBarWidget::PatientBarWidget(QWidget *parent)
    : QWidget(parent)
{
    // 整体深色底 + 细边框，与主界面（#12161D 背景）过渡自然，
    // 但「演示模式」标识单独用高对比橙色，避免被忽略。
    setStyleSheet(QStringLiteral(
        "PatientBarWidget { background:#161B24; border:1px solid #2A3240;"
        " border-radius:4px; }"));

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(8, 4, 8, 4);
    row->setSpacing(6);

    auto makeLabel = [this](const QString &text) {
        auto *l = new QLabel(text, this);
        l->setStyleSheet(QStringLiteral("color:#8A97A8; border:none;"));
        return l;
    };

    // ---- 演示模式标识（9.1 第 5 项：模拟数据必须显著标识）----
    m_demoLabel = new QLabel(QStringLiteral("⚠ 演示模式 · 模拟数据"), this);
    m_demoLabel->setStyleSheet(QStringLiteral(
        "color:#1B1200; background:#F0A020; border:none; border-radius:3px;"
        " padding:2px 8px; font-weight:bold;"));
    m_demoLabel->setToolTip(QStringLiteral(
        "本程序的数据由内置模拟器生成，**不是真实患者测量值**。\n"
        "数据不得用于任何临床判断。"));
    row->addWidget(m_demoLabel);
    row->addSpacing(10);

    // ---- 患者信息（9.1 第 2、4 项）----
    row->addWidget(makeLabel(QStringLiteral("床号")));
    m_bedEdit = new QLineEdit(this);
    m_bedEdit->setPlaceholderText(QStringLiteral("—"));
    m_bedEdit->setFixedWidth(60);
    m_bedEdit->setToolTip(QStringLiteral("床号：报警时用于确认患者归属"));
    row->addWidget(m_bedEdit);

    row->addWidget(makeLabel(QStringLiteral("患者ID")));
    m_idEdit = new QLineEdit(this);
    m_idEdit->setPlaceholderText(QStringLiteral("未登记"));
    m_idEdit->setFixedWidth(110);
    row->addWidget(m_idEdit);

    row->addWidget(makeLabel(QStringLiteral("姓名")));
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setPlaceholderText(QStringLiteral("—"));
    m_nameEdit->setFixedWidth(90);
    row->addWidget(m_nameEdit);

    m_patientTypeLabel = makeLabel(QString());
    m_patientTypeLabel->setText(QStringLiteral("类型：%1")
                                   .arg(patientTypeName(PatientType::Adult)));
    row->addWidget(m_patientTypeLabel);

    row->addSpacing(10);

    // ---- 报警计数（9.1 第 1、2 项的常驻摘要）----
    m_alarmCountLabel = new QLabel(QStringLiteral("活动报警 0 / 已确认 0"), this);
    m_alarmCountLabel->setStyleSheet(QStringLiteral(
        "color:#A8B4C4; border:none; font-weight:bold;"));
    row->addWidget(m_alarmCountLabel);

    row->addStretch(1);

    // ---- 自检结论（7.1 第 5 项）----
    m_selfTestLabel = new QLabel(this);
    m_selfTestLabel->setStyleSheet(QStringLiteral("color:#8A97A8; border:none;"));
    row->addWidget(m_selfTestLabel);
    row->addSpacing(10);

    // ---- 用户角色（8.2 第 2 项）----
    row->addWidget(makeLabel(QStringLiteral("角色")));
    m_roleCombo = new QComboBox(this);
    m_roleCombo->addItem(userRoleName(UserRole::Viewer),   static_cast<int>(UserRole::Viewer));
    m_roleCombo->addItem(userRoleName(UserRole::Operator), static_cast<int>(UserRole::Operator));
    m_roleCombo->addItem(userRoleName(UserRole::Admin),    static_cast<int>(UserRole::Admin));
    m_roleCombo->setToolTip(QStringLiteral(
        "查看者：只能查看\n"
        "操作者：可确认 / 暂停告警、导入导出\n"
        "管理员：可修改量程与报警限（高风险操作）"));
    m_roleCombo->setCurrentIndex(m_roleCombo->findData(static_cast<int>(m_role)));
    m_roleCombo->setFixedWidth(90);
    row->addWidget(m_roleCombo);

    connect(m_roleCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
                const auto role = static_cast<UserRole>(
                    m_roleCombo->itemData(idx).toInt());
                emit roleChangeRequested(role);
            });

    // 患者信息任一字段修改都要可追溯（谁在什么时候改了患者标识）。
    auto notifyPatientChanged = [this]() {
        emit patientInfoChanged(patientSummary());
    };
    connect(m_bedEdit, &QLineEdit::editingFinished, this, notifyPatientChanged);
    connect(m_idEdit, &QLineEdit::editingFinished, this, notifyPatientChanged);
    connect(m_nameEdit, &QLineEdit::editingFinished, this, notifyPatientChanged);
}

QString PatientBarWidget::patientSummary() const
{
    const QString bed  = m_bedEdit->text().trimmed();
    const QString id   = m_idEdit->text().trimmed();
    const QString name = m_nameEdit->text().trimmed();

    QStringList parts;
    parts.append(QStringLiteral("床号=%1").arg(bed.isEmpty() ? QStringLiteral("未填") : bed));
    parts.append(QStringLiteral("ID=%1").arg(id.isEmpty() ? QStringLiteral("未填") : id));
    if (!name.isEmpty())
        parts.append(QStringLiteral("姓名=%1").arg(name));
    parts.append(QStringLiteral("患者类型=%1")
                     .arg(patientTypeName(ChannelConfigManager::globalPatientType())));
    return parts.join(QStringLiteral(" / "));
}

void PatientBarWidget::setRoleSilently(UserRole role)
{
    m_role = role;
    const int idx = m_roleCombo->findData(static_cast<int>(role));
    if (idx >= 0) {
        QSignalBlocker blocker(m_roleCombo);
        m_roleCombo->setCurrentIndex(idx);
    }
}

void PatientBarWidget::setAlarmCounts(int active, int acknowledged)
{
    m_alarmCountLabel->setText(QStringLiteral("活动报警 %1 / 已确认 %2")
                                   .arg(active)
                                   .arg(acknowledged));

    // 有活动未确认报警时用高优先级红色，提醒医护「有未处理的事」。
    m_alarmCountLabel->setStyleSheet(
        active > 0
            ? QStringLiteral("color:#FF6B6B; border:none; font-weight:bold;")
            : QStringLiteral("color:#A8B4C4; border:none; font-weight:bold;"));
}

void PatientBarWidget::setSelfTestResult(const QString &text, bool ok)
{
    m_selfTestLabel->setText(text);
    m_selfTestLabel->setStyleSheet(
        ok ? QStringLiteral("color:#27AE60; border:none;")
           : QStringLiteral("color:#E67E22; border:none; font-weight:bold;"));
    m_selfTestLabel->setToolTip(text);
}
