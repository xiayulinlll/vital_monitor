#include "VitalSignSimulator.h"

#include <QDateTime>
#include <QRandomGenerator>
#include <QTimer>
#include <QtMath>

VitalSignSimulator::VitalSignSimulator(QObject *parent)
    : QObject(parent)
{
    // 预置一套默认配置，避免 start() 之前无配置可用。
    ChannelConfigManager mgr;
    m_configs = mgr.channels();

    resizeChannelState();
}

VitalSignSimulator::~VitalSignSimulator()
{
    stop();
}

void VitalSignSimulator::resizeChannelState()
{
    const int n = m_configs.size();

    // 所有「每通道一份」的状态数组必须与通道数严格同步。
    // 原实现只 resize 了 4 个数组，而 m_spikeCountdown 是 int[4] 定长数组，
    // 通道数变化时会越界访问（审核报告 R-1）。统一在这里处理，避免遗漏。
    m_current.resize(n);
    m_phase.resize(n);
    m_quality.resize(n);
    m_poorQualityFrames.resize(n);
    m_spikeCountdown.resize(n);
    m_artifactCountdown.resize(n);

    for (int i = 0; i < n; ++i) {
        const ChannelConfig &c = m_configs.at(i);
        // 以量程中值附近为起点。
        if (m_current.at(i) <= 0.0)
            m_current[i] = (c.minValue + c.maxValue) * 0.5;
        if (qFuzzyIsNull(m_phase.at(i)))
            m_phase[i] = QRandomGenerator::global()->generateDouble() * 6.28318;
        if (m_quality.at(i) <= 0.0)
            m_quality[i] = 1.0;
        m_poorQualityFrames[i] = 0;
        m_spikeCountdown[i] = -1;      // -1 = 不在脉冲中
        m_artifactCountdown[i] = 0;    // 0  = 无待注入伪迹
    }
}

void VitalSignSimulator::start(int intervalMs)
{
    if (!m_timer) {
        m_timer = new QTimer(this);
        // 定时器随对象所属线程创建，因此回调也在工作线程执行。
        m_timer->setTimerType(Qt::PreciseTimer);
        connect(m_timer, &QTimer::timeout, this, &VitalSignSimulator::onTick);
    }
    m_intervalMs = qMax(1, intervalMs);
    m_lastTickMs = 0;
    // 单调时钟在此启动：之后的每一帧都带 monotonicMs。
    m_clock.start();
    m_timer->start(m_intervalMs);
    emit statusText(QStringLiteral("采集已启动，采样周期 %1 ms").arg(m_intervalMs));
}

void VitalSignSimulator::stop()
{
    if (m_timer && m_timer->isActive()) {
        m_timer->stop();
        emit statusText(QStringLiteral("采集已停止"));
    }
}

void VitalSignSimulator::updateConfigs(const QVector<ChannelConfig> &configs)
{
    if (configs.isEmpty())
        return;
    m_configs = configs;
    resizeChannelState();
}

void VitalSignSimulator::reset()
{
    m_sampleIndex = 0;
    m_lastTickMs  = 0;
    m_clock.invalidate();
    m_simulatedDropCountdown = 0;
    for (int &c : m_spikeCountdown)
        c = -1;
    for (int &c : m_artifactCountdown)
        c = 0;
}

void VitalSignSimulator::injectArtifact(int channelId)
{
    for (int i = 0; i < m_configs.size() && i < m_artifactCountdown.size(); ++i) {
        if (channelId < 0 || m_configs.at(i).id == channelId)
            m_artifactCountdown[i] = 1;   // 下一帧注入
    }
    emit statusText(channelId < 0
                        ? QStringLiteral("已请求注入运动伪迹（全部通道）")
                        : QStringLiteral("已请求注入运动伪迹（通道 %1）").arg(channelId));
}

void VitalSignSimulator::onTick()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    SampleFrame frame;
    /// 单调时钟值：不受系统校时影响，供告警计时使用。
    const qint64 monoMs = m_clock.isValid() ? m_clock.elapsed() : 0;
    frame.monotonicMs = monoMs;

    // ---- 数据完整性（第二梯队第 8 项）----
    // 用单调时钟的实际间隔判定丢帧（不能只比较墙钟时间戳：校时会误判），
    // 若实际间隔明显大于采样周期，说明发生了抖动 / 丢帧，
    // 需要如实上报而非静默忽略。
    if (m_lastTickMs > 0 && m_lastMonotonicMs > 0) {
        const qint64 monoDelta = monoMs - m_lastMonotonicMs;
        frame.deltaMs = monoDelta;
        const qint64 expected = m_intervalMs;
        if (monoDelta > expected * 3 / 2) {
            frame.droppedFrames = static_cast<quint64>(
                qMax<qint64>(1, (monoDelta / qMax(1, expected)) - 1));
        }
    }
    m_lastTickMs = nowMs;
    m_lastMonotonicMs = monoMs;

    // ---- 模拟偶发丢帧事件，便于演示「数据不连续」技术报警 ----
    // @note 真实设备中该事件由驱动层上报；此处由模拟器主动制造。
    if (m_simulatedDropCountdown <= 0 && QRandomGenerator::global()->bounded(600) == 0)
        m_simulatedDropCountdown = 3;   // 3 帧后丢弃一帧

    if (m_simulatedDropCountdown > 0) {
        --m_simulatedDropCountdown;
        if (m_simulatedDropCountdown == 0) {
            frame.droppedFrames += 1;
            // 丢弃该帧：序号照常推进，但数据不发出。
            m_sampleIndex++;
            // @note 原实现在 return 前就 emit，且 droppedFrames 随后被丢弃 ——
            //       上层因此永远收不到「丢帧」这一事实。现在先发出一个
            //       「空数据但带 droppedFrames」的帧，让技术报警能真正触发。
            frame.sampleIndex = m_sampleIndex;
            frame.timestampMs = nowMs;
            emit frameReady(frame);
            emit heartbeat(m_sampleIndex);
            emit statusText(QStringLiteral("检测到 1 帧数据丢失（模拟）"));
            return;
        }
    }

    frame.sampleIndex = m_sampleIndex++;
    frame.timestampMs = nowMs;

    for (int i = 0; i < m_configs.size(); ++i) {
        ChannelSample s;
        s.channelId = m_configs.at(i).id;
        s.value     = nextValueFor(m_configs.at(i));
        s.quality   = m_quality.at(i);
        // SQI 极低时把该点标记为无效测量（等价于探头未测得有效信号）。
        s.invalid   = (m_quality.at(i) < 0.15);

        // ---- 伪迹标记（优化文档 3.2 第 4 项）----
        // 关键：伪迹是「测到的值物理上不可能」，与「测不到」是两回事。
        // 这里只**标记**，不改写数值 —— 若像旧实现那样直接 clamp 到量程，
        // HR 250 会被裁成 180，真实危重信息就被静默掩盖了。
        if (i < m_artifactCountdown.size() && m_artifactCountdown.at(i) > 0) {
            s.artifact = true;
            --m_artifactCountdown[i];
        }

        frame.channels.append(s);
    }

    emit frameReady(frame);

    // 心跳：采样序号单调递增即代表采集链路在推进（对应 7.1 第 6 项）。
    emit heartbeat(frame.sampleIndex);
}

double VitalSignSimulator::nextValueFor(const ChannelConfig &cfg)
{
    // @note 必须按「通道下标」索引状态数组，而不是按 cfg.id。
    //       原实现直接用 cfg.id 当下标：若配置里 id 不连续（如 0,1,3),
    //       或 id ≥ 通道数，就会越界读写（未定义行为 / 崩溃）。
    //       这里统一通过 id 查找下标；找不到则返回量程中值（不写任何状态）。
    int idx = -1;
    for (int i = 0; i < m_configs.size(); ++i) {
        if (m_configs.at(i).id == cfg.id) {
            idx = i;
            break;
        }
    }
    if (idx < 0 || idx >= m_current.size() || idx >= m_phase.size()
        || idx >= m_spikeCountdown.size())
        return (cfg.minValue + cfg.maxValue) * 0.5;

    // 使用“缓慢随机游走 + 周期分量”叠加，模拟真实体征的平滑波动。
    m_phase[idx] += 0.05;
    if (m_phase[idx] > 6.28318)
        m_phase[idx] -= 6.28318;

    const double span = qMax(1.0, cfg.maxValue - cfg.minValue);
    const double wave = qSin(m_phase[idx]) * span * 0.03;   // 周期波动
    const double noise = (QRandomGenerator::global()->generateDouble() - 0.5) * span * 0.01;

    // 让数值缓慢回归量程中心，避免长期漂移出界。
    const double center = (cfg.minValue + cfg.maxValue) * 0.5;
    m_current[idx] += (center - m_current[idx]) * 0.02;

    // 偶发越限：每隔一段时间制造一次“异常事件”，用于演示告警标记。
    if (m_spikeCountdown[idx] < 0 && QRandomGenerator::global()->bounded(300) == 0)
        m_spikeCountdown[idx] = 15; // 持续约 15 个采样周期

    double spike = 0.0;
    if (m_spikeCountdown[idx] > 0) {
        --m_spikeCountdown[idx];
        // 超过上限的冲动（如心率骤升 / 体温升高）。
        spike = span * 0.18;
        if (m_spikeCountdown[idx] == 0)
            m_spikeCountdown[idx] = -1;
    }

    // ---- 模拟信号质量劣化（第二梯队第 7 项）----
    // 真实设备中 SQI 由波形形态 / 灌注指数等计算；此处用「偶发低质量窗口」
    // 模拟指夹脱落、末梢灌注不足、电极干扰等场景，使 SQI 相关逻辑可测试。
    if (idx < m_poorQualityFrames.size()) {
        if (m_poorQualityFrames[idx] <= 0
            && QRandomGenerator::global()->bounded(500) == 0) {
            m_poorQualityFrames[idx] = 20;   // 低质量窗口长度（采样周期数）
        }

        if (m_poorQualityFrames[idx] > 0) {
            --m_poorQualityFrames[idx];
            // 在窗口内让质量在 [0.05, 0.30] 之间波动（低于 0.35 抑制阈值）。
            m_quality[idx] = 0.05 + QRandomGenerator::global()->generateDouble() * 0.25;
            // 低质量期间读数本身也不可信，叠加较大噪声。
            if (idx < m_current.size())
                m_current[idx] += (QRandomGenerator::global()->generateDouble() - 0.5) * span * 0.06;
        } else {
            // 缓慢恢复，避免质量值跳变。
            m_quality[idx] += (1.0 - m_quality[idx]) * 0.2;
            if (m_quality[idx] > 0.999)
                m_quality[idx] = 1.0;
        }
    }

    double v = m_current[idx] + wave + noise + spike;

    // ---- 不掩盖异常值（优化文档 3.2 第 3 项）----
    // 旧实现用 cfg.clamp(v) 把数值裁到量程内：一旦真实值超出量程
    // （如 HR 220 > maxValue 180），屏幕永远只显示 180，
    // 「数值异常」这一事实被静默抹掉，属于典型的「compensate 掩盖」。
    //
    // 现在改为：保留原始值，只把**内部状态**做软限幅，防止随机游走
    // 无限漂移导致长期失真。屏幕显示 / 告警判断都使用原始值，
    // 由上层（VitalValuePanel）负责「超量程」的视觉标记。
    m_current[idx] = qBound(cfg.minValue - span * 0.25,
                            v,
                            cfg.maxValue + span * 0.25);
    return v;
}
