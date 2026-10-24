#ifndef RINGBUFFER_H
#define RINGBUFFER_H

#include <QReadWriteLock>
#include <QVector>
#include <algorithm>

/**
 * @brief 线程安全定长环形缓冲区（模板）。
 *
 * 设计要点（对应项目难点 2）：
 *  - 容量固定，写入超过容量时自动淘汰最旧数据，内存占用恒定；
 *  - 使用 QReadWriteLock 保护，支持多读单写的并发访问；
 *  - 供业务层缓存历史采样点 / 告警事件，适合 7x24 小时连续运行。
 *
 * 说明：写入一般在主线程（业务层）进行，读取在绘图 / 回放时进行，
 *       因此采用读写锁而非互斥锁，读取时可并发。
 */
template <typename T>
class RingBuffer
{
public:
    explicit RingBuffer(int capacity = 1024)
        : m_capacity(qMax(1, capacity))
    {
        m_data.reserve(m_capacity);
    }

    /// 设置容量；若缩小则丢弃最旧的数据。
    void setCapacity(int capacity)
    {
        QWriteLocker locker(&m_lock);
        m_capacity = qMax(1, capacity);
        while (m_data.size() > m_capacity)
            m_data.removeFirst();
        m_data.reserve(m_capacity);
    }

    int capacity() const
    {
        QReadLocker locker(&m_lock);
        return m_capacity;
    }

    /// 当前保存的元素个数。
    int size() const
    {
        QReadLocker locker(&m_lock);
        return m_data.size();
    }

    bool isEmpty() const
    {
        QReadLocker locker(&m_lock);
        return m_data.isEmpty();
    }

    /// 追加一个元素；满则淘汰最旧的。
    void append(const T &value)
    {
        QWriteLocker locker(&m_lock);
        if (m_data.size() >= m_capacity)
            m_data.removeFirst();
        m_data.append(value);
    }

    /// 批量追加（减少加锁次数，提升吞吐）。
    void append(const QVector<T> &values)
    {
        QWriteLocker locker(&m_lock);
        for (const T &v : values) {
            if (m_data.size() >= m_capacity)
                m_data.removeFirst();
            m_data.append(v);
        }
    }

    void clear()
    {
        QWriteLocker locker(&m_lock);
        m_data.clear();
    }

    /// 以拷贝方式返回全部数据（线程安全快照）。
    QVector<T> snapshot() const
    {
        QReadLocker locker(&m_lock);
        return m_data;
    }

    /**
     * @brief 返回 [from, to] 采样序号区间内的数据快照（含端点）。
     *
     * 用于局部重绘：只取当前视口范围内的数据，避免遍历全量数据。
     * 要求 T 具有可比较的 sampleIndex 字段；通过 keyOf 提取。
     */
    template <typename KeyFn>
    QVector<T> rangeByKey(quint64 from, quint64 to, KeyFn keyOf) const
    {
        QReadLocker locker(&m_lock);
        QVector<T> result;
        if (m_data.isEmpty() || from > to)
            return result;

        // 数据在时间轴上是递增的，可用二分查找定位起点。
        auto beginIt = std::lower_bound(m_data.cbegin(), m_data.cend(), from,
            [&keyOf](const T &item, quint64 v) { return keyOf(item) < v; });
        for (auto it = beginIt; it != m_data.cend(); ++it) {
            if (keyOf(*it) > to)
                break;
            result.append(*it);
        }
        return result;
    }

    /// 最新元素（空时返回默认值）。
    T latest() const
    {
        QReadLocker locker(&m_lock);
        return m_data.isEmpty() ? T() : m_data.last();
    }

    /// 最旧元素（空时返回默认值）。
    T oldest() const
    {
        QReadLocker locker(&m_lock);
        return m_data.isEmpty() ? T() : m_data.first();
    }

private:
    mutable QReadWriteLock m_lock;
    QVector<T>            m_data;
    int                   m_capacity = 1024;
};

#endif // RINGBUFFER_H
