#!/usr/bin/env python3
"""电脑端探测脚本：验证米游社凭据能不能读到绝区零各接口。

从同工程的 config.h 读取凭据，不需要另外填。
用法：
    python tools/pc_zzz_probe.py

依次测试：
    1. 实时便笺（电量、活跃度）
    2. 菲林月报
    3. 式舆防卫战
    4. 危局强袭战
    5. 角色信息（几个候选接口，看官方是否开放）
"""

from __future__ import annotations

import hashlib
import hmac
import json
import pathlib
import random
import re
import time
import urllib.error
import urllib.request


SKETCH_DIR = pathlib.Path(__file__).resolve().parent.parent
CONFIG = SKETCH_DIR / "config.h"

WIDGET_SALT = "rtvTthKxEyreVXQCnhluFgLXPOFKPHlA"
RECORD_SALT = "xV8v4Qu54lUKrEYFZkJhB8cuOh9Asafs"

RECORD_BASE = "https://api-takumi-record.mihoyo.com"
NAPLEDGER_BASE = "https://api-takumi.mihoyo.com"
DEVICE_FP_URL = "https://public-data-api.mihoyo.com/device-fp/api/getFp"


def load_config() -> dict[str, str]:
    text = CONFIG.read_text(encoding="utf-8")
    values: dict[str, str] = {}
    for match in re.finditer(r'constexpr\s+char\s+(k\w+)\[\]\s*=\s*"([^"]*)"', text):
        values[match.group(1)] = match.group(2)
    return values


def md5(text: str) -> str:
    return hashlib.md5(text.encode()).hexdigest()


def record_ds(query: str) -> str:
    """战绩类接口的 DS：签名 query 参数。"""
    stamp = str(int(time.time()))
    rand = str(random.randint(100000, 199999))
    source = f"salt={RECORD_SALT}&t={stamp}&r={rand}&b=&q={query}"
    return f"{stamp},{rand},{md5(source)}"


def full_cookie(cfg: dict[str, str]) -> str:
    return (
        f"ltoken_v2={cfg['kLtokenV2']};cookie_token_v2={cfg['kCookieTokenV2']};"
        f"ltmid_v2={cfg['kMid']};ltuid_v2={cfg['kStuid']};"
        f"account_id_v2={cfg['kStuid']};account_mid_v2={cfg['kMid']};"
        f"ltuid={cfg['kStuid']};account_id={cfg['kStuid']};mid={cfg['kMid']};"
        f"stoken={cfg['kStoken']};mi18nLang=zh-cn;"
    )


def record_headers(cfg: dict[str, str], query: str, app_version: str = "2.90.1",
                   client_type: str = "5") -> dict[str, str]:
    return {
        "Cookie": full_cookie(cfg),
        "DS": record_ds(query),
        "Referer": "https://webstatic.mihoyo.com/",
        "User-Agent": (
            "Mozilla/5.0 (Linux; Android 13; XQ-BC52 Build/61.2.A.0.472A; wv) "
            "AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 "
            "Chrome/111.0.5563.116 Mobile Safari/537.36 miHoYoBBS/" + app_version
        ),
        "x-rpc-app_version": app_version,
        "x-rpc-client_type": client_type,
        "x-rpc-device_id": cfg["kDeviceId"],
        "x-rpc-device_fp": cfg["kDeviceFp"],
        "x-rpc-device_name": "Mi 10",
        "x-rpc-device_model": "Mi 10",
        "Origin": "https://act.mihoyo.com",
        "Referer": "https://act.mihoyo.com/",
    }


def widget_headers(cfg: dict[str, str]) -> dict[str, str]:
    stamp = str(int(time.time()))
    rand = "".join(random.choice("abcdefghijklmnopqrstuvwxyz0123456789") for _ in range(6))
    source = f"salt={WIDGET_SALT}&t={stamp}&r={rand}"
    return {
        "Origin": "https://app.mihoyo.com",
        "User-Agent": "Mozilla/5.0 (Linux; Android 13; Mi 10) miHoYoBBS/2.71.1",
        "Referer": "https://app.mihoyo.com",
        "x-rpc-app_version": "2.71.1",
        "x-rpc-client_type": "2",
        "x-rpc-device_id": cfg["kDeviceId"],
        "DS": f"{stamp},{rand},{md5(source)}",
        "Cookie": f"stuid={cfg['kStuid']};stoken={cfg['kStoken']};mid={cfg['kMid']};",
    }


def get(url: str, headers: dict[str, str]) -> tuple[int, dict, int]:
    request = urllib.request.Request(url, headers=headers)
    try:
        with urllib.request.urlopen(request, timeout=20) as response:
            raw = response.read()
            return response.status, json.loads(raw), len(raw)
    except urllib.error.HTTPError as exc:
        raw = exc.read()
        return exc.code, {"retcode": None, "message": f"HTTP {exc.code}"}, len(raw)


def show(label: str, status: int, body: dict, size: int,
         preview_keys: list[str] | None = None):
    retcode = body.get("retcode")
    message = body.get("message", "")
    print(f"\n--- {label} ---")
    print(f"HTTP={status} retcode={retcode} 响应{size}字节 message={message}")
    if retcode == 0 and preview_keys:
        data = body.get("data") or {}
        for key in preview_keys:
            if key in data:
                print(f"  {key} = {json.dumps(data[key], ensure_ascii=False)[:180]}")
        if not any(k in data for k in preview_keys):
            print("  data keys:", list(data.keys())[:12])
    elif retcode != 0:
        print("  raw:", json.dumps(body, ensure_ascii=False)[:220])


def main() -> None:
    cfg = load_config()
    missing = [k for k in ("kStuid", "kStoken", "kMid", "kZzzRoleId", "kDeviceId") if not cfg.get(k)]
    if missing:
        print("config.h 缺少:", ", ".join(missing))
        return

    uid = cfg["kZzzRoleId"]
    print(f"UID={uid}  stuid={cfg['kStuid']}  mid={cfg['kMid']}")
    print(f"stoken 长度={len(cfg['kStoken'])}  device_id={cfg['kDeviceId']}")

    # 1. 实时便笺
    status, body, size = get(f"{RECORD_BASE}/event/game_record_zzz/api/zzz/widget",
                             widget_headers(cfg))
    show("实时便笺", status, body, size, ["energy", "vitality", "vhs_sale", "card_sign"])

    # 2. 菲林月报
    signed = f"uid={uid}&region=prod_gf_cn"
    status, body, size = get(f"{NAPLEDGER_BASE}/event/nap_ledger/month_info?{signed}&month=",
                             record_headers(cfg, signed, "2.73.1", "2"))
    show("菲林月报", status, body, size, ["data_month", "optional_month", "month_data"])

    # 3. 式舆防卫战
    query = f"need_all=true&role_id={uid}&schedule_type=1&server=prod_gf_cn"
    status, body, size = get(f"{RECORD_BASE}/event/game_record_zzz/api/zzz/hadal_info_v2?{query}",
                             record_headers(cfg, query))
    show("式舆防卫战", status, body, size, ["hadal_ver", "hadal_info_v2"])

    # 4. 危局强袭战
    query = f"region=prod_gf_cn&schedule_type=1&uid={uid}"
    status, body, size = get(f"{RECORD_BASE}/event/game_record_zzz/api/zzz/hadal_mem_detail_v2?{query}",
                             record_headers(cfg, query))
    show("危局强袭战", status, body, size,
         ["total_score", "total_max_score", "total_star", "rank_percent"])

    # 5. 角色信息候选接口
    candidates = [
        ("角色列表 avatar_list", f"/event/game_record_zzz/api/zzz/avatar_list?role_id={uid}&server=prod_gf_cn",
         f"role_id={uid}&server=prod_gf_cn"),
        ("角色列表 single_role_avatar_list",
         f"/event/game_record_zzz/api/zzz/single_role_avatar_list?role_id={uid}&server=prod_gf_cn",
         f"role_id={uid}&server=prod_gf_cn"),
        ("角色列表 role_avatar_list",
         f"/event/game_record_zzz/api/zzz/role_avatar_list?role_id={uid}&server=prod_gf_cn",
         f"role_id={uid}&server=prod_gf_cn"),
        ("角色总览 index", f"/event/game_record_zzz/api/zzz/index?role_id={uid}&server=prod_gf_cn",
         f"role_id={uid}&server=prod_gf_cn"),
    ]
    for label, path, signed_query in candidates:
        status, body, size = get(f"{RECORD_BASE}{path}", record_headers(cfg, signed_query))
        show(label, status, body, size, ["avatar_list", "avatars", "list"])


if __name__ == "__main__":
    main()
