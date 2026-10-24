#include "TrendChartWidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <QDateTime>
#include <QtMath>

namespace {
// 绘图区域边距（左侧留出 Y 轴刻度，底部留出 X 轴标签）。
constexpr int kMarginLeft   = 64;
constexpr int kMarginRight  = 16;
constexpr int kMarginTop    = 16;
constexpr int kMarginBottom = 34;

const QColor kBgColor      (0x14, 0x18, 0x20);
const QColor kPlotBgColor  (0x0D, 0x10, 0x16);
const QColor kGridColor    (0x2A, 0x32, 0x40);
const QColor kGridMajorCol (0x3A, 0x46, 0x58);
const QColor kAxisColor    (0x6B, 0x7A, 0x90);
const QColor kTextColor    (0xB8, 0xC4, 0xD4);
} // namespace

TrendChartWidget::TrendChartWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(480, 260);
    setMouseTracking(true);
    setAutoFillBackground(false);
    setFocusPolicy(Qt::StrongFocus);
}

TrendChartWidget::~TrendChartWidget() = default;

void TrendChartWidget::setChannels(const QVector<ChannelConfig> &channels)
{
    m_channels = channels;

    // 按通道重建序列，尽量保留已有数据。
    QVector<ChannelSeries> newSeries;
    newSeries.reserve(channels.size());
    for (const ChannelConfig &c : channels) {
        ChannelSeries s;
        s.id = c.id;
        for (const ChannelSeries &old : m_series) {
            if (old.id == c.id) {
                s.points = old.points;
                break;
            }
        }
        newSeries.append(s);
    }
    m_series = newSeries;

    m_backgroundDirty = true;
    updateMapper();
    update();
}

void TrendChartWidget::appendFrames(const QVector<SampleFrame> &frames)
{
    if (frames.isEmpty())
        return;

    // 收集数据首尾序号范围。
    for (const SampleFrame &f : frames) {
        if (m_dataStart == 0 && m_dataEnd == 0) {
            m_dataStart = f.sampleIndex;
            m_dataEnd = f.sampleIndex;
        } else {
            m_dataEnd = qMax(m_dataEnd, f.sampleIndex);
            m_dataStart = qMin(m_dataStart, f.sampleIndex);
        }

        for (const ChannelSample &s : f.channels) {
            for (ChannelSeries &series : m_series) {
                if (series.id == s.channelId) {
                    series.points.append(QPointF(f.sampleIndex, s.value));
                    series.quality.append(s.quality);
                    series.artifact.append(s.artifact);
                    break;
                }
            }
        }
    }

    // 限制单通道内部点数上限，避免长时间运行内存无上限增长。
    // （主数据的环形缓冲由业务层负责；此处为绘图用的可见窗口副本。）
    constexpr int kMaxPointsPerSeries = 20000;
    for (ChannelSeries &series : m_series) {
        if (series.points.size() > kMaxPointsPerSeries) {
            const int drop = series.points.size() - kMaxPointsPerSeries;
            series.points.remove(0, drop);
            // quality 与 points 必须同步裁剪，否则下标错位。
            if (series.quality.size() > kMaxPointsPerSeries)
                series.quality.remove(0, series.quality.size() - kMaxPointsPerSeries);
            if (series.artifact.size() > kMaxPointsPerSeries)
                series.artifact.remove(0, series.artifact.size() - kMaxPointsPerSeries);
        }
    }

    if (m_followLatest) {
        // 跟随最新：把视口右端对齐到最新序号。
        updateMapper();
        // 实时模式下只重绘数据区，降低 CPU 开销（难点 1 / 难点 5）。
        update(computePlotRect().toRect().adjusted(-1, -1, 1, 1));
    } else {
        update();
    }
}

void TrendChartWidget::appendAlarms(const QVector<AlarmEvent> &alarms)
{
    if (alarms.isEmpty())
        return;
    m_alarms.append(alarms);

    constexpr int kMaxAlarms = 2000;
    if (m_alarms.size() > kMaxAlarms)
        m_alarms.remove(0, m_alarms.size() - kMaxAlarms);

    update();
}

void TrendChartWidget::clearData()
{
    for (ChannelSeries &series : m_series)
        series.points.clear();
    m_alarms.clear();
    m_dataStart = 0;
    m_dataEnd = 0;
    m_followLatest = true;
    updateMapper();
    update();
}


void TrendChartWidget::setFollowLatest(bool on)
{
    if (m_followLatest == on)
        return;
    m_followLatest = on;
    if (on)
        updateMapper();
    update();
}

void TrendChartWidget::setVisibleIndexSpan(quint64 span)
{
    m_visibleSpan = qBound<quint64>(20, span, 100000);
    updateMapper();
    update();
}

void TrendChartWidget::resetViewToLatest()
{
    m_followLatest = true;
    updateMapper();
    update();
}

QString TrendChartWidget::viewportInfo() const
{
    return QStringLiteral("视口 [%1 ~ %2]，跨度 %3 点，%4")
        .arg(m_mapper.viewStartIndex())
        .arg(m_mapper.viewEndIndex())
        .arg(m_mapper.indexSpan())
        .arg(m_followLatest ? QStringLiteral("实时跟随") : QStringLiteral("历史浏览"));
}

QRectF TrendChartWidget::computePlotRect() const
{
    // 多轴模式下右侧需要预留刻度空间，否则右轴文字会超出控件被裁掉。
    int rightMargin = kMarginRight;
    if (m_multiAxis) {
        int visibleCount = 0;
        for (const ChannelConfig &c : m_channels)
            if (c.visible)
                ++visibleCount;
        if (visibleCount / 2 > 0)
            rightMargin = qMax(kMarginRight, 58);
    }

    return QRectF(kMarginLeft,
                  kMarginTop,
                  qMax(1, width()  - kMarginLeft - rightMargin),
                  qMax(1, height() - kMarginTop  - kMarginBottom));
}

void TrendChartWidget::updateMapper()
{
    m_mapper.setPlotRect(computePlotRect());

    quint64 start = 0;
    quint64 end = 0;
    const quint64 span = qMax<quint64>(1, m_visibleSpan);

    if (m_dataEnd == 0 && m_dataStart == 0) {
        end = 0;
        start = 0;
    } else if (m_followLatest) {
        end = m_dataEnd;
        start = (end >= span) ? (end - span + 1) : 0;
    } else {
        end = m_dragStartIndex;
        start = (end >= span) ? (end - span + 1) : 0;
    }

    m_mapper.setIndexRange(start, end);

    for (const ChannelConfig &c : m_channels)
        m_mapper.setValueRange(c.id, c.minValue, c.maxValue);
}

void TrendChartWidget::clampView()
{
    if (m_dragStartIndex < m_dataStart)
        m_dragStartIndex = m_dataStart;
    if (m_dataEnd != 0 && m_dragStartIndex > m_dataEnd)
        m_dragStartIndex = m_dataEnd;
}

// ---------------------------------------------------------------------------
// 绘制部分
// ---------------------------------------------------------------------------

void TrendChartWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    m_backgroundDirty = true;
}

void TrendChartWidget::rebuildStaticBackground()
{
    // 静态背景缓存：把底色、网格、坐标轴刻度预先画到 QPixmap，
    // paintEvent 时直接 drawPixmap，避免每帧重复绘制网格（难点 1）。
    const QSize sz = size();
    if (sz.width() <= 0 || sz.height() <= 0)
        return;

    m_background = QPixmap(sz);
    m_background.fill(kBgColor);

    QPainter p(&m_background);
    p.setRenderHint(QPainter::Antialiasing, false);

    const QRectF plot = computePlotRect();
    p.fillRect(plot, kPlotBgColor);

    drawGrid(p);
    drawAxes(p);

    p.end();
    m_backgroundDirty = false;
}

void TrendChartWidget::drawGrid(QPainter &p)
{
    const QRectF plot = computePlotRect();

    QPen minorPen(kGridColor);
    minorPen.setWidth(1);

    constexpr int kXDivisions = 10;
    for (int i = 0; i <= kXDivisions; ++i) {
        const double x = plot.left() + plot.width() * i / kXDivisions;
        p.setPen(minorPen);
        p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
    }

    constexpr int kYDivisions = 8;
    for (int i = 0; i <= kYDivisions; ++i) {
        const double y = plot.top() + plot.height() * i / kYDivisions;
        p.setPen((i == 0 || i == kYDivisions) ? QPen(kGridMajorCol) : minorPen);
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }
}

void TrendChartWidget::drawAxes(QPainter &p)
{
    const QRectF plot = computePlotRect();

    QPen axisPen(kAxisColor);
    axisPen.setWidth(1);
    p.setPen(axisPen);
    p.drawLine(plot.topLeft(), plot.bottomLeft());
    p.drawLine(plot.bottomLeft(), plot.bottomRight());

    p.setPen(kTextColor);
    QFont f = p.font();
    f.setPointSizeF(8.5);
    p.setFont(f);

    const int divisions = 5;
    for (int i = 0; i <= divisions; ++i) {
        const double x = plot.left() + plot.width() * i / divisions;
        const quint64 idx = m_mapper.viewStartIndex()
                          + m_mapper.indexSpan() * i / divisions;
        const QString label = QStringLiteral("t+%1s").arg(idx / 10);
        p.drawText(QRectF(x - 40, plot.bottom() + 4, 80, 16),
                   Qt::AlignHCenter | Qt::AlignTop, label);
    }

    for (const ChannelConfig &c : m_channels) {
        if (!c.visible)
            continue;
        if (!m_multiAxis) {
            // 旧行为：只画第一个可见通道的刻度。
            p.setPen(c.color);
            const int divisionsY = 4;
            for (int i = 0; i <= divisionsY; ++i) {
                const double ratio = static_cast<double>(i) / divisionsY;
                const double y = plot.bottom() - ratio * plot.height();
                const double v = c.minValue + ratio * (c.maxValue - c.minValue);
                p.drawText(QRectF(4, y - 8, kMarginLeft - 8, 16),
                           Qt::AlignRight | Qt::AlignVCenter,
                           QString::number(v, 'f', 1));
            }
            break;
        }
    }

    if (m_multiAxis)
        drawChannelScales(p);
}

void TrendChartWidget::drawChannelScales(QPainter &p)
{
    // 每通道独立 Y 轴刻度（第二梯队第 10 项）。
    //
    // 多通道量程差异极大（HR 40~180 / SpO2 85~100 / Temp 34~42），
    // 若只显示单轴刻度，其余通道数值无法直接读取，临床易误判。
    //
    // 布局策略：可见通道按顺序交替分配在绘图区左侧与右侧，
    // 左轴右对齐、右轴左对齐，用通道色区分归属；
    // 刻度文字带通道缩写前缀，避免多轴混淆。
    const QRectF plot = computePlotRect();

    // 收集可见通道。
    QVector<const ChannelConfig *> vis;
    for (const ChannelConfig &c : m_channels)
        if (c.visible)
            vis.append(&c);
    if (vis.isEmpty())
        return;

    // 轴边距：通道数 > 2 时右侧也留出轴位（computePlotRect 已预留左侧）。
    constexpr int kRightAxisWidth = 52;
    const int nLeft  = (vis.size() + 1) / 2;   // 左轴通道数
    const int nRight = vis.size() / 2;

    QFont f = p.font();
    f.setPointSizeF(8.0);
    p.setFont(f);

    constexpr int kDivisions = 4;

    for (int ci = 0; ci < vis.size(); ++ci) {
        const ChannelConfig *c = vis.at(ci);
        const bool onLeft = (ci < nLeft);
        // 同一侧的多个通道横向错开，避免刻度文字互相重叠。
        const int slot = onLeft ? ci : (ci - nLeft);
        const int slotsOnSide = onLeft ? nLeft : qMax(1, nRight);
        const int slotWidth = onLeft
                                  ? qMax(18, (kMarginLeft - 6) / qMax(1, slotsOnSide))
                                  : qMax(18, kRightAxisWidth / qMax(1, slotsOnSide));

        p.setPen(c->color);

        for (int i = 0; i <= kDivisions; ++i) {
            const double ratio = static_cast<double>(i) / kDivisions;
            const double y = plot.bottom() - ratio * plot.height();
            const double v = c->minValue + ratio * (c->maxValue - c->minValue);

            if (onLeft) {
                const double x = 2 + slot * slotWidth;
                p.drawText(QRectF(x, y - 8, slotWidth - 2, 16),
                           Qt::AlignRight | Qt::AlignVCenter,
                           QString::number(v, 'f', 1));
            } else {
                const double x = plot.right() + 2 + slot * slotWidth;
                p.drawText(QRectF(x, y - 8, slotWidth - 2, 16),
                           Qt::AlignLeft | Qt::AlignVCenter,
                           QString::number(v, 'f', 1));
            }
        }

        // 在轴顶端标注通道缩写，明确该列刻度属于哪个通道。
        const QString abbr = c->name.section(QLatin1Char(' '), -1);
        p.setPen(c->color);
        if (onLeft) {
            const double x = 2 + slot * slotWidth;
            p.drawText(QRectF(x, plot.top() - 14, slotWidth - 2, 12),
                       Qt::AlignRight | Qt::AlignVCenter, abbr);
        } else {
            const double x = plot.right() + 2 + slot * slotWidth;
            p.drawText(QRectF(x, plot.top() - 14, slotWidth - 2, 12),
                       Qt::AlignLeft | Qt::AlignVCenter, abbr);
        }
    }
}

void TrendChartWidget::setMultiAxisEnabled(bool on)
{
    if (m_multiAxis == on)
        return;
    m_multiAxis = on;
    m_backgroundDirty = true;
    update();
}

void TrendChartWidget::drawCurves(QPainter &p, const QRectF &clipRect)
{
    p.save();
    p.setClipRect(clipRect);
    p.setRenderHint(QPainter::Antialiasing, true);

    const quint64 vStart = m_mapper.viewStartIndex();
    const quint64 vEnd   = m_mapper.viewEndIndex();

    for (const ChannelConfig &cfg : m_channels) {
        if (!cfg.visible)
            continue;

        // 找到对应序列。
        const ChannelSeries *series = nullptr;
        for (const ChannelSeries &s : m_series) {
            if (s.id == cfg.id) { series = &s; break; }
        }
        if (!series || series->points.size() < 2)
            continue;

        QPen pen(cfg.color);
        pen.setWidthF(1.6);
        pen.setJoinStyle(Qt::RoundJoin);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);

        // 只用视口范围内的数据构建折线路径（局部重绘友好）。
        // 同时按信号质量分段：SQI 正常用实线通道色，SQI 过低改用灰色虚线，
        // 提示「该段读数不可靠」（第二梯队第 7 项）。
        constexpr double kLowQ = 0.35;

        QPainterPath goodPath;    ///< 可信数据段
        QPainterPath poorPath;    ///< 不可信数据段

        // 伪迹点单独收集：既不进主线（会把尖峰连成折线、看起来像真实波动），
        // 也不能丢弃（丢弃就等于把测到的异常值藏起来）。
        // 用醒目的空心菱形单独标出，并保持曲线在伪迹处**断开**。
        QVector<QPointF> artifactPoints;

        bool startedGood = false;
        bool startedPoor = false;

        QPointF lastPoint;
        bool haveLast = false;
        bool lastWasPoor = false;

        for (int i = 0; i < series->points.size(); ++i) {
            const QPointF &pt = series->points.at(i);
            const quint64 idx = static_cast<quint64>(pt.x());
            if (idx < vStart || idx > vEnd)
                continue;

            const QPointF px = m_mapper.pointFor(cfg.id, idx, pt.y());
            const double q = (i < series->quality.size()) ? series->quality.at(i) : 1.0;
            const bool poor = (q < kLowQ);
            const bool artifact = (i < series->artifact.size()) && series->artifact.at(i);

            if (artifact) {
                // 记录标记位置，并切断当前折线：
                // 不能让伪迹值参与连线，否则两边正常点会被"拉"向尖峰，
                // 在图上凭空画出一段并不存在的趋势。
                artifactPoints.append(px);
                haveLast = false;   // 强制下一次正常点重新 moveTo
                startedGood = false;
                startedPoor = false;
                continue;
            }

            // 质量状态切换时切断路径，避免把可信与不可信段连成一条线。
            if (haveLast && poor != lastWasPoor) {
                if (poor) {
                    poorPath.moveTo(lastPoint);
                    poorPath.lineTo(px);
                    startedPoor = true;
                    startedGood = false;
                } else {
                    goodPath.moveTo(lastPoint);
                    goodPath.lineTo(px);
                    startedGood = true;
                    startedPoor = false;
                }
            } else if (poor) {
                if (!startedPoor) { poorPath.moveTo(px); startedPoor = true; }
                else poorPath.lineTo(px);
            } else {
                if (!startedGood) { goodPath.moveTo(px); startedGood = true; }
                else goodPath.lineTo(px);
            }

            lastPoint  = px;
            haveLast   = true;
            lastWasPoor= poor;
        }

        if (!goodPath.isEmpty()) {
            p.setPen(pen);
            p.drawPath(goodPath);
        }
        if (!poorPath.isEmpty()) {
            // 灰色虚线：明确表达「此段数据不可信」。
            QPen dashPen(QColor(0x88, 0x90, 0x9C));
            dashPen.setWidthF(1.2);
            dashPen.setStyle(Qt::DashLine);
            p.setPen(dashPen);
            p.drawPath(poorPath);
        }

        // 伪迹标记：空心橙红菱形，比曲线略大，方便在密集折线中辨认。
        if (!artifactPoints.isEmpty()) {
            QPen apPen(QColor(0xFF, 0x9F, 0x43));
            apPen.setWidthF(1.6);
            p.setPen(apPen);
            p.setBrush(Qt::NoBrush);
            for (const QPointF &pt : artifactPoints) {
                const double r = 4.0;
                QPolygonF diamond;
                diamond << QPointF(pt.x(), pt.y() - r) << QPointF(pt.x() + r, pt.y())
                        << QPointF(pt.x(), pt.y() + r) << QPointF(pt.x() - r, pt.y());
                p.drawPolygon(diamond);
            }
        }
    }

    p.restore();
}

void TrendChartWidget::drawAlarmMarkers(QPainter &p, const QRectF &clipRect)
{
    if (m_alarms.isEmpty())
        return;

    p.save();
    p.setClipRect(clipRect);
    p.setRenderHint(QPainter::Antialiasing, true);

    const quint64 vStart = m_mapper.viewStartIndex();
    const quint64 vEnd   = m_mapper.viewEndIndex();

    // 同一采样序号可能有多个通道告警，只画一次标记。
    quint64 lastDrawnIndex = std::numeric_limits<quint64>::max();

    for (const AlarmEvent &a : m_alarms) {
        if (a.sampleIndex < vStart || a.sampleIndex > vEnd)
            continue;

        const ChannelConfig cfg = [&]() {
            ChannelConfig c;
            for (const ChannelConfig &cc : m_channels)
                if (cc.id == a.channelId) { c = cc; break; }
            return c;
        }();

        if (!cfg.visible)
            continue;

        const double x = m_mapper.xForIndex(a.sampleIndex);

        // 顶部画一个告警三角（难点/功能：曲线告警标记）。
        const double ty = clipRect.top() + 6;
        QPolygonF tri;
        tri << QPointF(x, ty)
            << QPointF(x - 5, ty - 8)
            << QPointF(x + 5, ty - 8);

        // 颜色严格对齐 IEC 60601-1-8 色码，并与报警条 / 数值面板共用
        // 同一套定义（alarmIndicatorColor），避免同一告警在不同控件
        // 呈现不同颜色而被误判为两个事件（优化文档 6.1 第 1 项）。
        const QColor markColor = alarmIndicatorColor(a.level);
        p.setBrush(markColor);
        p.setPen(QPen(markColor.darker(140), 1));
        p.drawPolygon(tri);

        // 高优先级告警在曲线上再加一圈外框，强化视觉显著性
        // （IEC 60601-1-8 要求高优先级报警必须比中 / 低优先级更醒目）。
        if (a.level == AlarmEvent::Critical || a.level == AlarmEvent::Escalated) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(0xFF, 0xFF, 0xFF), 1));
            QPolygonF outer;
            outer << QPointF(x, ty + 1.5)
                  << QPointF(x - 6.5, ty - 9.5)
                  << QPointF(x + 6.5, ty - 9.5);
            p.drawPolygon(outer);
        }

        // 在曲线上的实际越限点画一个小圆点。
        if (a.sampleIndex != lastDrawnIndex) {
            const QPointF dot = m_mapper.pointFor(a.channelId, a.sampleIndex, a.value);
            p.setBrush(markColor);
            p.drawEllipse(dot, 2.5, 2.5);
            lastDrawnIndex = a.sampleIndex;
        }
    }

    p.restore();
}

void TrendChartWidget::drawLegend(QPainter &p)
{
    p.save();
    QFont f = p.font();
    f.setPointSizeF(8.5);
    p.setFont(f);

    int x = kMarginLeft + 8;
    const int y = kMarginTop + 6;

    for (const ChannelConfig &c : m_channels) {
        // 颜色小方块
        p.setBrush(c.visible ? c.color : QColor(0x50, 0x58, 0x64));
        p.setPen(Qt::NoPen);
        p.drawRect(x, y, 10, 10);

        p.setPen(c.visible ? kTextColor : QColor(0x60, 0x68, 0x74));
        const QString txt = QStringLiteral("%1(%2)").arg(c.name).arg(c.unit);
        const QRectF tr(x + 14, y - 3, 150, 16);
        p.drawText(tr, Qt::AlignLeft | Qt::AlignVCenter, txt);
        x += 22 + p.fontMetrics().horizontalAdvance(txt) + 14;
    }

    p.restore();
}

void TrendChartWidget::paintEvent(QPaintEvent *event)
{
    QPainter p(this);

    // 1) 背景：直接贴缓存的网格背景（不重复画网格）。
    if (m_backgroundDirty || m_background.size() != size())
        rebuildStaticBackground();
    p.drawPixmap(0, 0, m_background);

    const QRectF plot = computePlotRect();

    // 2) 曲线（局部绘制区域：本次重绘涉及的矩形与绘图区求交）。
    QRectF dirty = plot;
    if (event) {
        const QRect r = event->rect();
        if (r.isValid())
            dirty = plot.intersected(QRectF(r));
    }

    drawCurves(p, dirty);
    drawAlarmMarkers(p, plot);
    drawLegend(p);

    // 3) 左侧当前值提示（越限时高亮）——画在绘图区右端。
    if (!m_followLatest) {
        p.setPen(QColor(0xF3, 0x9C, 0x12));
        p.drawText(QRectF(plot.left(), plot.top() + 4, plot.width() - 8, 16),
                   Qt::AlignRight | Qt::AlignTop,
                   QStringLiteral("历史浏览模式（拖拽平移 / 滚轮缩放）"));
    }
}

// ---------------------------------------------------------------------------
// 交互：缩放 / 平移 / 探针
// ---------------------------------------------------------------------------

void TrendChartWidget::wheelEvent(QWheelEvent *event)
{
    // 滚轮缩放横向时间轴：只改视口跨度，不动原始数据（难点 5）。
    const int delta = event->angleDelta().y();
    if (delta == 0) {
        event->ignore();
        return;
    }

    const double factor = (delta > 0) ? 0.8 : 1.25;
    quint64 newSpan = static_cast<quint64>(m_visibleSpan * factor);
    newSpan = qBound<quint64>(20, newSpan, 100000);

    if (newSpan != m_visibleSpan) {
        m_visibleSpan = newSpan;
        updateMapper();
        emit viewportChanged(m_mapper.viewStartIndex(), m_mapper.viewEndIndex());
        update();
    }
    event->accept();
}

void TrendChartWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_dragLastPos = event->pos();
        // 进入拖拽即切到历史浏览模式，冻结视口跟随。
        if (m_followLatest) {
            m_followLatest = false;
            m_dragStartIndex = m_dataEnd;
        }
        setCursor(Qt::ClosedHandCursor);
    }
    QWidget::mousePressEvent(event);
}

void TrendChartWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging) {
        const int dx = event->pos().x() - m_dragLastPos.x();
        m_dragLastPos = event->pos();

        const double plotWidth = qMax(1.0, computePlotRect().width());
        // 像素位移 -> 采样序号位移（向右拖 = 看更早的数据）。
        const double indexPerPixel = static_cast<double>(m_visibleSpan) / plotWidth;
        const qint64 delta = static_cast<qint64>(-dx * indexPerPixel);

        qint64 target = static_cast<qint64>(m_dragStartIndex) + delta;
        if (target < 0)
            target = 0;
        m_dragStartIndex = static_cast<quint64>(target);
        clampView();

        updateMapper();
        emit viewportChanged(m_mapper.viewStartIndex(), m_mapper.viewEndIndex());
        update();
    } else {
        // 探针：报告鼠标位置的采样序号（供扩展光标探针功能）。
        const QRectF plot = computePlotRect();
        if (plot.contains(event->pos())) {
            const quint64 idx = m_mapper.indexForX(event->pos().x());
            if (!m_cursorValid || idx != m_cursorIndex) {
                m_cursorValid = true;
                m_cursorIndex = idx;
                emit cursorIndexChanged(idx, true);
            }
        } else if (m_cursorValid) {
            m_cursorValid = false;
            emit cursorIndexChanged(0, false);
        }
    }
    QWidget::mouseMoveEvent(event);
}

void TrendChartWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        setCursor(Qt::ArrowCursor);
        // 拖拽结束后做一次完整刷新，避免过程中频繁全量重绘（难点 5）。
        update();
    }
    QWidget::mouseReleaseEvent(event);
}

void TrendChartWidget::leaveEvent(QEvent *event)
{
    if (m_cursorValid) {
        m_cursorValid = false;
        emit cursorIndexChanged(0, false);
    }
    QWidget::leaveEvent(event);
}

bool TrendChartWidget::exportImage(const QString &filePath) const
{
    // 渲染整个控件到图片（含背景网格与曲线）。
    QPixmap pixmap(size());
    pixmap.fill(kBgColor);
    const_cast<TrendChartWidget *>(this)->render(&pixmap);
    return pixmap.save(filePath);
}

