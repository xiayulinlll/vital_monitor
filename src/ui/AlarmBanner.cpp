#include "AlarmBanner.h"

#include <QApplication>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace {

/// 严重度对应的底色（视觉报警级别：Critical 红 / Warning 黄 / Info 青）。
/// 复用 SampleData.h 中的统一定义，保证与曲线标记 / 数值面板完全一致。
QColor levelColor(AlarmEvent::Level lv)
{
    return alarmIndicatorColor(lv);
}

/// 严重度对应的中文名（用于横幅文案）。
QString levelName(AlarmEvent::Level lv)
{
    switch (lv) {
    case AlarmEvent::Critical:  return QStringLiteral("严重");
    case AlarmEvent::Escalated: return QStringLiteral("升级");
    case AlarmEvent::Warning:   return QStringLiteral("一般");
    case AlarmEvent::Info:
    default:                    return QStringLiteral("提示");
    }
}

/// 闪烁周期：严重度越高闪得越快（符合 IEC 60601-1-8 分级提示思路）。
int blinkIntervalMs(AlarmEvent::Level lv)
{
    switch (lv) {
    case AlarmEvent::Critical:
    case AlarmEvent::Escalated: return 300;
    case AlarmEvent::Warning:   return 550;
    default:                    return 900;
    }
}

/// 提示音重复周期：严重度越高越急促。
int soundIntervalMs(AlarmEvent::Level lv)
{
    switch (lv) {
    case AlarmEvent::Critical:
    case AlarmEvent::Escalated: return 900;
    case AlarmEvent::Warning:   return 1800;
    default:                    return 3000;
    }
}

} // namespace

/**
 * @brief 报警横幅的「彩色条」自绘部件。
 *
 * 之所以把它拆成独立的私有类而不是让 AlarmBanner 自己画：
 * AlarmBanner 现在还承载「活动报警列表」，若仍由父控件自绘底色，
 * 整块区域（含表格）都会被涂成报警色，表格文字将不可读。
 * 拆分后职责清晰：条 = 声光视觉，表 = 结构化明细。
 */
class AlarmStripWidget : public QWidget
{
public:
    explicit AlarmStripWidget(AlarmBanner *owner)
        : QWidget(owner), m_owner(owner)
    {
    }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    AlarmBanner *m_owner = nullptr;
};

AlarmBanner::AlarmBanner(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("AlarmBanner"));
    setMinimumHeight(34);

    // 根布局纵向：上面是自绘横幅，下面是报警列表。
    auto *rootLay = new QVBoxLayout(this);
    rootLay->setContentsMargins(0, 0, 0, 0);
    rootLay->setSpacing(0);

    // 横幅行独立成子控件：paintEvent 只给它上色，不会把列表也涂成红色。
    auto *banner = new AlarmStripWidget(this);
    banner->setObjectName(QStringLiteral("AlarmBannerStrip"));
    banner->setMinimumHeight(34);
    rootLay->addWidget(banner);

    auto *lay = new QHBoxLayout(banner);
    lay->setContentsMargins(10, 4, 10, 4);
    lay->setSpacing(8);

    m_textLabel = new QLabel(banner);
    QFont tf = m_textLabel->font();
    tf.setBold(true);
    tf.setPointSizeF(tf.pointSizeF() + 0.5);
    m_textLabel->setFont(tf);
    m_textLabel->setStyleSheet(QStringLiteral("color:#FFFFFF; background:transparent;"));
    lay->addWidget(m_textLabel, 1);

    m_ackBtn = new QPushButton(QStringLiteral("确认"), banner);
    m_ackBtn->setFixedWidth(70);
    m_ackBtn->setToolTip(QStringLiteral("确认并静音当前报警（告警条件仍持续时会按周期再次提醒）"));
    lay->addWidget(m_ackBtn);

    m_muteBtn = new QPushButton(QStringLiteral("静音: 关"), banner);
    m_muteBtn->setCheckable(true);
    m_muteBtn->setFixedWidth(84);
    m_muteBtn->setToolTip(QStringLiteral("关闭/开启声音提示（不影响视觉报警）"));
    lay->addWidget(m_muteBtn);

    // 暂停按钮：声音 + 闪烁全部停止，但横幅仍然显示（不能被「藏起来」）。
    m_pauseBtn = new QPushButton(QStringLiteral("暂停"), banner);
    m_pauseBtn->setCheckable(true);
    m_pauseBtn->setFixedWidth(84);
    m_pauseBtn->setToolTip(QStringLiteral(
        "暂停报警提示一段时间（到时自动恢复）。危急报警最长 %1 秒，"
        "其它最长 %2 秒。暂停期间横幅仍显示剩余时间。")
        .arg(kCriticalPauseMaxSeconds)
        .arg(kNormalPauseMaxSeconds));
    lay->addWidget(m_pauseBtn);

    m_blinkTimer = new QTimer(this);
    connect(m_blinkTimer, &QTimer::timeout, this, &AlarmBanner::onBlinkTick);

    m_soundTimer = new QTimer(this);
    connect(m_soundTimer, &QTimer::timeout, this, &AlarmBanner::onSoundTick);

    m_pauseTimer = new QTimer(this);
    m_pauseTimer->setInterval(1000);   // 秒级倒计时
    connect(m_pauseTimer, &QTimer::timeout, this, &AlarmBanner::onPauseTick);

    connect(m_ackBtn, &QPushButton::clicked,
            this, &AlarmBanner::acknowledgeRequested);
    connect(m_muteBtn, &QPushButton::toggled, this, [this](bool on) {
        m_muteBtn->setText(on ? QStringLiteral("静音: 开") : QStringLiteral("静音: 关"));
        setMuted(on);
    });
    connect(m_pauseBtn, &QPushButton::toggled, this, [this](bool on) {
        if (on)
            startPause(kNormalPauseMaxSeconds);
        else
            cancelPause();
    });

    // ---- 活动报警列表（9.1 第 1、2 项：报警条 + 优先级排序 + 持续时长）----
    m_list = new QTableWidget(this);
    m_list->setColumnCount(5);
    m_list->setHorizontalHeaderLabels({
        QStringLiteral("优先级"),
        QStringLiteral("类别"),
        QStringLiteral("内容"),
        QStringLiteral("持续"),
        QStringLiteral("状态")
    });
    m_list->verticalHeader()->setVisible(false);
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setShowGrid(false);
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setMaximumHeight(120);
    m_list->setStyleSheet(QStringLiteral(
        "QTableWidget { background:#12161D; color:#D5DDE8; border:none; }"
        "QHeaderView::section { background:#1B2029; color:#8A97A8; border:none;"
        " padding:2px 4px; }"));
    QHeaderView *lh = m_list->horizontalHeader();
    lh->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    lh->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    lh->setSectionResizeMode(2, QHeaderView::Stretch);
    lh->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    lh->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    rootLay->addWidget(m_list);
    m_list->setVisible(false);

    setVisible(false);
}

AlarmBanner::~AlarmBanner() = default;

void AlarmBanner::setAlarmDurations(const QHash<QString, qint64> &durationsMs)
{
    m_durations = durationsMs;
    // 持续时长每秒都在变（由 MainWindow 定时调用），但列表不需要每秒重建：
    // 只在行数或内容确实变化时刷新，避免无谓的控件重建开销。
    refreshList();
}

int AlarmBanner::listedAlarmCount() const
{
    return (m_list && m_list->isVisible()) ? m_list->rowCount() : 0;
}

int AlarmBanner::startPause(int maxSeconds)
{
    // 没有活动告警时无需暂停；返回 0 让调用方知道未生效。
    if (m_alarms.isEmpty())
        return 0;

    // 危急值不允许长时间静默：强制收紧上限（IEC 60601-1-8）。
    // 否则操作者可以用 120 秒「暂停」把一条危及生命的报警长期按住。
    int limit = (topLevel() >= AlarmEvent::Critical)
                    ? kCriticalPauseMaxSeconds
                    : kNormalPauseMaxSeconds;
    const int seconds = qBound(1, qMin(maxSeconds, limit), limit);

    m_paused         = true;
    m_pauseRemaining = seconds;

    m_blinkTimer->stop();
    m_soundTimer->stop();
    m_blinkOn = true;        // 暂停期间保持常亮，避免「闪到一半停住」的视觉歧义
    m_pauseTimer->start();

    m_pauseBtn->blockSignals(true);
    m_pauseBtn->setChecked(true);
    m_pauseBtn->setText(QStringLiteral("恢复"));
    m_pauseBtn->blockSignals(false);

    emit pauseChanged(true, seconds);
    refresh();
    return seconds;
}

void AlarmBanner::cancelPause()
{
    if (!m_paused)
        return;

    m_paused         = false;
    m_pauseRemaining = 0;
    m_pauseTimer->stop();

    m_pauseBtn->blockSignals(true);
    m_pauseBtn->setChecked(false);
    m_pauseBtn->setText(QStringLiteral("暂停"));
    m_pauseBtn->blockSignals(false);

    emit pauseChanged(false, 0);
    // 恢复后立即按当前告警重新提示；refresh() 会重启闪烁与声音。
    refresh();
}

int AlarmBanner::pauseRemainingSeconds() const
{
    return m_paused ? m_pauseRemaining : 0;
}

void AlarmBanner::onPauseTick()
{
    if (!m_paused) {
        m_pauseTimer->stop();
        return;
    }

    if (--m_pauseRemaining <= 0) {
        // 到期自动恢复：规范要求不得无限期静默。
        cancelPause();
        return;
    }
    refresh();   // 刷新倒计时文案与进度条
}

void AlarmBanner::setMuted(bool muted)
{
    if (m_muted == muted)
        return;
    m_muted = muted;
    if (m_muted || m_paused)
        m_soundTimer->stop();
    else if (hasAlarm())
        m_soundTimer->start(soundIntervalMs(topLevel()));
    emit mutedChanged(m_muted);
}

AlarmEvent::Level AlarmBanner::topLevel() const
{
    AlarmEvent::Level best = AlarmEvent::Info;
    bool found = false;
    for (const AlarmEvent &a : m_alarms) {
        if (!found || a.level > best) {
            best  = a.level;
            found = true;
        }
    }
    return found ? best : AlarmEvent::Info;
}

void AlarmBanner::setActiveAlarms(const QVector<AlarmEvent> &alarms)
{
    m_alarms = alarms;
    refresh();
    refreshList();
}

void AlarmBanner::refreshList()
{
    if (!m_list)
        return;

    if (m_alarms.isEmpty()) {
        m_list->setRowCount(0);
        m_list->setVisible(false);
        return;
    }

    // ---- 排序（9.1 第 2 项）----
    // 1) 严重度降序：危及生命的先看到；
    // 2) 同一严重度下「未确认」优先：已确认的说明有人处理了；
    // 3) 再按持续时间降序：拖得越久的越可疑。
    QVector<AlarmEvent> sorted = m_alarms;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [this](const AlarmEvent &a, const AlarmEvent &b) {
        if (a.level != b.level)
            return static_cast<int>(a.level) > static_cast<int>(b.level);

        const bool aAcked = (a.state == AlarmEvent::Acknowledged
                             || a.state == AlarmEvent::LatchedSilent);
        const bool bAcked = (b.state == AlarmEvent::Acknowledged
                             || b.state == AlarmEvent::LatchedSilent);
        if (aAcked != bAcked)
            return !aAcked;

        // 用 trackKey（跟踪键）而非 conditionKey()：技术报警的键是
        // "1/2/SQI" 这类，conditionKey() 只能生成生理告警键，查不到时长。
        return m_durations.value(a.trackKey, 0)
             > m_durations.value(b.trackKey, 0);
    });

    m_list->setRowCount(sorted.size());
    for (int row = 0; row < sorted.size(); ++row) {
        const AlarmEvent &a = sorted.at(row);

        auto *prioItem = new QTableWidgetItem(levelName(a.level));
        prioItem->setForeground(levelColor(a.level));
        m_list->setItem(row, 0, prioItem);

        m_list->setItem(row, 1, new QTableWidgetItem(
            a.category == AlarmEvent::Technical ? QStringLiteral("技术")
                                                : QStringLiteral("生理")));

        m_list->setItem(row, 2, new QTableWidgetItem(a.text));

        const qint64 ms = m_durations.value(a.trackKey, -1);
        m_list->setItem(row, 3, new QTableWidgetItem(
            ms < 0 ? QStringLiteral("—")
                   : QStringLiteral("%1 s").arg(ms / 1000.0, 0, 'f', 1)));

        QString stateText;
        switch (a.state) {
        case AlarmEvent::Active:        stateText = QStringLiteral("未确认"); break;
        case AlarmEvent::Acknowledged:  stateText = QStringLiteral("已确认"); break;
        case AlarmEvent::LatchedSilent: stateText = QStringLiteral("静音待重报"); break;
        case AlarmEvent::Cleared:       stateText = QStringLiteral("已解除"); break;
        }
        m_list->setItem(row, 4, new QTableWidgetItem(stateText));
    }

    m_list->setVisible(true);
}

void AlarmBanner::refresh()
{
    if (m_alarms.isEmpty()) {
        m_blinkTimer->stop();
        m_soundTimer->stop();
        m_blinkOn = true;
        // 告警全部解除后，暂停状态失去意义，必须一并清除，
        // 否则下一条新告警会「继承」上一次的暂停而无人知晓。
        if (m_paused)
            cancelPause();
        if (m_list)
            m_list->setVisible(false);
        setVisible(false);
        update();
        return;
    }

    const AlarmEvent::Level lv = topLevel();

    // 文案：最高严重度 + 活动告警条数 + 首条摘要。
    // 暂停期间必须把「暂停中 + 剩余秒数」写进横幅本身，
    // 让任何人一眼就能看出显示被抑制了多久（不得隐藏抑制状态）。
    QString text = QStringLiteral("[%1报警] 活动 %2 条 — %3")
                       .arg(levelName(lv))
                       .arg(m_alarms.size())
                       .arg(m_alarms.first().text);
    if (m_paused)
        text += QStringLiteral("　⏸ 已暂停 %1 秒").arg(m_pauseRemaining);
    m_textLabel->setText(text);

    if (m_paused) {
        // 暂停：闪烁与声音都停止，但横幅保持可见。
        m_blinkTimer->stop();
        m_soundTimer->stop();
        m_blinkOn = true;
        setVisible(true);
        update();
        return;
    }

    // 视觉：按严重度设定闪烁节奏。
    m_blinkTimer->start(blinkIntervalMs(lv));

    // 听觉：未静音时按严重度节奏发声。
    if (!m_muted && !m_soundTimer->isActive())
        m_soundTimer->start(soundIntervalMs(lv));

    setVisible(true);
    update();
}

void AlarmBanner::onBlinkTick()
{
    m_blinkOn = !m_blinkOn;
    update();
}

void AlarmBanner::onSoundTick()
{
    if (m_muted || m_alarms.isEmpty())
        return;

    // 严重度越高，连响次数越多（简易分级声提示）。
    const int beeps = (topLevel() >= AlarmEvent::Critical) ? 3 : 1;
    for (int i = 0; i < beeps; ++i)
        QApplication::beep();
}

void AlarmStripWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    // 所有状态都取自 owner：颜色 / 闪烁相位 / 是否暂停，
    // 这样 AlarmBanner 不必暴露额外接口，绘制逻辑也只有一份。
    QPainter p(this);
    const AlarmEvent::Level lv = m_owner->topLevel();
    QColor c = alarmIndicatorColor(lv);

    // 闪烁：暗相时降低亮度，形成明显视觉冲击。
    if (!m_owner->m_blinkOn)
        c = c.darker(180);

    // 暂停：整体转为灰暗并画斜线底纹。
    // 目的不是「隐藏报警」，而是让操作者一眼看出「此刻显示是被抑制的」——
    // 如果暂停时仍保持全彩横幅，使用者会误以为是正常报警在响；
    // 如果完全隐藏，则等同于把报警关掉（违反 IEC 60601-1-8）。
    if (m_owner->m_paused)
        c = QColor(0x5A, 0x5F, 0x66);

    p.fillRect(rect(), c);

    if (m_owner->m_paused) {
        // 45° 斜线底纹：视觉上代表「已抑制 / 待恢复」。
        p.setPen(QColor(0xFF, 0xFF, 0xFF, 40));
        for (int x = -height(); x < width(); x += 14)
            p.drawLine(x, height(), x + height(), 0);
    }

    // 左侧 4px 高亮条，强化边界识别。
    p.fillRect(QRect(0, 0, 4, height()),
               QColor(0xFF, 0xFF, 0xFF, m_owner->m_blinkOn ? 220 : 90));

    // 暂停进度条：底部按剩余比例显示，直观体现「多久后自动恢复」。
    if (m_owner->m_paused && m_owner->m_pauseRemaining > 0) {
        const int limit = (lv >= AlarmEvent::Critical)
                              ? AlarmBanner::kCriticalPauseMaxSeconds
                              : AlarmBanner::kNormalPauseMaxSeconds;
        const double frac = qBound(0.0, double(m_owner->m_pauseRemaining) / qMax(1, limit), 1.0);
        p.fillRect(QRect(0, height() - 3, int(width() * frac), 3),
                   QColor(0xFF, 0xD1, 0x66));
    }

    // 底部细描边，与绘图区区分。
    p.setPen(QColor(0, 0, 0, 120));
    p.drawLine(0, height() - 1, width(), height() - 1);
}
