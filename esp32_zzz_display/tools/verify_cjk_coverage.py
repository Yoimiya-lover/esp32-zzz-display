#!/usr/bin/env python3
"""检查屏幕上会画出来的汉字是否都在 cjk_font.h 里。

只扫描绘制函数和生成显示文本的函数的字符串参数，
给串口日志用的中文不算在内，避免误报。
输出 missing 为 (none) 就说明字模齐全。
"""

from __future__ import annotations

import pathlib
import re
import sys


SKETCH_DIR = pathlib.Path(__file__).resolve().parent.parent
SKETCH = SKETCH_DIR / "esp32_zzz_display.ino"
FONT = SKETCH_DIR / "cjk_font.h"

# 这些函数里的字符串最终会出现在屏幕或屏幕用到的文本里。
# 如果以后新增了绘制函数，记得把名字加进这个列表。
SCAN_FUNCTIONS = (
    "drawMixedText",
    "drawCenteredText",
    "drawHeader",
    "drawRow",
    "drawString",
    "formatDuration",
    "videoStoreText",
    "cardSignText",
)


def cjk(chars: str) -> set[str]:
    return {char for char in chars if "\u4e00" <= char <= "\u9fff"}


def literals_in_call(code: str, open_index: int) -> list[str]:
    """从函数调用的左括号开始，收集这一层参数里的字符串字面量。"""
    literals: list[str] = []
    depth = 0
    index = open_index
    length = len(code)
    while index < length:
        char = code[index]
        if char == '"':
            end = index + 1
            while end < length:
                if code[end] == "\\":
                    end += 2
                    continue
                if code[end] == '"':
                    break
                end += 1
            literals.append(code[index : end + 1])
            index = end + 1
            continue
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                break
        index += 1
    return literals


def main() -> int:
    sketch = SKETCH.read_text(encoding="utf-8")
    font_header = FONT.read_text(encoding="utf-8")

    # 去掉注释，避免里面的中文被当成屏幕文字。
    code = re.sub(r"/\*.*?\*/", "", sketch, flags=re.S)
    code = re.sub(r"//[^\n]*", "", code)

    drawn: list[str] = []
    for name in SCAN_FUNCTIONS:
        for match in re.finditer(r"\b" + name + r"\s*\(", code):
            open_index = code.index("(", match.start())
            drawn.extend(literals_in_call(code, open_index))

    used = cjk("".join(drawn))
    available = cjk(font_header)

    print("drawn_literals", len(drawn))
    print("used_chars", len(used))
    print("font_chars", len(available))
    missing = sorted(used - available)
    print("missing", "".join(missing) if missing else "(none)")
    if missing:
        print("missing_codepoints", " ".join(f"U+{ord(c):04X}" for c in missing))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
