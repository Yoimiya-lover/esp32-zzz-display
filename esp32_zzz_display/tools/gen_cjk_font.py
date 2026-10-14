#!/usr/bin/env python3
"""Generate a 16x16 1bpp CJK glyph header for the ZZZ display sketch.

Only the characters used by the on-screen text are baked in, so the font
costs a couple of kilobytes of flash instead of a full CJK font.
"""

from __future__ import annotations

import pathlib
import re

from PIL import Image, ImageDraw, ImageFont


# 字体文件：Windows 自带黑体，其它系统可以换成任意支持中文的 ttf。
FONT_PATH = pathlib.Path(r"C:\Windows\Fonts\simhei.ttf")
# 输出到本脚本所在目录的上一层，也就是 esp32_zzz_display/cjk_font.h。
OUTPUT = pathlib.Path(__file__).resolve().parent.parent / "cjk_font.h"

SIZE = 16

# Every Chinese string the sketch draws, plus a few spare words.
UI_TEXT = """
电量与日常回满活跃度录像带店刮刮卡
营业中待结算未营业已刮未刮
版本更新下次日期状态正常读取失败等待中成功
挑战战绩式舆防卫战期数数据有无危局强袭得分星排名第页层截至
天时分秒
已连接未连接连接中网络绝区零显示屏正在
今日每周
便信公口号同告实步求笺间启动
"""


def collect_glyphs() -> list[str]:
    seen: dict[str, None] = {}
    for char in UI_TEXT:
        if "\u4e00" <= char <= "\u9fff":
            seen.setdefault(char, None)
    return sorted(seen)


def render_glyph(char: str, font: ImageFont.FreeTypeFont) -> bytes:
    left, top, right, bottom = font.getbbox(char)
    canvas = Image.new("L", (64, 64), 0)
    draw = ImageDraw.Draw(canvas)
    offset_x = -(right - left) // 2 + (64 - (right - left)) // 2
    offset_y = -(bottom - top) // 2 + (64 - (bottom - top)) // 2
    draw.text((offset_x, offset_y), char, font=font, fill=255)

    box = canvas.getbbox()
    if box is None:
        return bytes(SIZE * 2)

    ink = canvas.crop(box)
    if ink.width > SIZE:
        ink = ink.resize((SIZE, int(ink.height * SIZE / ink.width)))
    if ink.height > SIZE:
        ink = ink.resize((int(ink.width * SIZE / ink.height), SIZE))

    cell = Image.new("L", (SIZE, SIZE), 0)
    cell.paste(ink, ((SIZE - ink.width) // 2, (SIZE - ink.height) // 2))

    data = bytearray()
    pixels = cell.load()
    for row in range(SIZE):
        high = 0
        low = 0
        for col in range(SIZE):
            on = pixels[col, row] >= 128
            if col < 8:
                high = (high << 1) | (1 if on else 0)
            else:
                low = (low << 1) | (1 if on else 0)
        data.append(high)
        data.append(low)
    return bytes(data)


def main() -> None:
    font = ImageFont.truetype(str(FONT_PATH), SIZE)
    glyphs = collect_glyphs()

    bitmap = bytearray()
    entries: list[tuple[str, int]] = []
    for char in glyphs:
        entries.append((char, len(bitmap)))
        bitmap.extend(render_glyph(char, font))

    lines = [
        "// 自动生成，请勿手工修改。",
        "// 由 tools/gen_cjk_font.py 生成：界面用到的汉字按 16x16 单色点阵打包。",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "constexpr uint8_t kCjkGlyphWidth = 16;",
        "constexpr uint8_t kCjkGlyphHeight = 16;",
        "",
        "struct CjkGlyph {",
        "  uint16_t codepoint;  // BMP 码点",
        "  uint16_t offset;     // 在 kCjkBitmaps 中的字节偏移",
        "};",
        "",
        f"constexpr size_t kCjkGlyphCount = {len(entries)};",
        "",
        "const CjkGlyph kCjkGlyphs[] = {",
    ]
    for char, offset in entries:
        lines.append(
            f"    {{0x{ord(char):04X}, {offset}}},  // {char}"
        )
    lines.append("};")
    lines.append("")
    lines.append("const uint8_t kCjkBitmaps[] = {")
    per_line = 16
    for index in range(0, len(bitmap), per_line):
        chunk = bitmap[index : index + per_line]
        lines.append(
            "    " + " ".join(f"0x{byte:02X}," for byte in chunk)
        )
    lines.append("};")
    lines.append("")

    text = "\n".join(lines)
    if not re.search(r"kCjkGlyphCount = \d+;", text):
        raise SystemExit("generated header looks wrong")

    OUTPUT.write_text(text, encoding="utf-8")
    print(f"SAVED {OUTPUT}")
    print(f"glyphs={len(entries)} bytes={len(bitmap)}")
    print("chars=" + "".join(char for char, _ in entries))


if __name__ == "__main__":
    main()
