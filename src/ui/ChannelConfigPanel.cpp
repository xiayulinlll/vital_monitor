#include "ChannelConfigPanel.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>

ChannelConfigPanel::ChannelConfigPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(6, 6, 6, 6);
    outer->setSpacing(6);

    auto *title = new QLabel(QStringLiteral("通道参数配置"), this);
    QFont tf = title->font();
    tf.setBold(true);
    tf.setPointSizeF(tf.pointSizeF() + 0.5);
    title->setFont(tf);
    outer->addWidget(title);

    // ---- 患者类型选择行（第二梯队：患者类型预设）----
    auto *patientRow = new QHBoxLayout();
    patientRow->setSpacing(6);
    patientRow->addWidget(new QLabel(QStringLiteral("患者类型:"), this));

    m_patientCombo = new QComboBox(this);
    m_patientCombo->addItem(QStringLiteral("成人"), static_cast<int>(PatientType::Adult));
    m_patientCombo->addItem(QStringLiteral("小儿"), static_cast<int>(PatientType::Pediatric));
    m_patientCombo->addItem(QStringLiteral("新生儿"), static_cast<int>(PatientType::Neonate));
    m_patientCombo->addItem(QStringLiteral("自定义"), static_cast<int>(PatientType::Custom));
    m_patientCombo->setToolTip(QStringLiteral(
        "切换患者类型将按预设剖面重载各通道量程与报警限\n"
        "（避免用成人报警限监护新生儿导致大量误报）"));
    patientRow->addWidget(m_patientCombo);
    patientRow->addStretch(1);
    outer->addLayout(patientRow);

    connect(m_patientCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
                const auto type = static_cast<PatientType>(
                    m_patientCombo->itemData(idx).toInt());
                emit patientTypeChangeRequested(type);
            });

    m_table = new QTableWidget(this);
    m_table->setColumnCount(7);
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("显示"),
        QStringLiteral("通道"),
        QStringLiteral("颜色"),
        QStringLiteral("量程下限"),
        QStringLiteral("量程上限"),
        QStringLiteral("告警下限"),
        QStringLiteral("告警上限")
    });

    // 列宽策略：显示/颜色两列固定窄宽，量程/告警四列按内容自适应，
    // 让告警下限、告警上限各自独占一整列，避免两个 spinbox 挤在同一列里溢出。
    QHeaderView *hh = m_table->horizontalHeader();
    hh->setSectionResizeMode(0, QHeaderView::Fixed);   // 显示
    hh->setSectionResizeMode(1, QHeaderView::Interactive); // 通道
    hh->setSectionResizeMode(2, QHeaderView::Fixed);   // 颜色
    hh->setSectionResizeMode(3, QHeaderView::ResizeToContents); // 量程下限
    hh->setSectionResizeMode(4, QHeaderView::ResizeToContents); // 量程上限
    hh->setSectionResizeMode(5, QHeaderView::Interactive); // 告警下限
    hh->setSectionResizeMode(6, QHeaderView::Interactive); // 告警上限
    // 宽度必须容纳"两字表头 + 单元格内边距(左右各约 4px)"，
    // 否则窄列下文字会被省略号截断。
    m_table->setColumnWidth(0, 56);
    m_table->setColumnWidth(2, 60);
    // 量程 / 告警四列宽度保持一致，避免"上限比下限宽"。
    for (int col = 3; col <= 6; ++col)
        m_table->setColumnWidth(col, 72);

    hh->setStretchLastSection(false);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    outer->addWidget(m_table, 1);

    auto *btnRow = new QHBoxLayout();
    m_lockBtn = new QPushButton(QStringLiteral("🔒 锁定配置"), this);
    m_lockBtn->setCheckable(true);
    m_lockBtn->setToolTip(QStringLiteral(
        "锁定后禁止修改量程 / 报警限，防止床旁误触\n"
        "（对应优化文档 6.1 第 5 项「抗误操作设计」）"));
    btnRow->addWidget(m_lockBtn);

    auto *hint = new QLabel(QStringLiteral("修改报警限需管理员角色 + 二次确认"), this);
    hint->setStyleSheet(QStringLiteral("color:#8A97A8;"));
    btnRow->addWidget(hint);

    btnRow->addStretch(1);
    m_applyBtn = new QPushButton(QStringLiteral("应用配置"), this);
    m_applyBtn->setToolTip(QStringLiteral("应用前会列出「旧值 → 新值」供二次确认"));
    btnRow->addWidget(m_applyBtn);
    outer->addLayout(btnRow);

    connect(m_applyBtn, &QPushButton::clicked, this, &ChannelConfigPanel::onApplyClicked);
    connect(m_lockBtn, &QPushButton::toggled, this, &ChannelConfigPanel::onLockToggled);

    applyEditability();
}

PatientType ChannelConfigPanel::patientType() const
{
    if (!m_patientCombo)
        return PatientType::Adult;
    return static_cast<PatientType>(m_patientCombo->currentData().toInt());
}

void ChannelConfigPanel::setChannels(const QVector<ChannelConfig> &channels)
{
    m_channels = channels;
    m_appliedChannels = channels;
    m_visible.clear();
    for (const ChannelConfig &c : channels)
        m_visible.append(c.visible);

    // 回灌配置时同步下拉框状态（阻塞信号避免触发二次重载）。
    if (m_patientCombo) {
        const int idx = m_patientCombo->findData(
            static_cast<int>(ChannelConfigManager::globalPatientType()));
        if (idx >= 0) {
            QSignalBlocker blocker(m_patientCombo);
            m_patientCombo->setCurrentIndex(idx);
        }
    }

    rebuildTable();
}

void ChannelConfigPanel::rebuildTable()
{
    m_table->setRowCount(m_channels.size());

    for (int row = 0; row < m_channels.size(); ++row) {
        const ChannelConfig &c = m_channels.at(row);

        // 列 0：可见性复选框
        auto *chk = new QCheckBox(m_table);
        chk->setChecked(c.visible);
        connect(chk, &QCheckBox::toggled, this, [this, row](bool on) {
            if (row >= 0 && row < m_visible.size())
                m_visible[row] = on;
        });
        m_table->setCellWidget(row, 0, chk);

        // 列 1：通道名称
        // 单位校验（9.1 第 3 项）：单位与量纲矛盾时在名称后标注 ⚠，
        // 避免「体温通道写着 bpm」这类静默错配被一直沿用。
        QString nameText = QStringLiteral("%1 (%2)").arg(c.name, c.unit);
        const UnitIssue ui = c.validateUnit();
        if (ui == UnitIssue::Mismatch || ui == UnitIssue::NotInRange) {
            nameText += QStringLiteral("  ⚠");
        }
        auto *nameItem = new QTableWidgetItem(nameText);
        if (ui != UnitIssue::Ok) {
            nameItem->setToolTip(QStringLiteral("%1：%2")
                                     .arg(unitIssueName(ui), c.unit));
            nameItem->setForeground(QColor(0xE6, 0x7E, 0x22));
        }
        m_table->setItem(row, 1, nameItem);

        // 列 2：颜色按钮。不设置固定宽度，让按钮自动填满单元格宽度；
        // 仅锁定高度，避免行高被撑开。
        auto *colorBtn = new QPushButton(m_table);
        colorBtn->setFixedHeight(22);
        colorBtn->setToolTip(QStringLiteral("点击选择曲线颜色"));
        colorBtn->setStyleSheet(QStringLiteral("background:%1; border:1px solid #555;")
                                    .arg(c.color.name()));
        colorBtn->setProperty("row", row);
        connect(colorBtn, &QPushButton::clicked, this, &ChannelConfigPanel::onColorButtonClicked);
        m_table->setCellWidget(row, 2, colorBtn);

        // 列 3：量程下限
        auto *minSpin = new QDoubleSpinBox(m_table);
        minSpin->setRange(-1000.0, 10000.0);
        minSpin->setValue(c.minValue);
        connect(minSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this, row](double v) {
                    if (row >= 0 && row < m_channels.size())
                        m_channels[row].minValue = v;
                });
        m_table->setCellWidget(row, 3, minSpin);

        // 列 4：量程上限
        auto *maxSpin = new QDoubleSpinBox(m_table);
        maxSpin->setRange(-1000.0, 10000.0);
        maxSpin->setValue(c.maxValue);
        connect(maxSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this, row](double v) {
                    if (row >= 0 && row < m_channels.size())
                        m_channels[row].maxValue = v;
                });
        m_table->setCellWidget(row, 4, maxSpin);

        // 列 5：告警下限（独立整列，占满列宽）
        auto *lowSpin = new QDoubleSpinBox(m_table);
        lowSpin->setRange(-1000.0, 10000.0);
        lowSpin->setDecimals(1);
        lowSpin->setValue(c.alarmLow);
        lowSpin->setToolTip(QStringLiteral("告警下限"));
        lowSpin->setButtonSymbols(QAbstractSpinBox::UpDownArrows);
        connect(lowSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this, row](double v) {
                    if (row >= 0 && row < m_channels.size())
                        m_channels[row].alarmLow = v;
                });
        m_table->setCellWidget(row, 5, lowSpin);

        // 列 6：告警上限（独立整列，占满列宽）
        auto *highSpin = new QDoubleSpinBox(m_table);
        highSpin->setRange(-1000.0, 10000.0);
        highSpin->setDecimals(1);
        highSpin->setValue(c.alarmHigh);
        highSpin->setToolTip(QStringLiteral("告警上限"));
        highSpin->setButtonSymbols(QAbstractSpinBox::UpDownArrows);
        connect(highSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this, row](double v) {
                    if (row >= 0 && row < m_channels.size())
                        m_channels[row].alarmHigh = v;
                });
        m_table->setCellWidget(row, 6, highSpin);
    }

    m_table->resizeRowsToContents();
}


void ChannelConfigPanel::refreshRow(int row)
{
    Q_UNUSED(row);
}

void ChannelConfigPanel::onRowChanged(int row, int column)
{
    Q_UNUSED(row);
    Q_UNUSED(column);
}

void ChannelConfigPanel::onColorButtonClicked()
{
    auto *btn = qobject_cast<QPushButton *>(sender());
    if (!btn)
        return;
    const int row = btn->property("row").toInt();
    if (row < 0 || row >= m_channels.size())
        return;

    const QColor color = QColorDialog::getColor(m_channels.at(row).color, this,
                                                QStringLiteral("选择曲线颜色"));
    if (!color.isValid())
        return;

    m_channels[row].color = color;
    btn->setStyleSheet(QStringLiteral("background:%1; border:1px solid #555;").arg(color.name()));
}

bool ChannelConfigPanel::hasCriticalChange() const
{
    // 只有「改变报警行为」的字段才算关键变更：
    // 量程端点、一级阈值、阈值启停。颜色 / 可见性不影响报警语义，
    // 若也弹窗确认，会让医护形成「一路点确定」的疲劳习惯，反而更危险。
    if (m_channels.size() != m_appliedChannels.size())
        return true;

    for (int i = 0; i < m_channels.size(); ++i) {
        const ChannelConfig &o = m_appliedChannels.at(i);
        const ChannelConfig &c = m_channels.at(i);

        if (!qFuzzyCompare(o.minValue + 1.0, c.minValue + 1.0)
            || !qFuzzyCompare(o.maxValue + 1.0, c.maxValue + 1.0)
            || !qFuzzyCompare(o.alarmLow + 1.0, c.alarmLow + 1.0)
            || !qFuzzyCompare(o.alarmHigh + 1.0, c.alarmHigh + 1.0))
            return true;
        if (o.hasLowAlarm != c.hasLowAlarm || o.hasHighAlarm != c.hasHighAlarm)
            return true;
    }
    return false;
}

bool ChannelConfigPanel::confirmCriticalChanges(QString *changedSummary)
{
    const QString diff = diffSummary();
    if (changedSummary)
        *changedSummary = diff;

    if (!hasCriticalChange())
        return true;   // 仅颜色 / 可见性变化，无需打断操作

    // 单位校验结论一并提示：错单位会让阈值语义整体错位（如 ℉ / ℃）。
    QStringList warnings;
    for (const ChannelConfig &c : m_channels) {
        const UnitIssue ui = c.validateUnit();
        if (ui != UnitIssue::Ok)
            warnings.append(QStringLiteral("%1：%2").arg(c.name, unitIssueName(ui)));
    }

    QString text = QStringLiteral(
        "以下变更会影响报警行为，请核对后确认：\n\n%1").arg(
            diff.isEmpty() ? QStringLiteral("（无字段差异）") : diff);

    if (!warnings.isEmpty())
        text += QStringLiteral("\n\n⚠ 单位 / 量纲提示：\n%1").arg(
            warnings.join(QLatin1Char('\n')));

    text += QStringLiteral("\n\n确认应用？本次修改将写入审计日志。");

    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QStringLiteral("确认修改报警参数"));
    box.setText(text);
    QPushButton *okBtn = box.addButton(QStringLiteral("确认应用"), QMessageBox::AcceptRole);
    box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    box.setDefaultButton(okBtn);
    box.exec();

    const bool accepted = (box.clickedButton() == okBtn);
    if (!accepted)
        emit changeRejected(QStringLiteral("用户取消了报警参数修改"));
    return accepted;
}

void ChannelConfigPanel::setUserRole(UserRole role)
{
    m_role = role;
    applyEditability();
}

void ChannelConfigPanel::applyEditability()
{
    // 修改配置需要 Admin；锁定开关本身也需要 Admin（否则等同无锁）。
    const bool canEdit = roleHasPermission(m_role, Permission::ModifyConfig) && !m_locked;

    if (m_table)
        m_table->setEnabled(canEdit);
    if (m_applyBtn)
        m_applyBtn->setEnabled(canEdit);
    if (m_patientCombo)
        m_patientCombo->setEnabled(canEdit);
    if (m_lockBtn) {
        m_lockBtn->setEnabled(roleHasPermission(m_role, Permission::ModifyConfig));
        m_lockBtn->setText(m_locked ? QStringLiteral("🔓 已锁定（点击解锁）")
                                    : QStringLiteral("🔒 锁定配置"));
    }
}

void ChannelConfigPanel::onLockToggled(bool locked)
{
    m_locked = locked;
    applyEditability();

    emit changeRejected(locked ? QStringLiteral("配置已锁定（防误触）")
                               : QStringLiteral("配置已解锁，可修改量程与报警限"));
}

void ChannelConfigPanel::revertPatientType(PatientType type)
{
    if (!m_patientCombo)
        return;

    // 切换过程中若配置被拒绝，界面必须回滚，否则出现
    // 「下拉框显示新生儿、实际生效成人阈值」的危险不一致。
    const int idx = m_patientCombo->findData(static_cast<int>(type));
    if (idx >= 0) {
        QSignalBlocker blocker(m_patientCombo);
        m_patientCombo->setCurrentIndex(idx);
    }
}

void ChannelConfigPanel::onApplyClicked()
{
    // 界面层第一道闸：非管理员 / 已锁定 → 直接拒绝并说明原因。
    // （MainWindow 侧仍会再校验一次权限，不信任 UI 状态。）
    if (!roleHasPermission(m_role, Permission::ModifyConfig)) {
        emit changeRejected(
            QStringLiteral("当前角色为「%1」，无权修改量程与报警限（需管理员）")
                .arg(userRoleName(m_role)));
        return;
    }
    if (m_locked) {
        emit changeRejected(QStringLiteral("配置已锁定，请先解除锁定再应用"));
        return;
    }

    // 把可见性写回配置。
    for (int i = 0; i < m_channels.size() && i < m_visible.size(); ++i)
        m_channels[i].visible = m_visible.at(i);

    // 第二道闸：关键参数二次确认（6.1 第 3 项）。
    if (!confirmCriticalChanges(nullptr))
        return;

    m_appliedChannels = m_channels;
    emit configApplied(m_channels);
}

QString ChannelConfigPanel::diffSummary() const
{
    QStringList lines;
    const int n = qMin(m_channels.size(), m_appliedChannels.size());
    for (int i = 0; i < n; ++i) {
        const ChannelConfig &o = m_appliedChannels.at(i);
        const ChannelConfig &c = m_channels.at(i);

        auto addIfChanged = [&lines, &c](const QString &field,
                                         double ov, double nv) {
            if (qFuzzyCompare(ov + 1.0, nv + 1.0))
                return;
            lines.append(QStringLiteral("%1.%2：%3 → %4")
                             .arg(c.name, field,
                                  QString::number(ov, 'f', 1),
                                  QString::number(nv, 'f', 1)));
        };

        addIfChanged(QStringLiteral("量程下限"), o.minValue, c.minValue);
        addIfChanged(QStringLiteral("量程上限"), o.maxValue, c.maxValue);
        addIfChanged(QStringLiteral("报警下限"), o.alarmLow, c.alarmLow);
        addIfChanged(QStringLiteral("报警上限"), o.alarmHigh, c.alarmHigh);

        if (o.hasLowAlarm != c.hasLowAlarm)
            lines.append(QStringLiteral("%1.下限告警：%2 → %3")
                             .arg(c.name,
                                  o.hasLowAlarm ? QStringLiteral("启用") : QStringLiteral("关闭"),
                                  c.hasLowAlarm ? QStringLiteral("启用") : QStringLiteral("关闭")));
        if (o.hasHighAlarm != c.hasHighAlarm)
            lines.append(QStringLiteral("%1.上限告警：%2 → %3")
                             .arg(c.name,
                                  o.hasHighAlarm ? QStringLiteral("启用") : QStringLiteral("关闭"),
                                  c.hasHighAlarm ? QStringLiteral("启用") : QStringLiteral("关闭")));
    }
    return lines.join(QLatin1Char('\n'));
}
