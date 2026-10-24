#include <QApplication>
#include <QMetaType>

#include "data/SampleData.h"
#include "ui/MainWindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("VitalMonitor"));
    app.setOrganizationName(QStringLiteral("QtMedicalDemo"));

    // 版本号来自 CMakeLists.txt 的 project(VERSION ...)，保证「构建产物」
    // 与「发布说明」引用的是同一个数字（配置管理要求：可追溯到具体版本）。
    app.setApplicationVersion(QStringLiteral(VITAL_MONITOR_VERSION));

    // 注册自定义类型，使跨线程信号槽（QueuedConnection）可以传递它们。
    qRegisterMetaType<ChannelSample>("ChannelSample");
    qRegisterMetaType<SampleFrame>("SampleFrame");
    qRegisterMetaType<AlarmEvent>("AlarmEvent");
    qRegisterMetaType<QVector<ChannelConfig>>("QVector<ChannelConfig>");

    MainWindow w;
    w.show();

    return app.exec();
}
