#include "CoordinateMapper.h"

#include <QtGlobal>

CoordinateMapper::CoordinateMapper() = default;

void CoordinateMapper::setIndexRange(quint64 startIndex, quint64 endIndex)
{
    if (startIndex <= endIndex) {
        m_viewStart = startIndex;
        m_viewEnd   = endIndex;
    }
}

void CoordinateMapper::setValueRange(int channelId, double minValue, double maxValue)
{
    if (maxValue < minValue)
        qSwap(minValue, maxValue);
    if (qFuzzyCompare(minValue, maxValue))
        maxValue = minValue + 1.0;

    for (auto &pair : m_valueRanges) {
        if (pair.first == channelId) {
            pair.second.min = minValue;
            pair.second.max = maxValue;
            return;
        }
    }
    m_valueRanges.append(qMakePair(channelId, Range{minValue, maxValue}));
}

bool CoordinateMapper::valueRange(int channelId, double &minValue, double &maxValue) const
{
    for (const auto &pair : m_valueRanges) {
        if (pair.first == channelId) {
            minValue = pair.second.min;
            maxValue = pair.second.max;
            return true;
        }
    }
    minValue = 0.0;
    maxValue = 1.0;
    return false;
}

double CoordinateMapper::xForIndex(quint64 index) const
{
    const quint64 span = indexSpan();
    if (span == 0)
        return m_plotRect.left();

    // index 相对视口起点的偏移比例。
    const qint64 offset = static_cast<qint64>(index) - static_cast<qint64>(m_viewStart);
    const double ratio = static_cast<double>(offset) / static_cast<double>(span);
    return m_plotRect.left() + ratio * m_plotRect.width();
}

double CoordinateMapper::yForValue(int channelId, double value) const
{
    double minV = 0.0, maxV = 1.0;
    valueRange(channelId, minV, maxV);

    const double range = (maxV - minV);
    if (qFuzzyIsNull(range))
        return m_plotRect.center().y();

    // 归一化后翻转（画布 Y 向下）。
    double ratio = (value - minV) / range;
    ratio = qBound(0.0, ratio, 1.0);
    return m_plotRect.bottom() - ratio * m_plotRect.height();
}

QPointF CoordinateMapper::pointFor(int channelId, quint64 index, double value) const
{
    return QPointF(xForIndex(index), yForValue(channelId, value));
}

quint64 CoordinateMapper::indexForX(double x) const
{
    if (m_plotRect.width() <= 0.0)
        return m_viewStart;

    const double ratio = (x - m_plotRect.left()) / m_plotRect.width();
    const qint64 idx = static_cast<qint64>(m_viewStart)
                     + static_cast<qint64>(qRound64(ratio * static_cast<double>(indexSpan())));
    return static_cast<quint64>(qMax<qint64>(0, idx));
}
