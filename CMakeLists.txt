cmake_minimum_required(VERSION 3.16)

project(vital_monitor VERSION 1.1.1 LANGUAGES CXX)

# 版本号单点真源：main.cpp 通过该宏显示「关于」版本，
# 与 docs/CHANGELOG.md 的发布记录一一对应（配置管理 / 变更控制）。
add_compile_definitions(VITAL_MONITOR_VERSION="${PROJECT_VERSION}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Let CMake run moc/uic/rcc automatically.
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR})

# Qt modules used by the project (pure Widgets, no OpenGL / no third-party libs).
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

qt_add_executable(vital_monitor
    src/main.cpp

    # ---- Layer 1: data simulation / acquisition (worker thread) ----
    src/data/SampleData.h
    src/data/VitalSignSimulator.h
    src/data/VitalSignSimulator.cpp

    # ---- Layer 2: business & data management (main thread) ----
    src/core/RingBuffer.h
    src/core/ChannelConfig.h
    src/core/ChannelConfig.cpp
    src/core/AlarmDetector.h
    src/core/AlarmDetector.cpp
    src/core/CoordinateMapper.h
    src/core/CoordinateMapper.cpp
    src/core/HistoryRecorder.h
    src/core/HistoryRecorder.cpp
    src/core/AuditLogger.h
    src/core/AuditLogger.cpp
    src/core/FileIntegrity.h
    src/core/FileIntegrity.cpp
    src/core/SelfTest.h
    src/core/SelfTest.cpp
    src/core/UserRole.h
    src/core/UserRole.cpp

    # ---- Layer 3: QPainter chart widget ----
    src/chart/TrendChartWidget.h
    src/chart/TrendChartWidget.cpp

    # ---- Layer 4: main UI panels ----
    src/ui/VitalValuePanel.h
    src/ui/VitalValuePanel.cpp
    src/ui/ChannelConfigPanel.h
    src/ui/ChannelConfigPanel.cpp
    src/ui/AlarmBanner.h
    src/ui/AlarmBanner.cpp
    src/ui/PatientBarWidget.h
    src/ui/PatientBarWidget.cpp
    src/ui/MainWindow.h
    src/ui/MainWindow.cpp
)

target_include_directories(vital_monitor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(vital_monitor PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

# ---------------------------------------------------------------------------
# 自动化测试
#
# 以独立可执行目标承载单元测试，复用业务层的 AlarmDetector / ChannelConfig，
# 不依赖 GUI，可在 CI 中以退出码判定通过 / 失败（0 = 全部通过）。
#
# 运行：  cmake --build --preset mingw-debug --target alarm_state_test
#         build\vscode-mingw-debug\alarm_state_test.exe
# ---------------------------------------------------------------------------
qt_add_executable(alarm_state_test
    tests/alarm_state_test.cpp

    # 被测业务层源文件（与主程序共用，避免测试与实现漂移）
    src/data/SampleData.h
    src/core/ChannelConfig.h
    src/core/ChannelConfig.cpp
    src/core/AlarmDetector.h
    src/core/AlarmDetector.cpp
)

target_include_directories(alarm_state_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(alarm_state_test PRIVATE
    Qt6::Core
    Qt6::Gui   # SampleData.h 的 alarmIndicatorColor() 使用 QColor
)

# 控制台程序：输出测试结果到标准输出，退出码即失败用例数。
#
# @note 关掉 AUTOMOC：本测试不含任何 Q_OBJECT 类，无需 moc；
#       且在该沙箱环境下 moc_predefs 探测（g++ -dM -E）不可用。
set_target_properties(alarm_state_test PROPERTIES
    WIN32_EXECUTABLE FALSE
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

enable_testing()
add_test(NAME alarm_state_test COMMAND alarm_state_test)

# ---------------------------------------------------------------------------
# 合规增强模块测试
# 文件哈希校验、导入输入校验、权限模型、历史文件容错、单位校验、开机自检。
#
# 这些模块不依赖 GUI，可纯 Core 编译；与主程序共用同一份源文件，
# 保证「测的就是跑的那份代码」。
# ---------------------------------------------------------------------------
qt_add_executable(compliance_test
    tests/compliance_test.cpp

    src/data/SampleData.h
    src/core/ChannelConfig.h
    src/core/ChannelConfig.cpp
    src/core/RingBuffer.h
    src/core/HistoryRecorder.h
    src/core/HistoryRecorder.cpp
    src/core/FileIntegrity.h
    src/core/FileIntegrity.cpp
    src/core/SelfTest.h
    src/core/SelfTest.cpp
    src/core/UserRole.h
    src/core/UserRole.cpp
)

target_include_directories(compliance_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(compliance_test PRIVATE
    Qt6::Core
    Qt6::Gui   # SampleData.h / ChannelConfig 使用 QColor
)

set_target_properties(compliance_test PROPERTIES
    WIN32_EXECUTABLE FALSE
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

add_test(NAME compliance_test COMMAND compliance_test)

# Windows GUI application (no console window).
set_target_properties(vital_monitor PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)

# Deploy Qt runtime DLLs next to the executable so it can run directly.
if(WIN32)
    get_target_property(_qmake_exe Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin_dir "${_qmake_exe}" DIRECTORY)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${_qt_bin_dir}")
    if(WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET vital_monitor POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}" --no-translations "$<TARGET_FILE:vital_monitor>"
            COMMENT "Running windeployqt on vital_monitor"
            VERBATIM
        )
    else()
        message(WARNING "windeployqt not found; Qt DLLs will not be copied automatically.")
    endif()
endif()
