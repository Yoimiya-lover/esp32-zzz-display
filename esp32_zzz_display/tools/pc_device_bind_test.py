#!/usr/bin/env python3
"""用手机真实设备信息试请求绳网月报（菲林月报）。

设备信息来自 adb getprop，改下面的 DEVICE 字典即可。
"""

from __future__ import annotations

import hashlib
import json
import pathlib
import random
import re
import time
import urllib.error
import urllib.request
import uuid


SKETCH_DIR = pathlib.Path(__file__).resolve().parent.parent
RECORD_SALT = "xV8v4Qu54lUKrEYFZkJhB8cuOh9Asafs"
WIDGET_SALT = "rtvTthKxEyreVXQCnhluFgLXPOFKPHlA"
RECORD_BASE = "https://api-takumi-record.mihoyo.com"
NAPLEDGER_BASE = "https://api-takumi.mihoyo.com"
DEVICE_FP_URL = "https://public-data-api.mihoyo.com/device-fp/api/getFp"

# ===== 换成你自己手机的参数（用 adb getprop 读，见 README）=====
# 下面留的是占位值，直接跑不会有意义，必须先换成真机数据。
DEVICE = {
    "deviceName": "YOUR_DEVICE_NAME",
    "deviceBoard": "YOUR_DEVICE_BOARD",
    "deviceModel": "YOUR_DEVICE_MODEL",
    "androidVersion": "YOUR_ANDROID_VERSION",
    "deviceFingerprint":
        "BRAND/DEVICE/PRODUCT:VERSION/BUILD_ID/BUILD_NUMBER:user/release-keys",
    "deviceProduct": "YOUR_DEVICE_PRODUCT",
}


def load_config() -> dict[str, str]:
    text = (SKETCH_DIR / "config.h").read_text(encoding="utf-8")
    return {
        m.group(1): m.group(2)
        for m in re.finditer(r'constexpr\s+char\s+(k\w+)\[\]\s*=\s*"([^"]*)"', text)
    }


def md5(text: str) -> str:
    return hashlib.md5(text.encode()).hexdigest()


def record_ds(query: str) -> str:
    stamp = str(int(time.time()))
    rand = str(random.randint(100000, 199999))
    return f"{stamp},{rand},{md5(f'salt={RECORD_SALT}&t={stamp}&r={rand}&b=&q={query}')}"


def full_cookie(cfg: dict[str, str]) -> str:
    return (
        f"ltoken_v2={cfg['kLtokenV2']};cookie_token_v2={cfg['kCookieTokenV2']};"
        f"ltmid_v2={cfg['kMid']};ltuid_v2={cfg['kStuid']};"
        f"account_id_v2={cfg['kStuid']};account_mid_v2={cfg['kMid']};"
        f"ltuid={cfg['kStuid']};account_id={cfg['kStuid']};mid={cfg['kMid']};"
        f"stoken={cfg['kStoken']};mi18nLang=zh-cn;"
    )


def post_json(url: str, headers: dict[str, str], payload: dict) -> dict:
    data = json.dumps(payload).encode()
    req = urllib.request.Request(url, data=data, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=25) as resp:
            return json.loads(resp.read())
    except urllib.error.HTTPError as exc:
        return {"error": f"HTTP {exc.code}", "body": exc.read()[:200].decode("utf-8", "replace")}


def get_json(url: str, headers: dict[str, str]) -> tuple[int, dict, int]:
    req = urllib.request.Request(url, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=25) as resp:
            raw = resp.read()
            return resp.status, json.loads(raw), len(raw)
    except urllib.error.HTTPError as exc:
        raw = exc.read()
        try:
            return exc.code, json.loads(raw), len(raw)
        except Exception:
            return exc.code, {"raw": raw[:120].decode("utf-8", "replace")}, len(raw)


def build_ext_fields(device_id: str) -> str:
    """按 ZZZ-Plugin 的思路，用真机参数拼一份较完整的设备档案。"""
    model = DEVICE["deviceModel"]
    name = DEVICE["deviceName"]
    brand = DEVICE["deviceFingerprint"].split("/")[0]
    fields = {
        "proxyStatus": 1, "isRoot": 0,
        "romCapacity": "103000", "romRemain": "51000",
        "deviceName": name,
        "productName": DEVICE["deviceProduct"],
        "hostname": "localhost",
        "screenSize": "1220x2712", "isTablet": 0,
        "aaid": device_id,
        "model": model, "brand": brand,
        "hardware": name, "deviceType": name, "devId": "REL",
        "serialNumber": "unknown",
        "sdCapacity": 0, "sdRemain": 0,
        "buildTime": str(int(time.time() * 1000) - 86400000),
        "buildUser": "builder",
        "simState": 1, "ramRemain": "250000",
        "appUpdateTimeDiff": 1780000000000,
        "deviceInfo": DEVICE["deviceFingerprint"],
        "vaid": device_id,
        "buildType": "user",
        "sdkVersion": DEVICE["androidVersion"],
        "ui_mode": "UI_MODE_TYPE_NORMAL",
        "isMockLocation": 0,
        "cpuType": "arm64-v8a", "isAirMode": 0, "ringMode": 2,
        "chargeStatus": 1, "manufacturer": brand, "emulatorStatus": 0,
        "appMemory": "2048", "osVersion": DEVICE["androidVersion"],
        "vendor": "unknown",
        "networkType": "WiFi", "oaid": device_id,
        "debugStatus": 0, "ramCapacity": "380000",
        "display": DEVICE["deviceFingerprint"].split("/")[3],
        "packageName": "com.mihoyo.hyperion",
        "packageVersion": "2.90.1",
        "batteryStatus": 85, "hasKeyboard": 0, "board": DEVICE["deviceBoard"],
    }
    return json.dumps(fields, separators=(",", ":"))


def get_device_fp(device_id: str, seed_fp: str = "38d805c20d53d") -> str:
    payload = {
        "app_name": "bbs_cn",
        "platform": "2",
        "device_id": device_id,
        "bbs_device_id": device_id,
        "seed_id": "".join(random.choice("0123456789abcdef") for _ in range(16)),
        "seed_time": str(int(time.time() * 1000)),
        "ext_fields": build_ext_fields(device_id),
        "device_fp": seed_fp,
    }
    headers = {
        "Content-Type": "application/json",
        "User-Agent": "okhttp/4.8.0",
        "x-rpc-app_version": "2.90.1",
        "x-rpc-device_id": device_id,
        "x-rpc-client_type": "5",
        "x-rpc-device_name": DEVICE["deviceName"],
        "x-rpc-device_model": DEVICE["deviceModel"],
    }
    body = post_json(DEVICE_FP_URL, headers, payload)
    data = body.get("data") or {}
    fp = str(data.get("device_fp") or "")
    print(f"  getFp: code={data.get('code')} fp_len={len(fp)} fp={fp}")
    return fp


def test_monthly(cfg: dict[str, str], label: str, device_id: str, device_fp: str,
                 name: str, model: str) -> None:
    uid = cfg["kZzzRoleId"]
    signed = f"uid={uid}&region=prod_gf_cn"
    headers = {
        "Cookie": full_cookie(cfg),
        "DS": record_ds(signed),
        "User-Agent": (
            f"Mozilla/5.0 (Linux; Android {DEVICE['androidVersion']}; {model} "
            f"Build/{DEVICE['deviceFingerprint'].split('/')[3]}; wv) "
            "AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 "
            "Chrome/111.0.5563.116 Mobile Safari/537.36 miHoYoBBS/2.73.1"
        ),
        "x-rpc-app_version": "2.73.1",
        "x-rpc-client_type": "2",
        "x-rpc-device_id": device_id,
        "x-rpc-device_fp": device_fp,
        "x-rpc-device_name": name,
        "x-rpc-device_model": model,
        "x-rpc-sys_version": DEVICE["androidVersion"],
        "x-rpc-channel": "mihoyo",
        "Origin": "https://act.mihoyo.com",
        "Referer": "https://act.mihoyo.com/",
    }
    status, body, size = get_json(
        f"{NAPLEDGER_BASE}/event/nap_ledger/month_info?{signed}&month=", headers)
    print(f"  绳网月报[{label}] HTTP={status} {size}B retcode={body.get('retcode')} "
          f"{json.dumps(body, ensure_ascii=False)[:140]}")


def test_widget(cfg: dict[str, str], label: str, device_id: str) -> None:
    stamp = str(int(time.time()))
    rand = "".join(random.choice("abcdefghijklmnopqrstuvwxyz0123456789") for _ in range(6))
    headers = {
        "Origin": "https://app.mihoyo.com",
        "User-Agent": "Mozilla/5.0 (Linux; Android 13; Mi 10) miHoYoBBS/2.71.1",
        "Referer": "https://app.mihoyo.com",
        "x-rpc-app_version": "2.71.1",
        "x-rpc-client_type": "2",
        "x-rpc-device_id": device_id,
        "DS": f"{stamp},{rand},{md5(f'salt={WIDGET_SALT}&t={stamp}&r={rand}')}",
        "Cookie": f"stuid={cfg['kStuid']};stoken={cfg['kStoken']};mid={cfg['kMid']};",
    }
    status, body, size = get_json(
        f"{RECORD_BASE}/event/game_record_zzz/api/zzz/widget", headers)
    print(f"  实时便笺[{label}] HTTP={status} retcode={body.get('retcode')}（对照用）")


def main() -> None:
    cfg = load_config()
    old_id = cfg["kDeviceId"]
    new_id = str(uuid.uuid4())

    print("=== 设备信息 ===")
    for k, v in DEVICE.items():
        print(f"  {k:<18} {v}")

    print("\n=== 一、用真机参数换新指纹，再试绳网月报 ===")
    fp_old_id = get_device_fp(old_id)
    if fp_old_id:
        test_monthly(cfg, "旧device_id+真机参数", old_id, fp_old_id,
                     DEVICE["deviceName"], DEVICE["deviceModel"])

    fp_new_id = get_device_fp(new_id)
    if fp_new_id:
        test_monthly(cfg, "新device_id+真机参数", new_id, fp_new_id,
                     DEVICE["deviceName"], DEVICE["deviceModel"])

    print("\n=== 二、其他组合 ===")
    test_monthly(cfg, "旧device_id+原指纹", old_id, cfg["kDeviceFp"],
                 DEVICE["deviceName"], DEVICE["deviceModel"])
    test_monthly(cfg, "旧device_id+原指纹+原设备名", old_id, cfg["kDeviceFp"],
                 "Mi 10", "Mi 10")

    print("\n=== 三、实时便笺对照 ===")
    test_widget(cfg, "新device_id", new_id)
    test_widget(cfg, "旧device_id", old_id)


if __name__ == "__main__":
    main()
