#ifndef COORDINATEMAPPER_H
#define COORDINATEMAPPER_H

#include <QPointF>
#include <QRectF>
#include <QVector>

#include "core/ChannelConfig.h"

/**
 * @brief 坐标变换计算器（对应架构 2.2 第 3 点，解决难点 4）。
 *
 * 职责：把「采样序号 + 生理数值」映射为绘图控件的画布像素坐标。
 * 多通道量程不同 → 每个通道独立做 Y 轴归一化，X 轴共享同一时间视口。
 *
 * 采用视口（Viewport）模型：
 *  - 视口的 X 轴用采样序号区间 [viewStartIndex, viewEndIndex] 表示；
 *  - 切换通道时只改 Y 量程，不触碰原始数据（对应难点 5）。
 */
class CoordinateMapper
{
public:
    CoordinateMapper();

    /// 设置绘图区（曲线实际绘制区域，已排除坐标轴/边距）。
    void setPlotRect(const QRectF &rect) { m_plotRect = rect; }
    QRectF plotRect() const { return m_plotRect; }

    /// 设置 X 轴采样序号视口范围。
    void setIndexRange(quint64 startIndex, quint64 endIndex);
    quint64 viewStartIndex() const { return m_viewStart; }
    quint64 viewEndIndex() const { return m_viewEnd; }

    /// 设置某通道的 Y 轴量程（原始数值范围）。
    void setValueRange(int channelId, double minValue, double maxValue);
    bool valueRange(int channelId, double &minValue, double &maxValue) const;

    /// 把采样序号映射为画布 X 像素。
    double xForIndex(quint64 index) const;

    /// 把某通道的原始数值映射为画布 Y 像素（注意 Y 轴向下）。
    double yForValue(int channelId, double value) const;

    /// 便捷：一次得到点坐标。
    QPointF pointFor(int channelId, quint64 index, double value) const;

    /// 画布 X 像素反查采样序号（用于鼠标定位 / 探针）。
    quint64 indexForX(double x) const;

    /// 当前 X 视口跨越的采样点数。
    quint64 indexSpan() const
    {
        return (m_viewEnd >= m_viewStart) ? (m_viewEnd - m_viewStart) : 0;
    }

private:
    struct Range { double min = 0.0; double max = 1.0; };

    QRectF m_plotRect;
    quint64 m_viewStart = 0;
    quint64 m_viewEnd   = 0;
    QVector<QPair<int, Range>> m_valueRanges; ///< channelId -> 量程
};

#endif // COORDINATEMAPPER_H
