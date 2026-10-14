#pragma once

/*
 * 配置模板
 *
 * 用法：把本文件复制成同目录下的 config.h，然后填入自己的值。
 *   Windows:  copy config.example.h config.h
 *   Linux/macOS:  cp config.example.h config.h
 *
 * config.h 已被 .gitignore 忽略，不会被提交，可以放心填真实账号。
 *
 * 必填：
 *   kWifiSsid       你的 Wi-Fi 名称（只能 2.4GHz）
 *   kWifiPassword   Wi-Fi 密码（开放网络留空）
 *   kStuid / kStoken / kMid   米游社登录凭据
 *   kZzzRoleId      你的绝区零 UID
 *
 * 选填：
 *   kPortalHost / kPortalUsername / kPortalPassword
 *       只有"连上 Wi-Fi 还要开浏览器登录"的校园网才需要填，家用 Wi-Fi 留空
 *   kDeviceId / kLtokenV2 / kCookieTokenV2 / kDeviceFp
 *       一般留空即可，kDeviceId 留空会自动用芯片 MAC 生成
 */

#include <Arduino.h>

// ============ 1. Wi-Fi ============
// ESP32 只支持 2.4GHz，连不上 5GHz 的 SSID。
constexpr char kWifiSsid[] = "YOUR_WIFI_SSID";
constexpr char kWifiPassword[] = "YOUR_WIFI_PASSWORD";

// ============ 2. 校园网 / 酒店门户（家用 Wi-Fi 全部留空） ============
// kPortalHost 填门户域名，例如某些学校的 net.xxx.edu.cn（深澜 Srun 系统）。
// 留空表示不使用门户认证，固件会直接跳过。
constexpr char kPortalHost[] = "";
constexpr char kPortalUsername[] = "";
constexpr char kPortalPassword[] = "";

// ============ 3. 绝区零角色 ============
// 游戏内：设置 -> 账户 -> UID，9 位数字。
constexpr char kZzzRoleId[] = "YOUR_ZZZ_UID";
// 国服官服固定为 prod_gf_cn，不要改。
constexpr char kZzzServer[] = "prod_gf_cn";

// ============ 4. 米游社登录凭据 ============
// 获取方法见 docs/配置说明.md 第 3 节。
// 只显示体力、战绩这些数据，实际只用得到 kStuid / kStoken / kMid。
constexpr char kStuid[] = "YOUR_STUID";
constexpr char kStoken[] = "YOUR_STOKEN";
constexpr char kMid[] = "YOUR_MID";
// 留空即可：固件会用芯片 MAC 自动生成一个设备标识。
constexpr char kDeviceId[] = "";
// 下面三项只有启用菲林月报时才用得到，保持原样即可。
constexpr char kLtokenV2[] = "";
constexpr char kCookieTokenV2[] = "";
constexpr char kDeviceFp[] = "";

// ============ 5. 通用参数（一般不用改） ============
constexpr char kNtpServer[] = "ntp.aliyun.com";
constexpr long kGmtOffsetSeconds = 8L * 3600L;
constexpr uint32_t kSerialBaud = 115200;
constexpr uint32_t kWifiConnectTimeoutMs = 20000UL;
constexpr uint32_t kNtpTimeoutMs = 10000UL;
