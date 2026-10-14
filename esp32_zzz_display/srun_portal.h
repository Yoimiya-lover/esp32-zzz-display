#pragma once
/*
 * 校园网门户（深澜 Srun）自动登录模块
 *
 * 认证流程（已用电脑端 Python 验证通过）：
 *   1. GET  /cgi-bin/get_challenge?callback=cb&username=账号&ip=本机IP
 *      返回 JSONP，里面有一次性 challenge（token）和 client_ip。
 *   2. 计算签名：
 *        hmd5   = HMAC-MD5(key=token, msg=明文密码)
 *        info   = "{SRBX1}" + 自定义Base64(XXTEA(JSON, token))
 *        chksum = SHA1(token+账号+token+hmd5+token+ac_id+token+ip
 *                      +token+200+token+1+token+info)
 *   3. GET  /cgi-bin/srun_portal?action=login&username=账号
 *        &password={MD5}+hmd5&...&chksum=...&info=...
 *      返回 res=ok 即认证成功。
 *
 * 说明：
 *   - 使用"本地账户"登录，用户名不带 @telecom / @cmcc 等运营商域后缀。
 *   - 门户域名来自 config.h 的 kPortalHost；留空表示不用门户认证，
 *     家用 Wi-Fi 保持留空即可。
 *   - 必须走 HTTPS：很多深澜门户会把 http 请求 301 跳到 https。
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

#include <mbedtls/md.h>
#include <mbedtls/sha1.h>

#include <vector>

namespace srun {

// 门户地址由 config.h 的 kPortalHost 指定，这里只固定端口。
constexpr uint16_t kPortalPort = 443;
constexpr char kAcId[] = "1";
constexpr char kAuthN[] = "200";
constexpr char kAuthType[] = "1";
constexpr char kEncVer[] = "srun_bx1";
// 深澜自定义 Base64 字母表。
constexpr char kBase64Alpha[] =
    "LVoJPiCN2R8G90yg+hmFHuacZ1OWMnrsSTXkYpUq/3dlbfKwv6xztjI7DeBE45QA";

inline String toHex(const uint8_t *data, size_t length) {
  static const char kHex[] = "0123456789abcdef";
  String out;
  out.reserve(length * 2);
  for (size_t index = 0; index < length; ++index) {
    out += kHex[data[index] >> 4];
    out += kHex[data[index] & 0x0F];
  }
  return out;
}

// HMAC-MD5，返回 32 位十六进制字符串。
inline String hmacMd5Hex(const String &key, const String &message) {
  uint8_t digest[16] = {0};
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
  if (info == nullptr || mbedtls_md_setup(&ctx, info, 1) != 0 ||
      mbedtls_md_hmac_starts(
          &ctx, reinterpret_cast<const unsigned char *>(key.c_str()),
          key.length()) != 0 ||
      mbedtls_md_hmac_update(
          &ctx, reinterpret_cast<const unsigned char *>(message.c_str()),
          message.length()) != 0 ||
      mbedtls_md_hmac_finish(&ctx, digest) != 0) {
    mbedtls_md_free(&ctx);
    return "";
  }
  mbedtls_md_free(&ctx);
  return toHex(digest, sizeof(digest));
}

inline String sha1Hex(const String &input) {
  uint8_t digest[20] = {0};
  mbedtls_sha1(reinterpret_cast<const unsigned char *>(input.c_str()),
               input.length(), digest);
  return toHex(digest, sizeof(digest));
}

// 深澜 XXTEA 变体：把字符串按 32 位小端分组后加密，输出原始字节。
inline std::vector<uint8_t> xxteaEncode(const String &data,
                                        const String &key) {
  if (data.length() == 0) {
    return {};
  }

  const size_t charCount = data.length();
  const size_t wordCount = (charCount + 3) / 4;
  std::vector<uint32_t> v(wordCount + 1, 0);
  for (size_t index = 0; index < charCount; ++index) {
    v[index >> 2] |=
        static_cast<uint32_t>(static_cast<uint8_t>(data[index]))
        << ((index & 3) * 8);
  }
  v[wordCount] = static_cast<uint32_t>(charCount);

  const size_t keyCount = key.length();
  std::vector<uint32_t> k(4, 0);
  for (size_t index = 0; index < keyCount && index < 16; ++index) {
    k[index >> 2] |=
        static_cast<uint32_t>(static_cast<uint8_t>(key[index]))
        << ((index & 3) * 8);
  }

  const uint32_t n = static_cast<uint32_t>(v.size() - 1);
  uint32_t z = v[n];
  uint32_t y = v[0];
  uint32_t d = 0;
  constexpr uint32_t kDelta = 0x9E3779B9UL;
  uint32_t rounds = static_cast<uint32_t>(6 + 52 / (n + 1));

  while (rounds-- > 0) {
    d += kDelta;
    const uint32_t e = (d >> 2) & 3;
    uint32_t p = 0;
    for (p = 0; p < n; ++p) {
      y = v[p + 1];
      uint32_t m = (z >> 5) ^ (y << 2);
      m += (y >> 3) ^ (z << 4) ^ (d ^ y);
      m += k[(p & 3) ^ e] ^ z;
      z = v[p] = v[p] + m;
    }
    y = v[0];
    uint32_t m = (z >> 5) ^ (y << 2);
    m += (y >> 3) ^ (z << 4) ^ (d ^ y);
    m += k[(p & 3) ^ e] ^ z;
    z = v[n] = v[n] + m;
  }

  std::vector<uint8_t> out(v.size() * 4);
  for (size_t index = 0; index < v.size(); ++index) {
    out[index * 4 + 0] = v[index] & 0xFF;
    out[index * 4 + 1] = (v[index] >> 8) & 0xFF;
    out[index * 4 + 2] = (v[index] >> 16) & 0xFF;
    out[index * 4 + 3] = (v[index] >> 24) & 0xFF;
  }
  return out;
}

// 标准 Base64 编码，但换成深澜自定义字母表。
inline String customBase64(const std::vector<uint8_t> &data) {
  String out;
  out.reserve(((data.size() + 2) / 3) * 4);
  size_t index = 0;
  while (index + 2 < data.size()) {
    const uint32_t triple = (static_cast<uint32_t>(data[index]) << 16) |
                            (static_cast<uint32_t>(data[index + 1]) << 8) |
                            static_cast<uint32_t>(data[index + 2]);
    out += kBase64Alpha[(triple >> 18) & 0x3F];
    out += kBase64Alpha[(triple >> 12) & 0x3F];
    out += kBase64Alpha[(triple >> 6) & 0x3F];
    out += kBase64Alpha[triple & 0x3F];
    index += 3;
  }

  const size_t remain = data.size() - index;
  if (remain == 1) {
    const uint32_t triple = static_cast<uint32_t>(data[index]) << 16;
    out += kBase64Alpha[(triple >> 18) & 0x3F];
    out += kBase64Alpha[(triple >> 12) & 0x3F];
    out += "==";
  } else if (remain == 2) {
    const uint32_t triple = (static_cast<uint32_t>(data[index]) << 16) |
                            (static_cast<uint32_t>(data[index + 1]) << 8);
    out += kBase64Alpha[(triple >> 18) & 0x3F];
    out += kBase64Alpha[(triple >> 12) & 0x3F];
    out += kBase64Alpha[(triple >> 6) & 0x3F];
    out += '=';
  }
  return out;
}

// URL 百分号编码。
inline String urlEncode(const String &value) {
  static const char kHex[] = "0123456789ABCDEF";
  String out;
  out.reserve(value.length() * 2);
  for (size_t index = 0; index < value.length(); ++index) {
    const char c = value[index];
    if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ||
        c == '.' || c == '~') {
      out += c;
    } else {
      out += '%';
      out += kHex[(static_cast<uint8_t>(c) >> 4) & 0x0F];
      out += kHex[static_cast<uint8_t>(c) & 0x0F];
    }
  }
  return out;
}

// 从 JSONP 包裹中取出 JSON 正文。
inline String stripJsonp(const String &body) {
  const int start = body.indexOf('(');
  const int end = body.lastIndexOf(')');
  if (start >= 0 && end > start) {
    return body.substring(start + 1, end);
  }
  return body;
}

// 发一个简单的 HTTP GET，返回响应正文。
inline bool httpGet(const String &path, String &body, int &httpCode) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(12);
  client.setTimeout(12000);

  // 用域名连接，这样 TLS 会带上 SNI，很多门户按域名区分站点。
  if (!client.connect(kPortalHost, kPortalPort)) {
    httpCode = -1;
    return false;
  }

  client.printf("GET %s HTTP/1.1\r\n", path.c_str());
  client.printf("Host: %s\r\n", kPortalHost);
  client.print("User-Agent: Mozilla/5.0\r\n");
  client.print("Accept: */*\r\n");
  client.print("Connection: close\r\n\r\n");

  const String statusLine = client.readStringUntil('\n');
  httpCode = 0;
  if (statusLine.startsWith("HTTP/")) {
    const int space = statusLine.indexOf(' ');
    if (space > 0) {
      httpCode = statusLine.substring(space + 1, space + 4).toInt();
    }
  }

  // 跳过响应头。
  while (client.connected() || client.available()) {
    const String line = client.readStringUntil('\n');
    if (line.length() <= 1) {
      break;
    }
  }

  body = "";
  const uint32_t startedAt = millis();
  while ((client.connected() || client.available()) &&
         millis() - startedAt < 8000) {
    while (client.available()) {
      body += static_cast<char>(client.read());
    }
    if (!client.connected()) {
      break;
    }
    delay(10);
  }
  client.stop();
  return true;
}

// 完整的门户登录流程。成功返回 true。
inline bool login(const char *username, const char *password,
                  bool verbose = true) {
  const String user = String(username);

  // 第一步：拿 challenge。
  const String challengePath =
      String("/cgi-bin/get_challenge?callback=cb&username=") +
      urlEncode(user) + "&ip=" + WiFi.localIP().toString() + "&_=" +
      String(static_cast<unsigned long>(millis()));

  String challengeBody;
  int httpCode = 0;
  if (!httpGet(challengePath, challengeBody, httpCode)) {
    if (verbose) {
      Serial.printf("[校园网] 连接认证服务器失败 HTTP=%d\n", httpCode);
    }
    return false;
  }
  if (verbose) {
    Serial.printf("[校园网] challenge 响应 HTTP=%d: %.180s\n", httpCode,
                  challengeBody.c_str());
  }

  JsonDocument challengeDoc;
  if (deserializeJson(challengeDoc, stripJsonp(challengeBody))) {
    if (verbose) {
      Serial.printf("[校园网] challenge 解析失败: %.160s\n",
                    challengeBody.c_str());
    }
    return false;
  }

  const String token = challengeDoc["challenge"].as<String>();
  String clientIp = challengeDoc["client_ip"].as<String>();
  if (token.length() == 0) {
    if (verbose) {
      Serial.println("[校园网] 未取得 challenge");
    }
    return false;
  }
  if (clientIp.length() == 0) {
    clientIp = WiFi.localIP().toString();
  }

  // 第二步：计算签名。
  const String hmd5 = hmacMd5Hex(token, String(password));
  if (hmd5.length() == 0) {
    if (verbose) {
      Serial.println("[校园网] HMAC-MD5 计算失败");
    }
    return false;
  }

  const String infoJson = "{\"username\":\"" + user +
                          "\",\"password\":\"" + String(password) +
                          "\",\"ip\":\"" + clientIp + "\",\"acid\":" +
                          String(kAcId) + ",\"enc_ver\":\"" +
                          String(kEncVer) + "\"}";
  const String info =
      "{SRBX1}" + customBase64(xxteaEncode(infoJson, token));

  String checkSource = token + user;
  checkSource += token + hmd5;
  checkSource += token + kAcId;
  checkSource += token + clientIp;
  checkSource += token + kAuthN;
  checkSource += token + kAuthType;
  checkSource += token + info;
  const String chksum = sha1Hex(checkSource);

  // 第三步：发起登录。
  String loginPath = "/cgi-bin/srun_portal?callback=cb&action=login";
  loginPath += "&username=" + urlEncode(user);
  loginPath += "&password=" + urlEncode("{MD5}" + hmd5);
  loginPath += "&os=Windows+10&name=Windows&double_stack=0";
  loginPath += "&chksum=" + chksum;
  loginPath += "&info=" + urlEncode(info);
  loginPath += "&ac_id=" + String(kAcId);
  loginPath += "&ip=" + clientIp;
  loginPath += "&n=" + String(kAuthN);
  loginPath += "&type=" + String(kAuthType);

  String loginBody;
  if (!httpGet(loginPath, loginBody, httpCode)) {
    if (verbose) {
      Serial.printf("[校园网] 登录请求发送失败 HTTP=%d\n", httpCode);
    }
    return false;
  }

  if (verbose) {
    Serial.printf("[校园网] 登录响应 HTTP=%d: %.200s\n", httpCode,
                  loginBody.c_str());
  }

  JsonDocument loginDoc;
  if (deserializeJson(loginDoc, stripJsonp(loginBody))) {
    if (verbose) {
      Serial.printf("[校园网] 登录响应解析失败: %.160s\n", loginBody.c_str());
    }
    return false;
  }

  const String res = loginDoc["res"].as<String>();
  const String error = loginDoc["error"].as<String>();
  const String errorMsg = loginDoc["error_msg"].as<String>();

  if (res == "ok" || error == "ok") {
    if (verbose) {
      Serial.printf("[校园网] 登录成功 IP=%s\n", clientIp.c_str());
    }
    return true;
  }
  if (error == "ip_already_online_error" ||
      errorMsg == "ip_already_online_error") {
    if (verbose) {
      Serial.println("[校园网] 该 IP 已在线，无需重复登录");
    }
    return true;
  }

  if (verbose) {
    Serial.printf("[校园网] 登录失败 res=%s error=%s msg=%s\n", res.c_str(),
                  error.c_str(), errorMsg.c_str());
  }
  return false;
}

// 轻量探测：能否直连外网 HTTP（用于判断是否已通过门户认证）。
inline bool hasInternet() {
  WiFiClient client;
  client.setTimeout(4000);
  if (!client.connect("connect.rom.miui.com", 80)) {
    return false;
  }
  client.print("GET /generate_204 HTTP/1.1\r\n");
  client.print("Host: connect.rom.miui.com\r\n");
  client.print("Connection: close\r\n\r\n");
  const String statusLine = client.readStringUntil('\n');
  client.stop();
  // 已认证时通常返回 204。
  return statusLine.indexOf(" 204") >= 0;
}

}  // namespace srun
