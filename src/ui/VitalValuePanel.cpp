#include "VitalValuePanel.h"

#include <QColor>
#include <QFrame>
#include <QLabel>
#include <QVBoxLayout>

VitalValuePanel::VitalValuePanel(QWidget *parent)
    : QWidget(parent)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 6, 4, 6);   // 面板收窄，压缩外边距
    outer->setSpacing(6);

    auto *title = new QLabel(QStringLiteral("实时体征数值"), this);
    QFont tf = title->font();
    tf.setBold(true);
    tf.setPointSizeF(tf.pointSizeF() + 0.5);
    title->setFont(tf);
    outer->addWidget(title);

    m_layout = new QVBoxLayout();
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(6);
    outer->addLayout(m_layout);
    outer->addStretch(1);
}

void VitalValuePanel::clearLayout()
{
    while (QLayoutItem *item = m_layout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }
    m_cards.clear();
}

void VitalValuePanel::buildPanels(const QVector<ChannelConfig> &channels)
{
    clearLayout();

    for (const ChannelConfig &c : channels) {
        auto *card = new QFrame(this);
        card->setFrameShape(QFrame::StyledPanel);
        card->setStyleSheet(QStringLiteral(
            "QFrame { background:#1B2029; border:1px solid #2A3240; border-radius:6px; }"));

        auto *v = new QVBoxLayout(card);
        v->setContentsMargins(7, 6, 7, 6);   // 面板收窄后同步压缩卡片内边距
        v->setSpacing(2);

        auto *name = new QLabel(QStringLiteral("%1  (%2)").arg(c.name, c.unit), card);
        name->setStyleSheet(QStringLiteral("color:#9AA7B8; border:none; background:transparent;"));
        QFont nf = name->font();
        nf.setPointSizeF(nf.pointSizeF() - 0.5);
        name->setFont(nf);

        auto *value = new QLabel(QStringLiteral("--"), card);
        value->setStyleSheet(QStringLiteral("color:%1; border:none; background:transparent; font-weight:bold;")
                                 .arg(c.color.name()));
        QFont vf = value->font();
        vf.setPointSizeF(vf.pointSizeF() + 3.5);   // 收窄后相应调小字号，保证数值不溢出
        value->setFont(vf);

        // 信号质量指示：显示 SQI 百分比，低质量时变色提示（第二梯队第 7 项）。
        auto *quality = new QLabel(QStringLiteral("SQI --%"), card);
        QFont qf = quality->font();
        qf.setPointSizeF(qf.pointSizeF() - 1.5);
        quality->setFont(qf);
        quality->setStyleSheet(QStringLiteral("color:#6E7A8A; border:none; background:transparent;"));

        v->addWidget(name);
        v->addWidget(value);
        v->addWidget(quality);
        m_layout->addWidget(card);

        Card cd;
        cd.channelId = c.id;
        cd.nameLabel = name;
        cd.valueLabel = value;
        cd.qualityLabel = quality;
        cd.alarmLow = c.alarmLow;
        cd.alarmHigh = c.alarmHigh;
        cd.hasLow = c.hasLowAlarm;
        cd.hasHigh = c.hasHighAlarm;
        cd.minValue = c.minValue;
        cd.maxValue = c.maxValue;
        cd.unit = c.unit;
        cd.colorHex = c.color.name();
        m_cards.append(cd);
    }
}

void VitalValuePanel::updateValues(const SampleFrame &frame)
{
    for (const ChannelSample &s : frame.channels) {
        for (Card &cd : m_cards) {
            if (cd.channelId != s.channelId)
                continue;

            // 信号质量差 / 测量无效时，数值显示为 "--"，
            // 避免护士把不可靠读数当作真实值（第二梯队第 7 项）。
            const bool unreliable = s.invalid || (s.quality < 0.35);

            // 伪迹样本：值存在但不可信。**不能显示为正常数字**，
            // 否则一次运动伪迹（如 HR 瞬间 250）会被当成真实心率；
            // 但也不能像 unreliable 那样显示 "--" —— 原始值必须可见，
            // 因为「测到了什么」本身是有价值的信息（优化文档 3.2 第 3 项）。
            const bool artifact = s.artifact && !unreliable;

            if (unreliable) {
                cd.valueLabel->setText(QStringLiteral("--"));
                cd.valueLabel->setStyleSheet(QStringLiteral(
                    "color:#78838F; border:none; background:transparent; font-weight:bold;"));
            } else {
                // 超量程数值加 "↑/↓" 后缀：既然不再 clamp，就必须让
                // 使用者知道这个读数已经超出量程、其精度不可保证。
                QString text = QString::number(s.value, 'f', 1);
                if (s.value > cd.maxValue)
                    text += QStringLiteral("↑");
                else if (s.value < cd.minValue)
                    text += QStringLiteral("↓");
                if (artifact)
                    text += QStringLiteral("?");   // 伪迹标记
                cd.valueLabel->setText(text);

                const bool lowAlarm  = cd.hasLow  && s.value < cd.alarmLow;
                const bool highAlarm = cd.hasHigh && s.value > cd.alarmHigh;
                const bool alarmed = lowAlarm || highAlarm;

                // 越限：红色高亮；伪迹：橙色（区别于真实的越限红色）；
                // 正常：使用通道颜色。
                if (alarmed && !artifact) {
                    cd.valueLabel->setStyleSheet(QStringLiteral(
                        "color:#FF5252; border:none; background:transparent; font-weight:bold;"));
                } else if (artifact) {
                    cd.valueLabel->setStyleSheet(QStringLiteral(
                        "color:#E0A030; border:none; background:transparent; font-weight:bold;"));
                } else {
                    cd.valueLabel->setStyleSheet(
                        QStringLiteral("color:%1; border:none; background:transparent; font-weight:bold;")
                            .arg(cd.colorHex));
                }
            }

            // 信号质量指示同步刷新。
            if (cd.qualityLabel) {
                const int pct = qRound(s.quality * 100.0);
                if (unreliable) {
                    cd.qualityLabel->setText(s.invalid
                                                 ? QStringLiteral("信号无效")
                                                 : QStringLiteral("信号差 SQI %1%").arg(pct));
                    cd.qualityLabel->setStyleSheet(QStringLiteral(
                        "color:#E0A030; border:none; background:transparent;"));
                } else if (artifact) {
                    // 明确写出「伪迹」，与「信号差」区分：
                    // 前者是数据不可信（已抑制报警），后者是测不到。
                    cd.qualityLabel->setText(QStringLiteral("伪迹 SQI %1%").arg(pct));
                    cd.qualityLabel->setStyleSheet(QStringLiteral(
                        "color:#E0A030; border:none; background:transparent;"));
                } else {
                    cd.qualityLabel->setText(QStringLiteral("SQI %1%").arg(pct));
                    cd.qualityLabel->setStyleSheet(QStringLiteral(
                        "color:#6E7A8A; border:none; background:transparent;"));
                }
            }
            break;
        }
    }
}

void VitalValuePanel::updateQuality(const SampleFrame &frame)
{
    updateValues(frame);
}

void VitalValuePanel::applyAlarmState(const QVector<AlarmEvent> &activeAlarms)
{
    // 依据活动告警，把对应通道卡片边框染成严重度色，形成全局视觉联动。
    for (Card &cd : m_cards) {
        QColor border(0x2A, 0x32, 0x40);   // 默认边框

        for (const AlarmEvent &a : activeAlarms) {
            if (a.channelId != cd.channelId)
                continue;

            // 技术报警用蓝色边框区分生理报警，便于快速定位设备问题。
            if (a.category == AlarmEvent::Technical) {
                border = QColor(0x2A, 0x6E, 0x8A);
                break;
            }

            QColor c;
            switch (a.level) {
            case AlarmEvent::Critical:
            case AlarmEvent::Escalated: c = alarmIndicatorColor(a.level); break;
            case AlarmEvent::Warning:   c = alarmIndicatorColor(a.level); break;
            default:                    c = QColor(0x2A, 0x6E, 0x8A); break;
            }
            // 已确认的告警边框减淡，表示「已处理但仍未恢复」。
            border = a.state == AlarmEvent::Acknowledged ? c.darker(150) : c;
            break;
        }

        if (cd.nameLabel) {
            QWidget *card = cd.nameLabel->parentWidget();
            if (card)
                card->setStyleSheet(QStringLiteral(
                    "QFrame { background:#1B2029; border:1px solid %1; border-radius:6px; }")
                        .arg(border.name()));
        }
    }
}

