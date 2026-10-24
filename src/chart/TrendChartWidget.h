#ifndef TRENDCHARTWIDGET_H
#define TRENDCHARTWIDGET_H

#include <QWidget>
#include <QPixmap>
#include <QVector>
#include <QPoint>

#include "data/SampleData.h"
#include "core/ChannelConfig.h"
#include "core/CoordinateMapper.h"

/**
 * @brief 多通道生理信号趋势绘图控件（对应架构 2.3，QPainter 绘图控件层）。
 *
 * 全部使用 QPainter 原生绘制，无 OpenGL、无第三方绘图库。
 *
 * 关键优化（对应难点 1、难点 5）：
 *  - 静态网格背景预渲染到 QPixmap 缓存，paintEvent 直接贴图，不重复画网格；
 *  - 支持局部重绘（updatePlotRect），实时模式下只刷新新增数据区域；
 *  - 交互（缩放/平移）只改视口参数，不触碰原始数据。
 */
class TrendChartWidget : public QWidget
{
    Q_OBJECT

public:
    explicit TrendChartWidget(QWidget *parent = nullptr);
    ~TrendChartWidget() override;

    /// 设置通道配置（名称/颜色/量程/可见性）。
    void setChannels(const QVector<ChannelConfig> &channels);

    /**
     * @brief 追加一批新采样数据（实时模式）。
     *
     * 控件内部维护各通道曲线点列，并根据是否"跟随最新"决定视口移动。
     */
    void appendFrames(const QVector<SampleFrame> &frames);

    /// 追加告警事件（用于在曲线上画三角标记）。
    void appendAlarms(const QVector<AlarmEvent> &alarms);

    /// 清空所有曲线与告警数据。
    void clearData();

    /// 是否自动跟随最新数据（实时模式）。关闭后为历史浏览模式。
    void setFollowLatest(bool on);
    bool followLatest() const { return m_followLatest; }

    /// 设置视口宽度（显示的采样点数）。
    void setVisibleIndexSpan(quint64 span);
    quint64 visibleIndexSpan() const { return m_visibleSpan; }

    /// 把视口复位到最新数据。
    void resetViewToLatest();

    /// 导出当前画布为图片。
    bool exportImage(const QString &filePath) const;

    /// 供状态栏显示：当前视口信息。
    QString viewportInfo() const;

signals:
    /// 视口范围变化（缩放/平移）时发出，供外部同步状态。
    void viewportChanged(quint64 startIndex, quint64 endIndex);
    /// 鼠标悬停在某个采样序号上（用于未来探针扩展）。
    void cursorIndexChanged(quint64 index, bool valid);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    struct ChannelSeries {
        int              id = -1;
        QVector<QPointF> points;   ///< (sampleIndex, value) —— 存原始值，非像素
        /// 与 points 一一对应的信号质量（0~1）。
        /// 用于把「信号不可靠」的曲线段以灰色虚线绘制，避免误读。
        QVector<double>  quality;

        /// 与 points 一一对应的伪迹标记。
        /// 伪迹点必须单独标记：它的数值可能「看起来像个真实的越限」，
        /// 但实际是运动 / 电极干扰造成的。不标记就会被误读为病情恶化。
        QVector<bool>    artifact;
    };

    void rebuildStaticBackground();
    QRectF computePlotRect() const;
    void updateMapper();
    void drawGrid(QPainter &p);
    void drawAxes(QPainter &p);
    void drawCurves(QPainter &p, const QRectF &clipRect);
    void drawAlarmMarkers(QPainter &p, const QRectF &clipRect);
    void drawLegend(QPainter &p);
    /// 绘制每个可见通道的独立 Y 轴刻度（左侧右对齐 / 右侧左对齐，按通道分配）。
    void drawChannelScales(QPainter &p);
    void clampView();

    QVector<ChannelConfig>  m_channels;
    QVector<ChannelSeries>  m_series;
    QVector<AlarmEvent>     m_alarms;

    CoordinateMapper m_mapper;
    QPixmap          m_background;     ///< 静态网格背景缓存
    bool             m_backgroundDirty = true;

    /// 是否启用「每通道独立 Y 轴刻度」（第二梯队第 10 项）。
    /// 关闭时退回「只显示第一个可见通道刻度」的旧行为。
    bool m_multiAxis = true;

public:
    /// 开启 / 关闭每通道独立 Y 轴刻度。
    void setMultiAxisEnabled(bool on);
    bool isMultiAxisEnabled() const { return m_multiAxis; }

private:

    bool    m_followLatest = true;
    quint64 m_visibleSpan  = 600;      ///< 默认显示最近 600 个采样点
    quint64 m_dataStart    = 0;        ///< 当前数据最旧序号
    quint64 m_dataEnd      = 0;        ///< 当前数据最新序号

    // 交互状态
    bool    m_dragging = false;
    QPoint  m_dragLastPos;
    quint64 m_dragStartIndex = 0;
    bool    m_needFullRepaint = false;

    // 探针
    bool    m_cursorValid = false;
    quint64 m_cursorIndex = 0;
};

#endif // TRENDCHARTWIDGET_H
