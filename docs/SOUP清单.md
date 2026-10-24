# SOUP 清单与风险评估（IEC 62304 §8.1.2）

> 对应 `docs/医疗器材合规优化方向.md` **第五章「SOUP 管理」**、**第三梯队第 12 项**。
>
> SOUP = Software Of Unknown Provenance，指**非自主开发、来源与质量不可完全掌控**
> 的软件组件（含框架、库、构建工具链）。IEC 62304 要求：识别、记录版本、
> 评估已知缺陷、制定升级策略。
>
> **版本信息采集日期**：2026-10-19。版本以本机开发环境实测为准。

---

## 1. SOUP 清单

| # | 组件 | 版本 | 用途 | 许可证 | 级别影响 | 已知缺陷 / 风险 | 缓解措施 |
|---|------|------|------|--------|---------|----------------|---------|
| S-1 | **Qt Core** | 6.2.4 (mingw_64) | 对象模型、信号槽、容器、文件 IO、加解密摘要 | LGPL-3.0 / 商业 | **C** | 线程亲和性误用导致崩溃；`QDateTime` 受系统时钟影响 | 严格 `moveToThread` + `QueuedConnection`；告警计时改用 `QElapsedTimer` 单调时钟（见 `SampleData.h`） |
| S-2 | **Qt Gui** | 6.2.4 | `QPainter`、`QColor`、图像导出 | LGPL-3.0 / 商业 | **C** | 绘制在非 GUI 线程调用会崩溃 | 所有绘制限定在主线程（`TrendChartWidget::paintEvent`） |
| S-3 | **Qt Widgets** | 6.2.4 | 主窗口 / 面板 / 表格控件 | LGPL-3.0 / 商业 | **C** | 布局 / 样式表差异；高 DPI 缩放 | 固定最小宽度、避免依赖隐式布局顺序 |
| S-4 | **MinGW-w64 (GCC)** | 11.2.0 | 编译器 / 运行时（libstdc++、libgcc） | GPL-3.0 with exception | B | 已知 ABI 兼容限制；异常 / RTTI 行为差异 | 固定工具链版本；`CMakePresets.json` 固化路径 |
| S-5 | **CMake** | ≥ 3.16（开发机实测 3.2x） | 构建系统 | BSD-3-Clause | B | 生成器行为差异 | 锁定 preset；构建产物不进版本库 |
| S-6 | **Ninja** | 随 CMake preset | 构建执行器 | Apache-2.0 | B | — | 同上 |
| S-7 | **Qt Test / CTest** | 随 Qt / CMake | 自动化测试执行 | LGPL / BSD | B | 本项目的测试为**自研断言**，仅用 CTest 做调度 | 测试目标不依赖 moc（见 CMakeLists 注释） |
| S-8 | **Windows SDK / 系统 DLL** | 10.0.26200 | 平台 API（win32 后端） | 系统组件 | B | 系统更新可能改变行为 | 记录目标 OS 版本 |

> **不属于 SOUP**：本项目自研的 `src/` 下全部代码（含 `AlarmDetector`、`RingBuffer` 等），
> 由本项目的变更控制流程管理（见 `docs/变更控制流程.md`）。

---

## 2. 关键风险评估

### R-SOUP-1：Qt 版本升级带来的行为变化（风险等级：中）

- **危害**：升级 Qt 后信号槽连接语义、`QStyle` 渲染或 `QDateTime` 行为变化，
  可能导致告警横幅不刷新（→ 报警不显示，C 级危害）。
- **控制措施**：
  1. 版本号写入本清单，升级必须走变更控制流程（提交 PR + 回归测试）；
  2. `tests/alarm_state_test` 覆盖告警状态机全部迁移路径，升级后必须全绿；
  3. UI 相关改动需人工目视验证（见 `docs/可用性工程文件.md` 检查表）。

### R-SOUP-2：`QDateTime` 使用墙钟，受 NTP 校时影响（风险等级：中，**已控制**）

- **危害**：系统校时回拨 1 小时，导致告警去抖计时、重复提示、升级计时失效，
  可能使报警永不触发或疯狂重报。
- **控制措施（已实现）**：`SampleFrame::monotonicMs` 由 `QElapsedTimer` 提供，
  `AlarmDetector::frameTimeMs()` 优先取单调时钟；墙钟仅用于显示与存档。
  回归用例见 `alarm_state_test` 用例 10「单调时钟时间基准」。

### R-SOUP-3：线程亲和性误用（风险等级：高）

- **危害**：在采集线程触碰 UI 对象 → 未定义行为 / 崩溃，监护中断。
- **控制措施**：`VitalSignSimulator` 仅通过信号槽投递 `SampleFrame`（值语义），
  采集线程内不持有任何 `QWidget*`；`AlarmDetector` 只在主线程运行。

### R-SOUP-4：MinGW 运行时 DLL 部署缺失（风险等级：低）

- **危害**：目标机缺少 `libstdc++-6.dll` 等导致程序无法启动。
- **控制措施**：`CMakeLists.txt` 中 `windeployqt` POST_BUILD 自动拷贝运行时。

---

## 3. 升级策略

1. **冻结原则**：发布分支上的 SOUP 版本冻结；升级只能通过变更请求。
2. **升级流程**：
   `提出变更请求 → 评估已知 CVE → 本地构建 → ctest 全绿 → 人工目视回归 → 更新本清单版本号 → 记录到 CHANGELOG`
3. **已知漏洞跟踪**：见 `docs/SBOM与网络安全.md` 第 3 节（NVD 检索入口与记录表）。
4. **禁止事项**：不得使用未记录版本的 Qt / 编译器；不得直接替换 bin 目录下的 DLL。

---

## 4. 版本采集方式（可重现）

```bat
:: Qt 版本
D:\qt6\6.2.4\mingw_64\bin\qmake.exe -query QT_VERSION

:: 编译器版本
D:\qt6\Tools\mingw1120_64\bin\g++.exe --version

:: CMake / Ninja
cmake --version
ninja --version
```

---

## 5. 变更记录

| 版本 | 日期 | 变更 |
|------|------|------|
| 1.0 | 2026-10-19 | 首次建立；登记 8 项 SOUP 及 4 条风险 |

*文档性质：工程自查，非法规符合性认定。*
