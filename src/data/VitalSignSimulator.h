#ifndef VITALSIGNSIMULATOR_H
#define VITALSIGNSIMULATOR_H

#include <QObject>
#include <QVector>
#include <QAtomicInt>
#include <QElapsedTimer>

#include "data/SampleData.h"
#include "core/ChannelConfig.h"

class QTimer;

/**
 * @brief 生理信号模拟器（对应架构 2.1 数据模拟采集层）。
 *
 * 运行在**后台子线程**中，周期性生成多通道模拟体征数据，
 * 通过信号 sendFrame() 把采样帧投递到主线程业务层。
 *
 * 线程模型（对应难点 3）：
 *  - 本对象 moveToThread() 到工作线程；
 *  - 定时器在该线程内触发，只负责“生产数据”，绝不触碰任何 UI 对象；
 *  - 与主线程之间仅通过 QueuedConnection 信号槽通信，天然线程安全。
 */
class VitalSignSimulator : public QObject
{
    Q_OBJECT

public:
    explicit VitalSignSimulator(QObject *parent = nullptr);
    ~VitalSignSimulator() override;

public slots:
    /// 启动采集（应在工作线程内被调用）。
    void start(int intervalMs = 100);
    /// 停止采集。
    void stop();
    /// 更新通道配置（阈值/量程变化后同步，用于模拟越限）。
    void updateConfigs(const QVector<ChannelConfig> &configs);
    /// 重置采样序号发生器。
    void reset();

    /**
     * @brief 注入一次「运动伪迹」（用于演示生理合理性校验，优化文档 3.2 第 4 项）。
     *
     * 真实设备中伪迹来自电极移动 / 患者运动，表现为单帧数值跳变；
     * 此处由 UI 主动制造，便于演示「伪迹被识别并标记，而不是被
     * clamp 静默裁剪为量程上下限」的差异。
     *
     * @param channelId 目标通道；传 -1 表示全部通道。
     */
    void injectArtifact(int channelId = -1);

signals:
    /// 产生一帧多通道采样数据。
    void frameReady(const SampleFrame &frame);
    /// 采集线程内部状态文本（供状态栏显示）。
    void statusText(const QString &text);

    /**
     * @brief 采集线程心跳（对应优化文档 7.1 第 6 项「看门狗 / 心跳监控」）。
     *
     * @param sampleIndex 当前采样序号 —— 主线程据此判断采集是否**真的在推进**，
     *                    而不仅仅是线程还活着（线程卡死时定时器仍在跑）。
     *
     * 与 frameReady 分开的原因：frameReady 只在模拟器决定产帧时发出，
     * 而心跳用于回答「采集链路是否停滞」，两者语义不同，不能混用。
     */
    void heartbeat(quint64 sampleIndex);

private slots:
    void onTick();

private:
    double nextValueFor(const ChannelConfig &cfg);

    /// 让所有「每通道一份」的状态数组与 m_configs 长度保持一致。
    void resizeChannelState();

    QTimer        *m_timer = nullptr;
    QVector<ChannelConfig> m_configs;
    quint64        m_sampleIndex = 0;
    int            m_intervalMs  = 100;      ///< 当前采样周期（用于估算丢帧）

    // 每个通道使用独立的随机游走状态，保证曲线连续、形态自然。
    QVector<double> m_current;
    QVector<double> m_phase;

    /// 每通道信号质量（0~1）与"低质量剩余帧数"，用于模拟偶发低灌注 / 导联干扰。
    QVector<double> m_quality;
    QVector<int>    m_poorQualityFrames;

    /// 用于模拟偶发的越限事件（例如运动后心率骤升）。
    ///
    /// @note 原实现为 `int m_spikeCountdown[4]`：数组长度与通道数无关联，
    ///       一旦通道数 ≠ 4 就会越界写（内存破坏，且难以复现）。
    ///       改为 QVector 后长度始终与 m_configs 同步（审核报告 R-1）。
    QVector<int>    m_spikeCountdown;

    /// 待注入的伪迹通道（idx = 通道下标，值 = 剩余帧数；0 表示未挂起）。
    QVector<int>    m_artifactCountdown;

    /// 上一帧发出时刻（毫秒），用于数据完整性校验（第二梯队第 8 项）。
    qint64 m_lastTickMs = 0;
    /// 上一帧发出的单调时钟值（毫秒），用于数据完整性校验。
    qint64 m_lastMonotonicMs = 0;
    /// 模拟丢帧：计数达到该值后跳过一帧并计入 droppedFrames。
    int m_simulatedDropCountdown = 0;

    /// 单调时钟：只受硬件计数器影响，不受系统时间（NTP）校时影响。
    /// 每个采样帧都带上 elapsed()，闭环上层的「抖动补偿 / 计时」。
    QElapsedTimer m_clock;
};

#endif // VITALSIGNSIMULATOR_H
