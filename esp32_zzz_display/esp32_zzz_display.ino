/*
 * ESP32 绝区零信息显示屏
 *
 * 硬件：
 *   - ESP32-WROOM-32
 *   - 2.4 寸 ILI9341 SPI TFT 液晶屏
 *
 * 程序框架：
 *   1. 启动与显示初始化
 *      - 初始化串口、TFT 屏幕、背光和 TFT_eSPI。
 *      - 屏幕先显示启动画面，避免联网期间看起来像死机。
 *
 *   2. 网络与时间同步
 *      - 连接 config.h 中配置的 Wi-Fi。
 *      - 校园网是"开放 SSID + 网页门户"结构，拿到 IP 后还要通过深澜
 *        Srun 门户认证才能上外网，srun_portal.h 会自动完成这一步，
 *        并每 2 分钟检查一次会话，掉线自动重登。
 *      - 通过 NTP 获取北京时间，后续接口签名和倒计时都依赖正确时间。
 *
 *   3. 绝区零数据获取
 *      - 电量：读取米游社绝区零 widget 接口。
 *      - 活跃度、录像带店、刮刮卡：同样来自 widget 接口。
 *      - 式舆防卫战：读取 hadal_info_v2 接口，取层数、得分、本期截止日期。
 *      - 危局强袭战：读取 hadal_mem_detail_v2 接口，取总分、星数、排名。
 *      - 版本倒计时：流式读取米游社官方“版本更新公告”，解析版本结束时间。
 *
 *   4. 页面显示（两页，每 8 秒轮换）
 *      - 第 1 页“电量与版本”：电量、回满、活跃度、录像带店、刮刮卡，
 *        以及下一次版本更新倒计时与日期。
 *      - 第 2 页“挑战战绩”：式舆防卫战的层数/得分/截止日期，
 *        危局强袭战的得分/星数/排名。
 *      - 每 30 秒重绘一次，保证倒计时不会长期不刷新。
 *
 *   5. 主循环
 *      - 断网后自动重连 Wi-Fi。
 *      - 按不同周期刷新数据：
 *          体力/日常数据 5 分钟；
 *          挑战战绩 30 分钟；
 *          版本公告 6 小时。
 *      - 页面轮换和屏幕重绘由主循环统一调度。
 *
 *   6. 串口输出
 *      - 每轮抓取都会把结果打印到串口，波特率 115200，UTF-8 编码。
 *      - 串口信息包含电量、活跃度、挑战战绩和版本更新时间。
 *
 * 说明：
 *   - 本程序只显示绝区零信息，不请求也不显示原神数据。
 *   - 账号凭据复用 esp32_widget_access_test/config.h，不复制密钥到本工程。
 *   - 界面文字为中文。TFT_eSPI 自带字体只有 ASCII，所以中文字模由
 *     tools/gen_cjk_font.py 生成到 cjk_font.h，只包含界面用到的汉字。
 *   - 菲林月报接口当前被米游社 10035 风控，代码保留在 #if 0 中暂停使用。
 */

#include <Arduino.h>

// 有 config.h 就用它（放真实账号），没有就退回模板，保证能直接编译。
#if __has_include("config.h")
#include "config.h"
#else
#include "config.example.h"
#endif

// 必须在 TFT_eSPI.h 之前包含，确保使用 ILI9341 驱动和正确的引脚定义。
// TFT_eSPI 库自带的 User_Setup.h 可能配置了错误的 ST7789 驱动。
#include "tft_setup.h"

// 界面用到的汉字点阵，由 tools/gen_cjk_font.py 生成。
#include "cjk_font.h"

// 校园网（深澜 Srun）门户自动登录。
#include "srun_portal.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <mbedtls/md5.h>
#include <sys/time.h>

#include <utility>
#include <vector>

namespace {

// 绝区零实时便笺：电量、活跃度、录像带店、刮刮卡。
constexpr char kZzzWidgetUrl[] =
    "https://api-takumi-record.mihoyo.com/event/game_record_zzz/api/zzz/widget";

// 菲林月报：菲林、母带、邦布券的当月收入。
constexpr char kZzzMonthlyUrlBase[] =
    "https://api-takumi.mihoyo.com/event/nap_ledger/month_info";

// 绝区零式舆防卫战和危局强袭战接口。
constexpr char kZzzShiyuUrlBase[] =
    "https://api-takumi-record.mihoyo.com/event/game_record_zzz/api/zzz/"
    "hadal_info_v2";
constexpr char kZzzDeadlyUrlBase[] =
    "https://api-takumi-record.mihoyo.com/event/game_record_zzz/api/zzz/"
    "hadal_mem_detail_v2";

// 米游社官方论坛帖子列表，用来寻找最新的“版本更新公告”。
constexpr char kZzzForumListUrl[] =
    "https://bbs-api.miyoushe.com/post/wapi/getForumPostList"
    "?forum_id=58&gids=8&is_good=false&is_hot=false&page_size=30&sort_type=2";
constexpr char kZzzForumReferer[] = "https://www.miyoushe.com/zzz/forum/58";
constexpr char kZzzPostUrlBase[] =
    "https://bbs-api.miyoushe.com/post/wapi/getPostFull?post_id=";
// 当前 3.2 版本更新公告。新版本公告发布后替换成新的 post_id。
constexpr char kZzzUpdatePostId[] = "78055591";

constexpr char kWidgetSalt[] = "rtvTthKxEyreVXQCnhluFgLXPOFKPHlA";
constexpr char kRecordSalt[] = "xV8v4Qu54lUKrEYFZkJhB8cuOh9Asafs";
constexpr char kWifiUserAgent[] =
    "Mozilla/5.0 (Linux; Android 13; Mi 10 Build/UKQ1.230804.001; wv) "
    "AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 "
    "Chrome/74.0.3729.186 Mobile Safari/537.36 miHoYoBBS/2.71.1";
constexpr char kBbsUserAgent[] =
    "Mozilla/5.0 (Linux; Android 13; Pixel 5) AppleWebKit/537.36 "
    "Mobile Safari/537.36 miHoYoBBS/2.40.1";

// 公告正文里的关键字段，UTF-8 编码，避免源文件直接出现中文字符。
constexpr char kVersionEndMarker[] =
    "\xE7\x89\x88\xE6\x9C\xAC\xE7\xBB\x93\xE6\x9D\x9F"
    "\xE6\x97\xB6\xE9\x97\xB4\xE4\xB8\xBA";
// 官方帖子标题里的关键字段，UTF-8 编码。
constexpr char kUpdateAnnouncementMarker[] =
    "\xE6\x9B\xB4\xE6\x96\xB0\xE5\x85\xAC\xE5\x91\x8A";

// 绝区零角色 UID 和服务器区服在 config.h 里配置。

constexpr uint32_t kHttpTimeoutMs = 30000;
constexpr uint8_t kHttpRetryCount = 1;
constexpr uint32_t kWidgetIntervalMs = 5UL * 60UL * 1000UL;
constexpr uint32_t kMonthlyIntervalMs = 6UL * 60UL * 60UL * 1000UL;
constexpr uint32_t kChallengeIntervalMs = 30UL * 60UL * 1000UL;
constexpr uint32_t kVersionIntervalMs = 6UL * 60UL * 60UL * 1000UL;
// 两页每 8 秒轮换一次。
constexpr uint32_t kPageIntervalMs = 8UL * 1000UL;
constexpr uint32_t kRenderIntervalMs = 30UL * 1000UL;
constexpr uint32_t kWifiRetryIntervalMs = 20UL * 1000UL;
// 校园网门户会话检查周期（防掉线）。
constexpr uint32_t kPortalCheckIntervalMs = 2UL * 60UL * 1000UL;

constexpr uint16_t kBackground = 0x0841;
constexpr uint16_t kPanel = 0x18E3;
constexpr uint16_t kAccent = 0x05BF;
constexpr uint16_t kMuted = 0x9CD3;

// 统一保存 HTTP 请求头。
using Headers = std::vector<std::pair<String, String>>;

TFT_eSPI tft = TFT_eSPI();

// 绝区零状态数据。widget 和战绩接口都写入这里，界面层只读这些字段。
struct ZzzStatus {
  bool widgetValid = false;
  int energyCurrent = 0;
  int energyMax = 0;
  long energyRestoreSeconds = 0;
  int vitalityCurrent = 0;
  int vitalityMax = 0;
  String videoStore;
  String cardSign;

  // 式舆防卫战：已通关层数、得分、本期结束时间。
  bool shiyuValid = false;
  int shiyuLayerCount = 0;
  int shiyuScore = 0;
  int shiyuMaxScore = 0;
  int shiyuEndMonth = 0;
  int shiyuEndDay = 0;

  // 危局强袭战：总分、星数、排名百分比。
  bool deadlyValid = false;
  int deadlyScore = 0;
  int deadlyMaxScore = 0;
  int deadlyStar = 0;
  int deadlyTotalStar = 0;
  int deadlyRankPercent = -1;
};

// 菲林月报状态：官方默认月份及菲林、母带、邦布券收入。
struct MonthlyStatus {
  bool valid = false;
  String month = "--";
  long polychromes = 0;
  long masterTapes = 0;
  long boopons = 0;
  String statusText = "等待中";
};

// 版本公告解析结果：当前版本、版本结束时间、剩余时间。
struct VersionStatus {
  bool valid = false;
  String currentVersion = "--";
  String subject;
  String endText = "--";
  time_t endEpoch = 0;
  String statusText = "WAITING";
};

ZzzStatus zzz;
MonthlyStatus monthly;
VersionStatus versionInfo;

uint32_t lastWidgetFetchMs = 0;
uint32_t lastMonthlyFetchMs = 0;
uint32_t lastChallengeFetchMs = 0;
uint32_t lastVersionFetchMs = 0;
uint32_t lastPageSwitchMs = 0;
uint32_t lastRenderMs = 0;
uint32_t lastWifiRetryMs = 0;
uint32_t lastPortalCheckMs = 0;
// 0 = 电量与版本，1 = 挑战战绩。
uint8_t currentPage = 0;
constexpr uint8_t kPageCount = 2;
bool clockReady = false;
bool screenDirty = true;

// 计算米游社接口需要的 MD5 签名。
String md5Hex(const String &input) {
  unsigned char digest[16] = {0};
  mbedtls_md5(reinterpret_cast<const unsigned char *>(input.c_str()),
              input.length(), digest);

  char output[33] = {0};
  for (size_t index = 0; index < sizeof(digest); ++index) {
    snprintf(output + index * 2, 3, "%02x", digest[index]);
  }
  return String(output);
}

String randomAlphaNumeric(size_t length) {
  static constexpr char kCharacters[] =
      "abcdefghijklmnopqrstuvwxyz0123456789";
  String result;
  result.reserve(length);
  for (size_t index = 0; index < length; ++index) {
    result += kCharacters[esp_random() % (sizeof(kCharacters) - 1)];
  }
  return result;
}

String randomNumberString() {
  return String(100000 + static_cast<int>(esp_random() % 100000));
}

String makeWidgetDs() {
  const String timestamp = String(static_cast<unsigned long>(time(nullptr)));
  const String randomPart = randomAlphaNumeric(6);
  const String source =
      "salt=" + String(kWidgetSalt) + "&t=" + timestamp + "&r=" + randomPart;
  return timestamp + "," + randomPart + "," + md5Hex(source);
}

String makeRecordDs(const String &query) {
  const String timestamp = String(static_cast<unsigned long>(time(nullptr)));
  const String randomPart = randomNumberString();
  const String source = "salt=" + String(kRecordSalt) + "&t=" + timestamp +
                        "&r=" + randomPart + "&b=&q=" + query;
  return timestamp + "," + randomPart + "," + md5Hex(source);
}

String makeStokenCookie() {
  return "stuid=" + String(kStuid) + ";stoken=" + String(kStoken) +
         ";mid=" + String(kMid) + ";";
}

String makeFullCookie() {
  return String("ltoken_v2=") + kLtokenV2 + ";cookie_token_v2=" +
         kCookieTokenV2 + ";ltmid_v2=" + kMid + ";ltuid_v2=" + kStuid +
         ";account_id_v2=" + kStuid + ";account_mid_v2=" + kMid +
         ";ltuid=" + kStuid + ";account_id=" + kStuid + ";mid=" + kMid +
         ";stoken=" + kStoken + ";mi18nLang=zh-cn;";
}

// widget 接口使用 stoken 和专用 DS 规则。
Headers widgetHeaders() {
  return {
      {"Origin", "https://app.mihoyo.com"},
      {"User-Agent", kWifiUserAgent},
      {"Content-Type", "application/json, text/plain, */*"},
      {"Referer", "https://app.mihoyo.com"},
      {"X-Requested-With", "com.mihoyo.hyperion"},
      {"x-rpc-app_version", "2.71.1"},
      {"x-rpc-sys_version", "13"},
      {"x-rpc-client_type", "2"},
      {"x-rpc-device_id", kDeviceId},
      {"x-rpc-device_name", "Mi 10"},
      {"x-rpc-device_model", "Mi 10"},
      {"x-rpc-channel", "miyousheluodi"},
      {"x-rpc-verify_key", "bll8iq97cem8"},
      {"x-rpc-app_id", "bll8iq97cem8"},
      {"DS", makeWidgetDs()},
      {"Cookie", makeStokenCookie()},
  };
}

// 战绩接口使用完整 Cookie 和另一套 DS 规则（DS 签名 query 参数）。
Headers recordHeaders(const String &query) {
  return {
      {"Cookie", makeFullCookie()},
      {"DS", makeRecordDs(query)},
      {"Referer", "https://webstatic.mihoyo.com/"},
      {"User-Agent", kBbsUserAgent},
      {"x-rpc-app_version", "2.90.1"},
      {"x-rpc-client_type", "5"},
      {"x-rpc-device_id", kDeviceId},
      {"x-rpc-device_fp", kDeviceFp},
      {"X-Requested-With", "com.mihoyo.hyperion"},
  };
}

// 菲林月报使用 webHost 上的 nap_ledger 接口。
// DS 只签名 uid 和 region，month 参数追加在签名之后发送。
Headers monthlyHeaders(const String &signedQuery) {
  return {
      {"Cookie", makeFullCookie()},
      {"DS", makeRecordDs(signedQuery)},
      {"User-Agent",
       "Mozilla/5.0 (Linux; Android 13; XQ-BC52 Build/61.2.A.0.472A; wv) "
       "AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 "
       "Chrome/111.0.5563.116 Mobile Safari/537.36 miHoYoBBS/2.73.1"},
      {"x-rpc-app_version", "2.73.1"},
      {"x-rpc-client_type", "2"},
      {"x-rpc-sys_version", "12"},
      {"x-rpc-channel", "mihoyo"},
      {"x-rpc-device_id", kDeviceId},
      {"x-rpc-device_fp", kDeviceFp},
      {"x-rpc-device_name", "Mi 10"},
      {"x-rpc-device_model", "Mi 10"},
      {"Origin", "https://act.mihoyo.com"},
      {"Referer", "https://act.mihoyo.com/"},
  };
}

// 米游社公告接口是公开接口，只需要常见浏览器请求头。
Headers bbsHeaders(const char *referer) {
  return {
      {"Accept", "application/json"},
      {"User-Agent", kBbsUserAgent},
      {"Referer", referer},
  };
}

// 兼容带过滤器的 JSON 请求，避免为公告列表解析整包大 JSON。
bool httpGetJson(const String &url, const Headers &headers,
                 JsonDocument &document, int &httpCode, String &error,
                 JsonDocument *filter = nullptr) {
  for (uint8_t attempt = 0; attempt <= kHttpRetryCount; ++attempt) {
    document.clear();

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    if (!http.begin(client, url)) {
      error = "HTTP begin failed";
      continue;
    }

    http.setTimeout(kHttpTimeoutMs);
    for (const auto &header : headers) {
      http.addHeader(header.first, header.second);
    }

    httpCode = http.GET();
    if (httpCode <= 0) {
      error = HTTPClient::errorToString(httpCode);
      http.end();
      delay(750);
      continue;
    }

    // 先把响应读到字符串并关闭 TLS，再解析 JSON。
    // 这样 TLS 缓冲和 JSON 文档不会同时占用内存，小内存场景更稳。
    const String payload = http.getString();
    http.end();

    DeserializationError jsonError;
    if (filter != nullptr) {
      jsonError = deserializeJson(document, payload,
                                  DeserializationOption::Filter(*filter));
    } else {
      jsonError = deserializeJson(document, payload);
    }

    if (jsonError) {
      error = String("JSON: ") + jsonError.c_str();
      Serial.printf("[HTTP] JSON 解析失败: %s (响应%u字节)\n", jsonError.c_str(),
                    static_cast<unsigned>(payload.length()));
      delay(750);
      continue;
    }
    return true;
  }
  return false;
}

// 校园网门户认证。只有 config.h 里填了 kPortalHost 和账号才会执行；
// 家用 Wi-Fi 留空即可，这里会直接跳过。
void servicePortalLogin() {
  if (strlen(kPortalHost) == 0 || strlen(kPortalUsername) == 0) {
    return;
  }
  if (srun::hasInternet()) {
    Serial.println("[网络] 外网可用，跳过门户认证");
    return;
  }
  Serial.println("[网络] 外网未通，开始门户认证...");
  if (!srun::login(kPortalUsername, kPortalPassword)) {
    Serial.println("[网络] 门户认证失败，稍后会重试");
  }
}

// 连接 Wi-Fi。密码为空时按开放网络处理（校园网常见）。
bool connectWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  Serial.printf("[网络] 正在连接 %s\n", kWifiSsid);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  if (strlen(kWifiPassword) == 0) {
    WiFi.begin(kWifiSsid);
  } else {
    WiFi.begin(kWifiSsid, kWifiPassword);
  }

  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startedAt < kWifiConnectTimeoutMs) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[网络] 已连接 IP=%s 信号=%d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    servicePortalLogin();
    return true;
  }
  return false;
}

// 同步北京时间。接口签名和倒计时都依赖正确时间。
bool syncClock() {
  configTime(kGmtOffsetSeconds, 0, kNtpServer, "pool.ntp.org");
  struct tm timeInfo {};
  const uint32_t startedAt = millis();
  while (millis() - startedAt < kNtpTimeoutMs) {
    if (getLocalTime(&timeInfo, 500)) {
      Serial.printf("[时间] 已同步 %04d-%02d-%02d %02d:%02d:%02d\n",
                    timeInfo.tm_year + 1900, timeInfo.tm_mon + 1,
                    timeInfo.tm_mday, timeInfo.tm_hour, timeInfo.tm_min,
                    timeInfo.tm_sec);
      return true;
    }
  }
  return false;
}

// NTP 失败时的备选方案：从 HTTP 响应头的 Date 字段读取时间。
// 校园网可能封了 UDP 123 端口，但 HTTP 请求通常可用。
bool syncClockHttp() {
  Serial.println("[时间] 正在通过 HTTP 获取网络时间...");

  // 用普通 HTTP (端口 80) 而不是 HTTPS，避免 TLS 握手超时。
  // 百度在国内可达，响应头始终带 Date 字段。
  WiFiClient client;
  client.setTimeout(5000);
  if (!client.connect("www.baidu.com", 80)) {
    Serial.println("[时间] HTTP 连接失败，校园网门户可能未认证");
    return false;
  }

  Serial.println("[时间] HTTP 已连接，读取响应...");
  client.printf(
      "HEAD / HTTP/1.1\r\n"
      "Connection: close\r\n\r\n",
      "www.baidu.com");

  // 逐行读取响应头，找 Date 字段。
  bool found = false;
  String dateValue;
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    if (line.length() <= 1) {
      break;
    }
    if (!found && line.startsWith("Date: ")) {
      dateValue = line.substring(6);
      dateValue.trim();
      found = true;
    }
  }
  client.stop();

  if (!found || dateValue.length() == 0) {
    Serial.println("[时间] HTTP 响应中没有 Date 头");
    return false;
  }

  // 格式：Wed, 21 Oct 2026 06:00:00 GMT
  // 直接用 settimeofday 设置 UTC 时间，localtime 会自动加时区偏移。
  struct tm utcTime = {};
  const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int day = 0, month = 0, year = 0, hour = 0, minute = 0, second = 0;
  char monthStr[4] = {0};
  if (sscanf(dateValue.c_str(), "%*s %d %3s %d %d:%d:%d", &day, monthStr,
             &year, &hour, &minute, &second) != 6) {
    Serial.printf("[时间] HTTP Date 解析失败: %s\n", dateValue.c_str());
    return false;
  }

  for (int i = 0; i < 12; ++i) {
    if (strcmp(monthStr, months[i]) == 0) {
      month = i;
      break;
    }
  }

  utcTime.tm_year = year - 1900;
  utcTime.tm_mon = month;
  utcTime.tm_mday = day;
  utcTime.tm_hour = hour;
  utcTime.tm_min = minute;
  utcTime.tm_sec = second;
  utcTime.tm_isdst = 0;
  // 临时切到 UTC+0，mktime() 直接返回正确的 UTC epoch。
  configTime(0, 0, "pool.ntp.org");
  const time_t utcEpoch = mktime(&utcTime);
  // 恢复正常时区偏移。
  configTime(kGmtOffsetSeconds, 0, kNtpServer, "pool.ntp.org");
  if (utcEpoch <= 0) {
    Serial.println("[时间] HTTP Date 转换失败");
    return false;
  }

  const time_t localEpoch = utcEpoch + kGmtOffsetSeconds;
  struct timeval tv = {};
  tv.tv_sec = localEpoch;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);

  struct tm localTime = {};
  localtime_r(&localEpoch, &localTime);
  Serial.printf("[时间] HTTP 同步 %04d-%02d-%02d %02d:%02d:%02d\n",
                localTime.tm_year + 1900, localTime.tm_mon + 1,
                localTime.tm_mday, localTime.tm_hour, localTime.tm_min,
                localTime.tm_sec);
  return true;
}

// 把秒数格式化成“9天10时32分”这种屏幕易读形式。
String formatDuration(time_t seconds) {
  if (seconds <= 0) {
    return "已回满";
  }

  const uint32_t totalSeconds = static_cast<uint32_t>(seconds);
  const uint32_t days = totalSeconds / 86400UL;
  const uint32_t hours = (totalSeconds % 86400UL) / 3600UL;
  const uint32_t minutes = (totalSeconds % 3600UL) / 60UL;
  const uint32_t secs = totalSeconds % 60UL;

  if (days > 0) {
    return String(days) + "天" + String(hours) + "时" + String(minutes) + "分";
  }
  if (hours > 0) {
    return String(hours) + "时" + String(minutes) + "分";
  }
  if (minutes > 0) {
    return String(minutes) + "分" + String(secs) + "秒";
  }
  return String(secs) + "秒";
}

// 把 Unix 时间戳格式化为本地日期时间。
String formatDateTime(time_t value) {
  struct tm timeInfo {};
  localtime_r(&value, &timeInfo);
  char output[24] = {0};
  strftime(output, sizeof(output), "%Y-%m-%d %H:%M", &timeInfo);
  return String(output);
}

// 底栏时钟只显示到分钟，屏幕每 30 秒重绘一次，不需要秒。
String formatClockNow() {
  const time_t now = time(nullptr);
  struct tm timeInfo {};
  localtime_r(&now, &timeInfo);
  char output[12] = {0};
  strftime(output, sizeof(output), "%H:%M", &timeInfo);
  return String(output);
}

String formatDateNow() {
  const time_t now = time(nullptr);
  struct tm timeInfo {};
  localtime_r(&now, &timeInfo);
  char output[12] = {0};
  strftime(output, sizeof(output), "%Y-%m-%d", &timeInfo);
  return String(output);
}

// 把接口返回值转换成屏幕上的简短状态文本。
String videoStoreText(const String &raw) {
  if (raw == "SaleStateDoing") return "营业中";
  if (raw == "SaleStateDone") return "待结算";
  if (raw == "SaleStateNo") return "未营业";
  return raw.length() ? raw : "--";
}

String cardSignText(const String &raw) {
  if (raw == "CardSignDone") return "已刮";
  if (raw == "CardSignNo") return "未刮";
  return raw.length() ? raw : "--";
}

// 从公告标题中提取类似 3.2、3.3 的版本号。
String extractVersionFromSubject(const String &subject) {
  int start = -1;
  int end = -1;
  for (int index = 0; index < static_cast<int>(subject.length()); ++index) {
    const char value = subject[index];
    if (isDigit(static_cast<unsigned char>(value)) || value == '.') {
      if (start < 0) {
        start = index;
      }
      end = index;
    } else if (start >= 0) {
      break;
    }
  }

  if (start < 0 || end < start) {
    return "--";
  }
  return subject.substring(start, end + 1);
}

// 在公告正文中查找“版本结束时间为”，并解析后面的日期时间。
bool parseVersionEndTime(const char *content, time_t &endEpoch,
                         String &endText) {
  if (content == nullptr) {
    return false;
  }

  const char *marker = strstr(content, kVersionEndMarker);
  if (marker == nullptr) {
    return false;
  }

  // 标记后面可能夹着 HTML 转义（例如 < 被写成 \u003c），
  // 所以往后逐字节尝试匹配 "YYYY/MM/DD HH:MM"，命中就采用。
  const char *cursor = marker + strlen(kVersionEndMarker);
  for (size_t offset = 0; offset < 400 && cursor[offset] != '\0'; ++offset) {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    if (sscanf(cursor + offset, "%d/%d/%d %d:%d", &year, &month, &day, &hour,
               &minute) != 5) {
      continue;
    }
    if (year < 2020 || year > 2100 || month < 1 || month > 12 || day < 1 ||
        day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59) {
      continue;
    }

    struct tm target = {};
    target.tm_year = year - 1900;
    target.tm_mon = month - 1;
    target.tm_mday = day;
    target.tm_hour = hour;
    target.tm_min = minute;
    target.tm_isdst = -1;

    endEpoch = mktime(&target);
    if (endEpoch <= 0) {
      continue;
    }

    char output[24] = {0};
    snprintf(output, sizeof(output), "%04d-%02d-%02d %02d:%02d", year, month,
             day, hour, minute);
    endText = output;
    return true;
  }
  return false;
}

// 拉取绝区零实时便笺数据。
void fetchZzzWidget() {
  JsonDocument document;
  int httpCode = 0;
  String error;
  const bool ok =
      httpGetJson(kZzzWidgetUrl, widgetHeaders(), document, httpCode, error);
  // 打印完整 JSON 响应，方便在串口监视器中调试。
  {
    String rawJson;
    serializeJson(document, rawJson);
    Serial.printf("[实时便笺] HTTP=%d 原始JSON(%u字节):\n%s\n", httpCode,
                  static_cast<unsigned>(rawJson.length()), rawJson.c_str());
  }
  if (!ok || (document["retcode"] | -1) != 0) {
    Serial.printf("[实时便笺] 请求失败 HTTP=%d retcode=%d error=%s\n", httpCode,
                  document["retcode"] | -1, error.c_str());
    return;
  }

  JsonObject data = document["data"].as<JsonObject>();
  JsonObject energy = data["energy"].as<JsonObject>();
  JsonObject progress = energy["progress"].as<JsonObject>();
  JsonObject vitality = data["vitality"].as<JsonObject>();

  zzz.energyCurrent = progress["current"] | 0;
  zzz.energyMax = progress["max"] | 0;
  zzz.energyRestoreSeconds = energy["restore"] | 0L;
  zzz.vitalityCurrent = vitality["current"] | 0;
  zzz.vitalityMax = vitality["max"] | 0;
  zzz.videoStore = data["vhs_sale"]["sale_state"].as<String>();
  zzz.cardSign = data["card_sign"].as<String>();
  zzz.widgetValid = true;

  Serial.printf(
      "[实时便笺] 电量=%d/%d 回满=%s(%ld秒) 活跃度=%d/%d 录像带店=%s "
      "刮刮卡=%s\n",
      zzz.energyCurrent, zzz.energyMax,
      formatDuration(zzz.energyRestoreSeconds).c_str(),
      zzz.energyRestoreSeconds, zzz.vitalityCurrent, zzz.vitalityMax,
      videoStoreText(zzz.videoStore).c_str(),
      cardSignText(zzz.cardSign).c_str());
  screenDirty = true;
}

#if 0  // 菲林月报当前被米游社 10035 风控，暂停请求，代码保留备用。
// 兼容官方月报 count 既可能是数字，也可能是字符串的情况。
long monthlyCountValue(JsonVariantConst value) {
  if (value.is<const char *>()) {
    return String(value.as<const char *>()).toInt();
  }
  return value.as<long>();
}

// 官方月份格式为 202610，屏幕上改显示为 2026-10。
String formatMonthlyMonth(const String &raw) {
  if (raw.length() == 6) {
    return raw.substring(0, 4) + "-" + raw.substring(4);
  }
  return raw.length() > 0 ? raw : "--";
}

// 拉取当前官方默认月份的菲林月报。
void fetchZzzMonthly() {
  const String signedQuery =
      "uid=" + String(kZzzRoleId) + "&region=" + String(kZzzServer);
  const String url =
      String(kZzzMonthlyUrlBase) + "?" + signedQuery + "&month=";

  JsonDocument document;
  int httpCode = 0;
  String error;
  const bool ok =
      httpGetJson(url, monthlyHeaders(signedQuery), document, httpCode, error);
  if (!ok || (document["retcode"] | -1) != 0) {
    monthly.statusText = String("ERR ") + String(document["retcode"] | -1);
    Serial.printf("[ZZZ monthly] failed HTTP=%d retcode=%d error=%s\n",
                  httpCode, document["retcode"] | -1, error.c_str());
    return;
  }

  JsonObject data = document["data"].as<JsonObject>();
  const String month = data["data_month"].as<String>();
  if (month.length() == 0) {
    monthly.statusText = "NO MONTH";
    Serial.println("[ZZZ monthly] data_month missing");
    return;
  }

  monthly.month = formatMonthlyMonth(month);
  monthly.polychromes = 0;
  monthly.masterTapes = 0;
  monthly.boopons = 0;

  for (JsonObject item : data["month_data"]["list"].as<JsonArray>()) {
    const String type = item["data_type"].as<String>();
    const long count = monthlyCountValue(item["count"]);
    if (type == "PolychromesData") {
      monthly.polychromes = count;
    } else if (type == "MatserTapeData") {
      monthly.masterTapes = count;
    } else if (type == "BooponsData") {
      monthly.boopons = count;
    }
  }

  monthly.valid = true;
  monthly.statusText = "OK";
  Serial.printf("[ZZZ monthly] month=%s film=%ld tape=%ld boopon=%ld\n",
                monthly.month.c_str(), monthly.polychromes,
                monthly.masterTapes, monthly.boopons);
  screenDirty = true;
}
#endif  // 月报暂停结束

// 拉取式舆防卫战摘要：已通关层数、得分、本期结束日期。
void fetchZzzShiyu() {
  const String query = "need_all=true&role_id=" + String(kZzzRoleId) +
                       "&schedule_type=1&server=" + String(kZzzServer);
  const String url = String(kZzzShiyuUrlBase) + "?" + query;

  // 只取需要的字段，避免把嵌套明细整包塞进内存。
  JsonDocument filter;
  filter["retcode"] = true;
  filter["data"]["hadal_info_v2"]["brief"]["cur_period_zone_layer_count"] = true;
  filter["data"]["hadal_info_v2"]["brief"]["score"] = true;
  filter["data"]["hadal_info_v2"]["brief"]["max_score"] = true;
  filter["data"]["hadal_info_v2"]["hadal_end_time"]["month"] = true;
  filter["data"]["hadal_info_v2"]["hadal_end_time"]["day"] = true;

  JsonDocument document;
  int httpCode = 0;
  String error;
  const bool ok = httpGetJson(url, recordHeaders(query), document, httpCode,
                              error, &filter);
  if (!ok || (document["retcode"] | -1) != 0) {
    Serial.printf(
        "[式舆防卫战] 请求失败 HTTP=%d retcode=%d error=%s 可用内存=%u\n",
        httpCode, document["retcode"] | -1, error.c_str(), ESP.getFreeHeap());
    return;
  }

  JsonObject data = document["data"].as<JsonObject>();
  JsonObject info = data["hadal_info_v2"].as<JsonObject>();
  if (info.isNull()) {
    Serial.println("[式舆防卫战] 本期无数据");
    return;
  }

  JsonObject brief = info["brief"].as<JsonObject>();
  zzz.shiyuLayerCount = brief["cur_period_zone_layer_count"] | 0;
  zzz.shiyuScore = brief["score"] | 0;
  zzz.shiyuMaxScore = brief["max_score"] | 0;
  JsonObject endTime = info["hadal_end_time"].as<JsonObject>();
  zzz.shiyuEndMonth = endTime["month"] | 0;
  zzz.shiyuEndDay = endTime["day"] | 0;
  zzz.shiyuValid = true;

  Serial.printf("[式舆防卫战] 层数=%d 得分=%d/%d 截止=%02d-%02d\n",
                zzz.shiyuLayerCount, zzz.shiyuScore, zzz.shiyuMaxScore,
                zzz.shiyuEndMonth, zzz.shiyuEndDay);
  screenDirty = true;
}

// 拉取危局强袭战摘要：总分、星数、排名百分比。
void fetchZzzDeadlyAssault() {
  const String query = "region=" + String(kZzzServer) +
                       "&schedule_type=1&uid=" + String(kZzzRoleId);
  const String url = String(kZzzDeadlyUrlBase) + "?" + query;

  // 只取汇总字段。危局响应里的 list 数组带长文本和角色明细，
  // 全部解析会吃掉大量内存，这里直接过滤掉。
  JsonDocument filter;
  filter["retcode"] = true;
  filter["data"]["has_data"] = true;
  filter["data"]["total_score"] = true;
  filter["data"]["total_max_score"] = true;
  filter["data"]["total_star"] = true;
  filter["data"]["rank_percent"] = true;

  JsonDocument document;
  int httpCode = 0;
  String error;
  const bool ok = httpGetJson(url, recordHeaders(query), document, httpCode,
                              error, &filter);
  if (!ok || (document["retcode"] | -1) != 0) {
    Serial.printf(
        "[危局强袭战] 请求失败 HTTP=%d retcode=%d error=%s 可用内存=%u\n",
        httpCode, document["retcode"] | -1, error.c_str(), ESP.getFreeHeap());
    return;
  }

  JsonObject data = document["data"].as<JsonObject>();
  if (data.isNull() || !(data["has_data"] | false)) {
    Serial.println("[危局强袭战] 本期无数据");
    return;
  }

  zzz.deadlyScore = data["total_score"] | 0;
  zzz.deadlyMaxScore = data["total_max_score"] | 0;
  zzz.deadlyStar = data["total_star"] | 0;
  zzz.deadlyRankPercent = data["rank_percent"] | -1;

  // 危局一共 3 场，满星固定 3 星，不需要再从 list 里读。
  zzz.deadlyTotalStar = 3;
  zzz.deadlyValid = true;

  Serial.printf("[危局强袭战] 得分=%d/%d 星数=%d/%d 排名=%d\n",
                zzz.deadlyScore, zzz.deadlyMaxScore, zzz.deadlyStar,
                zzz.deadlyTotalStar, zzz.deadlyRankPercent);
  screenDirty = true;
}

// 公告正文约 68 KB 且使用分块传输，ESP32 无法整包解析。
// 这里直接读 TLS 分块数据，只扫描“版本结束时间为”后面的日期。
bool readChunkedBodyChunk(WiFiClientSecure &client, String &chunk,
                          bool &done) {
  chunk = "";
  done = false;
  while (client.connected() || client.available()) {
    String sizeLine = client.readStringUntil('\n');
    sizeLine.trim();
    if (sizeLine.length() == 0) {
      continue;
    }

    const int extension = sizeLine.indexOf(';');
    if (extension >= 0) {
      sizeLine = sizeLine.substring(0, extension);
    }
    const long size = strtol(sizeLine.c_str(), nullptr, 16);
    if (size <= 0) {
      done = true;
      return true;
    }

    long remaining = size;
    char buffer[512];
    while (remaining > 0) {
      const size_t request =
          min(sizeof(buffer), static_cast<size_t>(remaining));
      const int readCount = client.readBytes(buffer, request);
      if (readCount <= 0) {
        return false;
      }
      for (int index = 0; index < readCount; ++index) {
        chunk += buffer[index];
      }
      remaining -= readCount;
    }

    // 每个分块后面跟一个 CRLF。
    char trailer[2] = {0, 0};
    if (client.readBytes(trailer, 2) != 2) {
      return false;
    }
    return true;
  }
  return false;
}

// 连接米游社公告接口，流式扫描版本结束时间。
bool fetchVersionEndTimeFromAnnouncement(String &endText, time_t &endEpoch) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15000);
  if (!client.connect("bbs-api.miyoushe.com", 443)) {
    Serial.println("[版本更新] 连接公告接口失败");
    return false;
  }

  client.printf(
      "GET /post/wapi/getPostFull?post_id=%s HTTP/1.1\r\n"
      "Host: bbs-api.miyoushe.com\r\n"
      "User-Agent: %s\r\n"
      "Referer: https://www.miyoushe.com/zzz/article/%s\r\n"
      "Accept: application/json\r\n"
      "Accept-Encoding: identity\r\n"
      "Connection: close\r\n\r\n",
      kZzzUpdatePostId, kBbsUserAgent, kZzzUpdatePostId);

  const String statusLine = client.readStringUntil('\n');
  if (statusLine.indexOf(" 200 ") < 0) {
    Serial.printf("[版本更新] 响应异常: %s\n", statusLine.c_str());
    client.stop();
    return false;
  }

  bool chunked = false;
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    if (line.length() <= 1) {
      break;
    }
    line.toLowerCase();
    if (line.indexOf("transfer-encoding:") >= 0 &&
        line.indexOf("chunked") >= 0) {
      chunked = true;
    }
  }
  Serial.printf("[版本更新] 状态=%s 分块传输=%d\n", statusLine.c_str(),
                chunked ? 1 : 0);

  String tail;
  uint32_t totalBytes = 0;
  bool markerSeen = false;
  while (client.connected() || client.available()) {
    String chunk;
    bool done = false;
    if (chunked) {
      if (!readChunkedBodyChunk(client, chunk, done) || done) {
        break;
      }
    } else {
      while (client.available()) {
        chunk += static_cast<char>(client.read());
      }
      if (chunk.length() == 0 && !client.connected()) {
        break;
      }
    }

    totalBytes += chunk.length();
    const String scan = tail + chunk;
    if (scan.indexOf(kVersionEndMarker) >= 0) {
      markerSeen = true;
      if (parseVersionEndTime(scan.c_str(), endEpoch, endText)) {
        client.stop();
        return true;
      }
    }
    tail = scan.length() > 128 ? scan.substring(scan.length() - 128) : scan;
  }

  Serial.printf("[版本更新] 读取结束 共%lu字节 标记=%d\n",
                static_cast<unsigned long>(totalBytes), markerSeen ? 1 : 0);
  client.stop();
  return false;
}

// 版本倒计时获取流程：读公告正文 -> 解析结束时间 -> 更新状态。
void fetchZzzVersionInfo() {
  String endText;
  time_t endEpoch = 0;
  if (!fetchVersionEndTimeFromAnnouncement(endText, endEpoch)) {
  versionInfo.statusText = "读取失败";
    Serial.println("[版本更新] 未能读取版本结束时间");
    return;
  }

  versionInfo.valid = true;
  versionInfo.endText = endText;
  versionInfo.endEpoch = endEpoch;
  versionInfo.statusText = "正常";

  const long remainingSeconds = static_cast<long>(endEpoch - time(nullptr));
  Serial.printf("[版本更新] 更新时间=%s 剩余=%s(%ld秒)\n",
                versionInfo.endText.c_str(),
                formatDuration(remainingSeconds).c_str(), remainingSeconds);
  screenDirty = true;
}

// 从下面开始是屏幕渲染部分，所有函数只读取 zzz 和 versionInfo 两个状态结构。
// 中文字模只能画界面用到的字，缺字时画一个空心方块占位。
const uint8_t *findCjkBitmap(uint16_t codepoint) {
  for (size_t index = 0; index < kCjkGlyphCount; ++index) {
    if (kCjkGlyphs[index].codepoint == codepoint) {
      return kCjkBitmaps + kCjkGlyphs[index].offset;
    }
  }
  return nullptr;
}

// 解码一个 UTF-8 字符，cursor 前进到下一个字符。
uint16_t decodeUtf8(const char *&cursor) {
  const uint8_t first = static_cast<uint8_t>(*cursor);
  if (first < 0x80) {
    ++cursor;
    return first;
  }

  uint16_t code = 0;
  uint8_t extra = 0;
  if ((first & 0xE0) == 0xC0) {
    code = first & 0x1F;
    extra = 1;
  } else if ((first & 0xF0) == 0xE0) {
    code = first & 0x0F;
    extra = 2;
  } else if ((first & 0xF8) == 0xF0) {
    code = first & 0x07;
    extra = 3;
  } else {
    ++cursor;
    return 0;
  }

  ++cursor;
  for (uint8_t index = 0; index < extra; ++index) {
    if ((static_cast<uint8_t>(*cursor) & 0xC0) != 0x80) {
      break;
    }
    code = (code << 6) | (static_cast<uint8_t>(*cursor) & 0x3F);
    ++cursor;
  }
  return code;
}

void drawCjkGlyph(int16_t x, int16_t y, uint16_t codepoint, uint16_t fg,
                  uint16_t bg) {
  const uint8_t *bits = findCjkBitmap(codepoint);
  if (bits == nullptr) {
    tft.fillRect(x, y, kCjkGlyphWidth, kCjkGlyphHeight, bg);
    tft.drawRect(x, y, kCjkGlyphWidth - 1, kCjkGlyphHeight - 1, fg);
    return;
  }

  static uint16_t buffer[kCjkGlyphWidth * kCjkGlyphHeight];
  for (uint8_t row = 0; row < kCjkGlyphHeight; ++row) {
    const uint8_t high = bits[row * 2];
    const uint8_t low = bits[row * 2 + 1];
    for (uint8_t col = 0; col < kCjkGlyphWidth; ++col) {
      const bool on = col < 8 ? ((high >> (7 - col)) & 1) != 0
                              : ((low >> (15 - col)) & 1) != 0;
      buffer[row * kCjkGlyphWidth + col] = on ? fg : bg;
    }
  }
  tft.pushImage(x, y, kCjkGlyphWidth, kCjkGlyphHeight, buffer);
}

// 量一下混排文本的像素宽度：ASCII 交给 TFT 字体，汉字固定 16 像素。
int16_t measureMixedText(const char *text, uint8_t asciiFont = 2) {
  const char *cursor = text;
  String asciiRun;
  int16_t width = 0;

  while (*cursor != '\0') {
    const uint16_t code = decodeUtf8(cursor);
    if (code >= 0x20 && code < 0x7F) {
      asciiRun += static_cast<char>(code);
      continue;
    }
    if (asciiRun.length() > 0) {
      width += tft.textWidth(asciiRun, asciiFont);
      asciiRun = "";
    }
    width += code >= 0x4E00 ? kCjkGlyphWidth : 6;
  }

  if (asciiRun.length() > 0) {
    width += tft.textWidth(asciiRun, asciiFont);
  }
  return width;
}

// 中英混排绘制，返回绘制结束后的 x 坐标。
int16_t drawMixedText(int16_t x, int16_t y, const char *text, uint16_t fg,
                      uint16_t bg, uint8_t asciiFont = 2) {
  const char *cursor = text;
  String asciiRun;
  tft.setTextDatum(TL_DATUM);

  while (*cursor != '\0') {
    const uint16_t code = decodeUtf8(cursor);
    if (code >= 0x20 && code < 0x7F) {
      asciiRun += static_cast<char>(code);
      continue;
    }
    if (asciiRun.length() > 0) {
      tft.setTextFont(asciiFont);
      tft.setTextColor(fg, bg);
      tft.drawString(asciiRun, x, y);
      x += tft.textWidth(asciiRun, asciiFont);
      asciiRun = "";
    }
    if (code >= 0x4E00) {
      drawCjkGlyph(x, y, code, fg, bg);
      x += kCjkGlyphWidth;
    } else if (code == ' ') {
      x += 6;
    }
  }

  if (asciiRun.length() > 0) {
    tft.setTextFont(asciiFont);
    tft.setTextColor(fg, bg);
    tft.drawString(asciiRun, x, y);
    x += tft.textWidth(asciiRun, asciiFont);
  }
  return x;
}

int16_t drawCenteredText(int16_t y, const char *text, uint16_t fg,
                         uint8_t asciiFont = 2) {
  const int16_t width = measureMixedText(text, asciiFont);
  return drawMixedText((240 - width) / 2, y, text, fg, kBackground, asciiFont);
}

void drawHeader(const char *title) {
  tft.fillRect(0, 0, 240, 34, kPanel);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(kAccent, kPanel);
  tft.drawString("ZZZ", 10, 9);
  drawMixedText(54, 9, title, TFT_WHITE, kPanel);
}

void drawFooter() {
  tft.fillRect(0, 300, 240, 20, kPanel);

  // 左侧页码，中间日期，右侧时间。
  String pageText = "第";
  pageText += String(currentPage + 1);
  pageText += "/";
  pageText += String(kPageCount);
  pageText += "页";
  drawMixedText(8, 302, pageText.c_str(), kMuted, kPanel);

  const String dateText = formatDateNow();
  const int16_t dateWidth = measureMixedText(dateText.c_str());
  drawMixedText(120 - dateWidth / 2, 302, dateText.c_str(), kMuted, kPanel);

  String statusText;
  uint16_t statusColor = TFT_GREEN;
  if (WiFi.status() == WL_CONNECTED) {
    statusText = formatClockNow();
  } else {
    statusText = "未连接";
    statusColor = TFT_RED;
  }
  const int16_t width = measureMixedText(statusText.c_str());
  drawMixedText(232 - width, 302, statusText.c_str(), statusColor, kPanel);
}

// 左标签、右数值的一行。
void drawRow(int y, const char *label, const char *value,
             uint16_t valueColor = TFT_WHITE) {
  drawMixedText(12, y, label, kMuted, kBackground);
  const int16_t width = measureMixedText(value);
  drawMixedText(228 - width, y, value, valueColor, kBackground);
}

// 单页布局：上半部分是电量与日常，下半部分是版本更新倒计时。
void drawMainPage() {
  tft.fillScreen(kBackground);
  drawHeader("电量与版本");

  // 电量用大号字突出显示。
  const String energyText =
      zzz.widgetValid ? String(zzz.energyCurrent) + "/" + String(zzz.energyMax)
                      : "--";
  drawMixedText(12, 48, "电量", kMuted, kBackground);
  tft.setTextDatum(TR_DATUM);
  tft.setTextFont(4);
  tft.setTextColor(kAccent, kBackground);
  tft.drawString(energyText, 228, 42);
  tft.setTextDatum(TL_DATUM);

  const String restoreText =
      zzz.widgetValid ? formatDuration(zzz.energyRestoreSeconds) : "--";
  drawRow(88, "回满", restoreText.c_str(), TFT_CYAN);

  const String activityText =
      zzz.widgetValid
          ? String(zzz.vitalityCurrent) + "/" + String(zzz.vitalityMax)
          : "--";
  drawRow(120, "活跃度", activityText.c_str(), TFT_GREEN);

  const String videoText =
      zzz.widgetValid ? videoStoreText(zzz.videoStore) : "--";
  drawRow(152, "录像带店", videoText.c_str(), TFT_YELLOW);

  const String cardText = zzz.widgetValid ? cardSignText(zzz.cardSign) : "--";
  drawRow(184, "刮刮卡", cardText.c_str(), TFT_ORANGE);

  // 分隔线，把电量区和版本区分开。
  tft.drawFastHLine(12, 212, 216, kPanel);

  const long remainingSeconds =
      versionInfo.valid
          ? static_cast<long>(versionInfo.endEpoch - time(nullptr))
          : 0;
  const String countdown =
      versionInfo.valid ? formatDuration(remainingSeconds) : "--";
  drawRow(222, "下次更新", countdown.c_str(), kAccent);

  drawRow(254, "更新日期",
          versionInfo.valid ? versionInfo.endText.c_str() : "--",
          versionInfo.valid ? TFT_WHITE : TFT_YELLOW);

  drawFooter();
}

#if 0  // 月报页随月报接口一起暂停，代码保留备用。
void drawMonthlyPage() {
  tft.fillScreen(kBackground);
  drawHeader("MONTHLY REPORT");

  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(kMuted, kBackground);
  tft.drawString("MONTH", 120, 52);

  tft.setTextFont(4);
  tft.setTextColor(kAccent, kBackground);
  tft.drawString(monthly.valid ? monthly.month : "--", 120, 88);

  drawRow(152, "FILM", monthly.valid ? String(monthly.polychromes) : "--",
          TFT_CYAN);
  drawRow(184, "MASTER TAPE",
          monthly.valid ? String(monthly.masterTapes) : "--", TFT_YELLOW);
  drawRow(216, "BOOPON", monthly.valid ? String(monthly.boopons) : "--",
          TFT_MAGENTA);

  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(1);
  tft.setTextColor(kMuted, kBackground);
  tft.drawString("STATUS " + monthly.statusText, 120, 258);

  drawFooter();
}
#endif

// 第 2 页：式舆防卫战和危局强袭战战绩。
void drawChallengePage() {
  tft.fillScreen(kBackground);
  drawHeader("挑战战绩");

  // 上半部分：式舆防卫战
  drawMixedText(12, 44, "式舆防卫战", TFT_CYAN, kBackground);

  const String layerText =
      zzz.shiyuValid ? String(zzz.shiyuLayerCount) + "层" : "--";
  drawRow(76, "层数", layerText.c_str(), TFT_WHITE);

  const String shiyuScoreText = zzz.shiyuValid ? String(zzz.shiyuScore) : "--";
  drawRow(104, "得分", shiyuScoreText.c_str(), TFT_ORANGE);

  String deadlineText = "--";
  if (zzz.shiyuValid && zzz.shiyuEndMonth > 0) {
    char buffer[16] = {0};
    snprintf(buffer, sizeof(buffer), "%02d-%02d", zzz.shiyuEndMonth,
             zzz.shiyuEndDay);
    deadlineText = buffer;
  }
  drawRow(132, "截至", deadlineText.c_str(), kMuted);

  tft.drawFastHLine(12, 160, 216, kPanel);

  // 下半部分：危局强袭战
  drawMixedText(12, 172, "危局强袭战", TFT_MAGENTA, kBackground);

  const String deadlyScoreText =
      zzz.deadlyValid
          ? String(zzz.deadlyScore) + "/" + String(zzz.deadlyMaxScore)
          : "--";
  drawRow(204, "得分", deadlyScoreText.c_str(), TFT_ORANGE);

  const String starText =
      zzz.deadlyValid
          ? String(zzz.deadlyStar) + "/" + String(zzz.deadlyTotalStar)
          : "--";
  drawRow(232, "星数", starText.c_str(), TFT_YELLOW);

  String rankText = "--";
  if (zzz.deadlyValid && zzz.deadlyRankPercent > 0) {
    rankText = String(zzz.deadlyRankPercent / 100.0f, 1) + "%";
  }
  drawRow(260, "排名", rankText.c_str(), TFT_WHITE);

  drawFooter();
}

// 第 1 页电量与版本，第 2 页挑战战绩，按 currentPage 重绘。
void drawCurrentPage() {
  if (currentPage == 0) {
    drawMainPage();
  } else {
    drawChallengePage();
  }
  lastRenderMs = millis();
  screenDirty = false;
}

// 开机启动画面。
void drawBootScreen() {
  tft.fillScreen(kBackground);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(4);
  tft.setTextColor(kAccent, kBackground);
  tft.drawString("ZZZ", 120, 118);
  tft.setTextDatum(TL_DATUM);
  drawCenteredText(156, "绝区零显示屏", TFT_WHITE);
  drawCenteredText(212, "正在连接网络", kMuted);
}

// 维护 Wi-Fi 和 NTP 状态。断网后会自动尝试重连。
void serviceConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    clockReady = false;
    Serial.println("[诊断] Wi-Fi 未连接，等待重连");
    if (millis() - lastWifiRetryMs >= kWifiRetryIntervalMs) {
      lastWifiRetryMs = millis();
      connectWifi();
    }
    return;
  }

  if (!clockReady) {
    Serial.println("[诊断] 时钟未就绪，尝试 NTP 同步...");
    clockReady = syncClock();
    if (!clockReady) {
      Serial.println("[诊断] NTP 同步失败，尝试 HTTP 时间同步...");
      clockReady = syncClockHttp();
    }
    if (clockReady) {
      Serial.println("[诊断] 时钟同步成功");
    } else {
      Serial.println("[诊断] 时钟同步失败，数据获取将被跳过");
    }
  }

  // 门户会话可能过期，定期确认外网是否仍然可用（没配门户会自动跳过）。
  if (millis() - lastPortalCheckMs >= kPortalCheckIntervalMs) {
    lastPortalCheckMs = millis();
    if (!srun::hasInternet()) {
      Serial.println("[网络] 外网中断，重新确认门户状态...");
      servicePortalLogin();
    }
  }
}

// 按各自周期触发数据刷新，网络未就绪时直接返回。
void serviceDataFetch() {
  if (WiFi.status() != WL_CONNECTED || !clockReady) {
    Serial.printf("[诊断] 数据获取跳过: WiFi=%d clockReady=%d\n",
                  WiFi.status() == WL_CONNECTED ? 1 : 0,
                  clockReady ? 1 : 0);
    return;
  }

  const uint32_t now = millis();
  if (lastWidgetFetchMs == 0 ||
      now - lastWidgetFetchMs >= kWidgetIntervalMs) {
    lastWidgetFetchMs = now;
    fetchZzzWidget();
  }

  // 菲林月报当前被米游社 10035 风控，暂停请求。
  // if (lastMonthlyFetchMs == 0 ||
  //     now - lastMonthlyFetchMs >= kMonthlyIntervalMs) {
  //   lastMonthlyFetchMs = now;
  //   fetchZzzMonthly();
  // }

  if (lastChallengeFetchMs == 0 ||
      now - lastChallengeFetchMs >= kChallengeIntervalMs) {
    lastChallengeFetchMs = now;
    fetchZzzShiyu();
    fetchZzzDeadlyAssault();
  }

  if (lastVersionFetchMs == 0 ||
      now - lastVersionFetchMs >= kVersionIntervalMs) {
    lastVersionFetchMs = now;
    fetchZzzVersionInfo();
  }
}

}  // namespace

// Arduino 启动入口：初始化屏幕、联网、同步时间，并拉取第一轮数据。
void setup() {
  Serial.begin(kSerialBaud);
  delay(500);
  Serial.println();
  Serial.println("=== 绝区零显示屏启动 ===");

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);

  tft.init();
  tft.setRotation(0);
  drawBootScreen();

  connectWifi();
  if (WiFi.status() == WL_CONNECTED) {
    clockReady = syncClock();
  }

  serviceDataFetch();
  drawCurrentPage();

  Serial.printf("=== 首轮数据读取完成，屏幕开始轮换两页（可用内存 %u 字节）===\n",
                ESP.getFreeHeap());
}

// 主循环：联网维护、定时刷新数据、切换页面、重绘屏幕。
void loop() {
  serviceConnection();
  serviceDataFetch();

  const uint32_t now = millis();
  if (now - lastPageSwitchMs >= kPageIntervalMs) {
    lastPageSwitchMs = now;
    currentPage = (currentPage + 1) % kPageCount;
    screenDirty = true;
  }

  if (screenDirty || now - lastRenderMs >= kRenderIntervalMs) {
    drawCurrentPage();
  }

  delay(50);
}
