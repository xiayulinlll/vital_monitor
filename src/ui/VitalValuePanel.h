#ifndef VITALVALUEPANEL_H
#define VITALVALUEPANEL_H

#include <QWidget>
#include <QVector>
#include <QHash>

#include "data/SampleData.h"
#include "core/ChannelConfig.h"

class QLabel;
class QVBoxLayout;

/**
 * @brief 实时体征数值面板（对应架构 2.4 第 1 点）。
 *
 * 为每个通道显示一个数值卡片：名称、当前值、单位；
 * 越限时卡片文字变红高亮（对应功能清单第 5 条）。
 */
class VitalValuePanel : public QWidget
{
    Q_OBJECT

public:
    explicit VitalValuePanel(QWidget *parent = nullptr);

    /// 重建面板（通道配置变化时调用）。
    void buildPanels(const QVector<ChannelConfig> &channels);

    /// 更新一帧数值。
    void updateValues(const SampleFrame &frame);

private:
    struct Card {
        int     channelId = -1;
        QLabel *nameLabel = nullptr;
        QLabel *valueLabel = nullptr;
        QLabel *qualityLabel = nullptr;   ///< 信号质量指示（第二梯队第 7 项）
        double  alarmLow = 0.0;
        double  alarmHigh = 0.0;
        bool    hasLow = false;
        bool    hasHigh = false;

        /// 量程上下限：用于标记「超量程」读数。
        /// 有了它才能在不再 clamp 的前提下，告诉使用者
        /// 「这个数字已经超出可测量范围，精度不可保证」。
        double  minValue = 0.0;
        double  maxValue = 0.0;
        QString unit;
        QString colorHex;
    };

    void clearLayout();

    QVBoxLayout     *m_layout = nullptr;
    QVector<Card>    m_cards;

public:
    /// 更新各通道信号质量指示（无新帧时也可调用）。
    void updateQuality(const SampleFrame &frame);

    /// 依据活动告警集合高亮对应通道卡片边框（视觉报警联动）。
    void applyAlarmState(const QVector<AlarmEvent> &activeAlarms);
};

#endif // VITALVALUEPANEL_H
