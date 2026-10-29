# Changelog

本仓库所有对外可见的变更记录于此，格式遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。

## [1.0.0] - 2026-10-26

首个正式版本：C++14 埋点数据采集 SDK，以静态库 + 头文件形式提供，支持 macOS / Linux / Windows，供桌面 / 客户端宿主程序集成。全部对外 API 同步返回且 `noexcept`，未初始化、非法参数与禁用状态下调用均为安全 no-op，不向调用方抛出任何异常。

### Added

- **事件采集**：`trackEvent` / `track` 同步 fire-and-forget 上报；事件先落盘后发送，本地队列容量 200 条、保留 7 天，超限自动淘汰最旧，进程销毁前排水
- **异常采集**：`trackException` 手动上报（固定 `error` 级，不受崩溃采集开关限制）；`enableCrashTrack` 开启后自动捕获未捕获 C++ 异常（`std::terminate`）、致命信号（macOS / Linux：SIGSEGV、SIGABRT 等）与 Windows SEH 未处理异常，以 `fatal` 级落盘、下次启动自动补发；异常帧 `in_app` 归属支持 `inAppIncludes` / `inAppExcludes`（excludes 优先）
- **用户身份**：`identify` / `setLoginId` / `reset`，匿名 ID + 登录 ID 双身份随事件自动携带；用户属性七类操作（对应预置事件 `$UserSet`，触发 `$Identify` 绑定）
- **公共属性**：全局公共属性注册与清除，按 预置属性 < 公共属性 < 事件属性 优先级合并
- **AB 测试**：`checkFeatureGate` / `getFeatureConfig` / `getExperiment` 三个异步查询接口（`std::future`），fastFetch 与本地缓存语义对齐 JS SDK（空结果重拉、预取时序、惰性 props 合并）；特性 / 实验命中且允许曝光时自动上报 `$FeatureImpress` / `$ExpImpress`，曝光可携带用户属性，evalall 结果定时刷新
- **安装激活采集**：首次启动自动上报 `$AppInstall`（含 `$install_time`，ISO 8601 UTC），经 `autoCapture` 配置项控制，默认开启
- **采集合规**：`optOutCapturing` / `persistOptOut` 配置项支持禁用态初始化与跨进程持久化，运行时 `optOutCapturing()` / `optInCapturing()` 随时切换（禁用时不创建发送与持久化设施，恢复后幂等补建）
- **预置事件与属性**：`$AppInstall` / `$Exception` / `$Identify` / `$UserSet`；`$device_id`（统一小写）、`$lib`（恒为 `"cpp"`）、`$lib_version`、`$os` / `$os_version`、`$language`（BCP 47 归一）、`$model`、`$screen_width` / `$screen_height`、`$timezone_offset`、`$session_id` 及应用信息属性随事件自动携带
- **可定制网络层**：内置 libcurl 发送实现，可经 `IHttpClient` 接口注入宿主自定义实现；发送成败按 HTTP 状态码（2xx）判定，与 JS SDK 一致
- **调试支持**：`debug: true` 时输出关键路径日志（init 结果、上报报文、evalall 请求与响应），生产默认静默
