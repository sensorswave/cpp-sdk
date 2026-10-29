[English](README.md) | 简体中文

# Sensors Wave C++ SDK

![C++14](https://img.shields.io/badge/C%2B%2B-14-00599C) ![CMake](https://img.shields.io/badge/CMake-%E2%89%A5%203.16-064F8C) ![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20macOS%20%7C%20Linux-lightgrey) ![License](https://img.shields.io/badge/license-Apache--2.0-blue)

Sensors Wave C++ SDK 是面向 C++ 应用（桌面客户端、嵌入式等）的埋点数据采集库，提供事件采集、用户身份管理、用户属性、AB 测试与异常崩溃采集能力。以**静态库 + 单一头文件**形式交付，随宿主工程同批编译，无强制第三方依赖（内置 libcurl 发送实现可关闭并注入宿主自定义实现；JSON 等实现细节不外漏到公共头）。

如果你是第一次接触 Sensors Wave，可以在 [sensorswave.com](http://sensorswave.com/) 了解产品并创建账号。当前版本 1.0.0，变更记录见 [CHANGELOG.md](CHANGELOG.md)。

## Features

- **事件采集**：`trackEvent` 同步 fire-and-forget 上报；事件先落盘后发送，网络失败退避重试，进程崩溃后下次启动补发
- **异常与崩溃采集**：手动 `trackException`；开启 `enableCrashTrack` 后自动捕获未捕获 C++ 异常、致命信号（macOS / Linux）与 Windows SEH 未处理异常
- **用户身份**：匿名 ID + 登录 ID 双身份随事件自动携带，`identify` / `setLoginId` / `reset`
- **用户属性与公共属性**：七类用户属性操作；全局公共属性支持动态 getter，按 预置 < 公共 < 事件 优先级合并
- **AB 测试**：`checkFeatureGate` / `getFeatureConfig` / `getExperiment`，fastFetch 与本地缓存语义对齐 JS SDK
- **采集合规**：Opt-Out / Opt-In 运行时切换，支持跨进程持久化
- **纯净接入面**：唯一公共头 `sdk.h`，全部 API `noexcept` 静默失败，可多线程并发调用；未初始化、非法参数、禁用状态下调用均为安全 no-op

## Requirements

| 项目 | 要求 |
| --- | --- |
| 平台 | Windows / macOS / Linux |
| 编译器 | GCC ≥ 5 / Clang ≥ 3.4 / MSVC 2017 15.7+（C++14 基线，不要求更高标准） |
| 构建 | CMake ≥ 3.16 |
| 网络库 | libcurl 开发包（内置发送实现需要；`SW_WITH_CURL=OFF` 或注入 `httpClient` 时不需要） |

## SDK Usage

SDK 以**源码形式**分发，仓库地址 <https://github.com/sensorswave/cpp-sdk>（公开发布前的开发仓库位于内网 GitLab），随宿主工程一起编译并静态链接，无预编译二进制、无运行时组件。宿主编译器需支持 C++14。

### 1. 引入 SDK

**方式一：FetchContent（推荐）** —— 宿主 CMake 配置阶段自动拉取源码、同批编译：

```cmake
include(FetchContent)
FetchContent_Declare(sensorswave
  GIT_REPOSITORY https://github.com/sensorswave/cpp-sdk.git
  GIT_TAG        v1.0.0)   # 以最新发布 tag 为准
FetchContent_MakeAvailable(sensorswave)
```

**方式二：git clone / submodule + add_subdirectory**：

```bash
git clone https://github.com/sensorswave/cpp-sdk.git third_party/cpp-sdk
# 或 git submodule add https://github.com/sensorswave/cpp-sdk.git third_party/cpp-sdk
```

```cmake
add_subdirectory(third_party/cpp-sdk)
```

**方式三：本地构建安装产物后手动链接**：

```bash
git clone https://github.com/sensorswave/cpp-sdk.git
cd cpp-sdk
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cmake --install build --prefix /opt/sensorswave
```

安装产物为 `include/sensorswave/sdk.h`（唯一头文件）与 `lib/libsensorswave.a`（Windows 为 `lib/sensorswave.lib`）。

### 2. 链接

方式一 / 二只需一行（静态库的系统依赖会自动传递给宿主目标）：

```cmake
target_link_libraries(your_app PRIVATE sensorswave)
```

方式三手动链接安装产物时，请一并链接对应平台的系统依赖：

| 平台 | 需链接 |
| --- | --- |
| macOS | `-lcurl -framework CoreFoundation -framework CoreGraphics -framework IOKit -framework SystemConfiguration` |
| Linux | `-lcurl -lpthread` |
| Windows | `user32 gdi32 version advapi32 iphlpapi bcrypt` + libcurl 导入库 |

> 你的工程若自行实现网络发送（见 `SensorsWaveConfig::httpClient`），可以用 `-DSW_WITH_CURL=OFF` 构建一个不依赖 libcurl 的库。`SW_WITH_CURL=OFF` 且未注入 `httpClient` 时，事件照常入队落盘并按重试策略留队（发送端按网络失败降级），不会崩溃。

### 3. 初始化

```cpp
#include <sensorswave/sdk.h>

sensorswave::SensorsWaveConfig config;
config.apiHost = "https://your-api-host.com";
config.dataDir = "/var/lib/your-app/sw";   // 本地持久化目录；留空使用平台默认路径

sensorswave::init("your-source-token", config);
```

进程生命周期内调用一次即可；重复调用会被静默忽略。

### 4. 发送事件

```cpp
sensorswave::PropertyMap props;
props["order_id"] = sensorswave::PropertyValue::fromString("o-1001");
props["amount"]   = sensorswave::PropertyValue::fromDouble(99.5);
sensorswave::trackEvent("OrderPaid", props);
```

事件先写入本地持久化队列，由后台线程批量上报：`batchSend=true` 时按定量（10 条）+ 定时（5s）封批，`false`（默认）时每条入队后立即触发发送；网络类失败重试 1 次，指数退避（基数 5s、上限 30s），单请求超时 10s；进程崩溃后下次启动补发。

### 5. 退出前销毁

```cpp
sensorswave::destroy();
```

`destroy()` 会同步排水尚未上报的事件（阻塞上限 10 秒，截止后不再发起新批次）并释放资源，之后所有 API 进入无操作的安全状态。**进程退出前请务必调用**；幂等，可安全多次调用。崩溃场景无需调用，SDK 会自动落盘并在下次启动时补发。

## Configuration Options

`SensorsWaveConfig` 各字段：

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `sourceToken` | string | —（必填） | 项目接入 token，随请求头 `SourceToken` 上送 |
| `apiHost` | string | —（必填） | 数据接收服务器地址 |
| `dataDir` | string | 平台默认路径 | 本地持久化目录（事件队列、用户身份、AB 缓存等），默认值见 Behavior Notes |
| `debug` | bool | `false` | 是否输出调试日志（诊断信息仅在此模式下打印到 stderr） |
| `batchSend` | bool | `false` | 是否批量发送；`false` 时每条事件入队后立即触发 |
| `autoCapture` | bool | `true` | 全埋点总开关；v1 仅覆盖首次启动 `$AppInstall` 事件 |
| `enableAB` | bool | `false` | 是否启用 AB 测试；关闭时三个查询接口直接返回默认值 |
| `abRefreshInterval` | int64_t | `600000` | AB 配置刷新间隔（毫秒），最小 30000，低值自动钳制 |
| `enableCrashTrack` | bool | `false` | 是否启用崩溃自动采集（见 Error Tracking） |
| `optOutCapturing` | bool | `false` | 以「禁用采集」状态初始化（不创建发送与持久化设施） |
| `persistOptOut` | bool | `false` | 是否持久化禁用决策（跨进程生效） |
| `exceptionConfig` | `ExceptionConfig` | — | 异常帧 `in_app` 归属配置：`inAppIncludes` / `inAppExcludes`（excludes 优先） |
| `httpClient` | `std::shared_ptr<IHttpClient>` | 内置 libcurl 实现 | 自定义网络发送实现，见 `sdk.h` 中 `IHttpClient` 接口 |

批量参数、队列容量与各类超时由 SDK 内部管理，暂不作为配置项开放。

## API Methods

除 AB 三个查询接口返回 `std::future` 外，其余 API 全部同步返回且不抛异常（`noexcept`）：任何非法参数、未初始化、禁用状态下调用都是安全的无操作。全部公共 API 可多线程并发调用。

### 事件采集

#### trackEvent

```cpp
void trackEvent(const std::string& eventName, const PropertyMap& properties = {});
void track(const SendEvent& eventData);   // 高级入口：一次传入事件名 + 属性
```

```cpp
sensorswave::trackEvent("ButtonClick", {{"button_name", sensorswave::PropertyValue::fromString("submit")}});
```

属性值类型 `PropertyValue` 支持 string / double / int64 / bool / string 列表 / 结构化对象列表 / 动态 getter（`fromString` / `fromDouble` / `fromInt64` / `fromBool` / `fromStringList` / `fromMapList` / `fromGetter`）。动态 getter 在每个事件发送时重新求值，getter 抛异常时跳过该属性、事件照常发送；getter 内不得再调用本 SDK 任何 API，且应快速返回。

#### trackException

```cpp
void trackException(const std::exception& e, const PropertyMap& properties = {});
void trackException(const std::string& type, const std::string& message, const PropertyMap& properties = {});
```

手动上报异常，固定为 `error` 级，不受 `enableCrashTrack` 开关限制：

```cpp
try {
    riskyOperation();
} catch (const std::exception& e) {
    sensorswave::trackException(e);
}
```

### 用户身份

每个事件自动携带匿名 ID（`anon_id`），登录后携带登录 ID（`login_id`）。

#### identify

```cpp
void identify(const std::string& loginId);
```

绑定登录 ID：持久化保存，并发送一条 `$Identify` 事件将匿名身份与登录身份关联。

#### setLoginId

```cpp
void setLoginId(const std::string& loginId);
```

仅保存登录 ID，不发送事件。

#### reset

```cpp
void reset(bool resetAnonId = false);
```

登出：清除登录 ID 并立即开启新会话；`reset(true)` 同时更换匿名 ID。

#### 身份查询

```cpp
std::string getAnonId();      // 匿名 ID；未初始化 / 禁用时返回 ""
std::string getLoginId();     // 登录 ID；未登录返回 ""
std::string getSessionId();   // 当前会话 ID
```

### 用户属性

| 方法 | 语义 |
| --- | --- |
| `profileSet(props)` | 覆盖写 |
| `profileSetOnce(props)` | 仅首次写入生效 |
| `profileIncrement(props)` | 数值累加（非数值会被跳过） |
| `profileAppend(props)` | 列表追加（可重复） |
| `profileUnion(props)` | 列表合并（去重） |
| `profileUnset(keys)` | 删除指定属性 |
| `profileDelete()` | 清空当前用户全部属性 |

用户属性操作以预置事件 `$UserSet` 承载（操作类型 `$set` / `$set_once` / `$increment` / `$append` / `$union` / `$unset` / `$delete` 随事件属性区分）。

```cpp
sensorswave::PropertyMap profile;
profile["age"] = sensorswave::PropertyValue::fromInt64(25);
sensorswave::profileSet(profile);
```

### 公共属性

```cpp
void registerCommonProperties(const PropertyMap& props);
void clearCommonProperties();
void clearCommonProperties(const std::vector<std::string>& keys);
```

注册后随每个事件自动携带；属性值支持动态 getter，每个事件发送时重新求值。三者优先级：事件属性 > 公共属性 > 预置属性。

```cpp
sensorswave::PropertyMap common;
common["app_version"] = sensorswave::PropertyValue::fromString("1.2.0");
sensorswave::registerCommonProperties(common);
```

### AB 测试

仅以下三个 API 是异步的（`std::future`），其余全部同步：

```cpp
std::future<bool> checkFeatureGate(const std::string& featureKey);
std::future<std::string> getFeatureConfig(const std::string& configKey);
std::future<std::string> getExperiment(const std::string& expKey);
```

```cpp
if (sensorswave::checkFeatureGate("new-checkout").get()) {
    // 命中特性开关
}
const std::string config = sensorswave::getFeatureConfig("search-config").get();
```

返回值约定：

- `checkFeatureGate`：命中且未被判为 `fail` 返回 `true`；未开启 AB / 未命中 / 网络失败返回 `false`
- `getFeatureConfig` / `getExperiment`：取值以 JSON 文本返回（对象 / 数组为其紧凑 JSON 串，可二次解析）；取不到时返回 `"{}"`。与 JS SDK「返回解析后的值」语义一致——C++ 公共头不引入 JSON 类型，以文本承载同一信息；字符串值解析失败时原样返回
- 命中且数据侧允许曝光时，自动上报 `$FeatureImpress` / `$ExpImpress` 曝光事件，无需手动埋点

### 采集合规

```cpp
void optOutCapturing();             // 停止采集与上报（init 之前调用也安全）
void optInCapturing();              // 恢复采集（幂等）
bool hasOptedOutCapturing();        // 查询当前状态
```

禁用后立即停止采集，已排队的事件保留在本地队列，恢复后继续发送。配合 `persistOptOut` / `optOutCapturing` 配置项可跨进程保持用户选择。

### 销毁

```cpp
void destroy();
```

见「SDK Usage · 5. 退出前销毁」。崩溃场景（致命信号 / 未捕获异常）无需调用，SDK 会自动落盘并在下次启动时补发。

## Error Tracking

### 手动上报

见 `trackException`。事件属性包含 `$exception_level`（error）、`$exception_type`、`$exception_message` 与 `$exception_frames`（最多 30 帧，每帧含 `platform`（恒为 `"native"`）/ `instruction_addr` / `in_app`）。

### 崩溃自动采集

```cpp
config.enableCrashTrack = true;
config.exceptionConfig.inAppExcludes = {"/usr/lib", "third-party/"};
```

开启后 SDK 注册进程级钩子，捕获未捕获 C++ 异常（`std::terminate`）、致命信号（macOS / Linux：SIGSEGV、SIGABRT 等）与未处理异常（Windows SEH）。崩溃信息以 `fatal` 级 `$Exception` 事件落盘，进程下次启动时自动补发，无需额外代码。

`in_app` 归属判定：异常帧地址命中 `inAppExcludes` 关键词则标记为非本应用代码（excludes 优先）；未命中 excludes 且命中 `inAppIncludes` 时标记为本应用代码。

## Supported Event Types

SDK 自动生成的事件：

| 事件 | 触发时机 |
| --- | --- |
| `$AppInstall` | 首次启动自动上报（`autoCapture` 默认开启），携带 `$install_time` |
| `$Exception` | 手动 `trackException`（error 级）；崩溃自动采集（fatal 级） |
| `$Identify` | 调用 `identify` 绑定登录身份 |
| `$UserSet` | 七个用户属性操作（操作类型随事件属性区分） |
| `$FeatureImpress` / `$ExpImpress` | AB 特性 / 实验命中且允许曝光时自动上报 |

随事件自动携带的预置属性：`$lib`（恒为 `"cpp"`）、`$lib_version`、`$os` / `$os_version`、`$language`、`$model`、`$screen_width` / `$screen_height`、`$timezone_offset`、`$network_type` / `$wifi`、`$session_id`、`$device_id`、`$region`、`$brand` / `$manufacturer`，以及应用信息 `$app_id` / `$app_name` / `$app_version`（尽力采集，取不到时省略）。

自定义事件与属性请勿使用 `$` 前缀（该前缀为预置命名保留；误用照常发送，仅在 debug 模式下输出告警日志）。

## Behavior Notes

- 事件必带字段：`event` / `time`（毫秒时间戳）/ `trace_id`（UUID）/ `anon_id`；登录后必带 `login_id`
- 事件先落盘后发送：本地队列容量 200 条、保留 7 天，超限自动淘汰最旧事件
- 会话（`$session_id`）：空闲 30 分钟或单次会话时长满 12 小时后自动轮换
- 默认持久化目录（`dataDir` 留空时）：Windows `%APPDATA%\SensorsWave`；macOS `~/Library/Application Support/SensorsWave`；Linux `$XDG_DATA_HOME/sensorswave`（回退 `~/.local/share/sensorswave`）。服务类宿主（daemon / 容器中环境变量可能被清空，回退为随进程 CWD 漂移的相对路径）应显式配置 `dataDir`
- **单实例假设**：同一 `dataDir`（含默认目录）仅供一个 SDK 实例使用；同机多进程 / 同进程多实例共享目录会互踩本地状态（last-writer-wins），需要隔离时以不同 `dataDir` 区分
- SDK 不会向你的代码抛出任何异常，也不会因未初始化而崩溃；排查问题可开启 `debug: true` 查看诊断日志
- **fork 限制**：请勿在 fork 出的子进程中调用 `init()`（macOS 上会因系统库 fork 不安全而崩溃）；fork 后子进程应 exec 新程序再初始化
- **敏感数据**：调用方对事件属性内容负全责，手机号、身份证号、邮箱等敏感信息请先脱敏再上报，SDK 不做检测与过滤

## 从源码构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

可用 CMake 选项：

| Option | Default | Description |
| --- | --- | --- |
| `SW_WITH_CURL` | `ON` | 内置 libcurl HTTP 客户端；关闭后须注入 `IHttpClient` 实现 |
| `SW_USE_SYSTEM_JSON` | `OFF` | 使用系统 nlohmann/json 而非 FetchContent 内置版本 |
| `SW_SANITIZE` | `""` | `asan` / `tsan` 检测变体（仅 GCC / Clang） |

## License

[Apache License 2.0](LICENSE)
