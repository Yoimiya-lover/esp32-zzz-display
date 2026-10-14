#!/usr/bin/env python3
"""Draw the ESP32 to ILI9341 wiring diagram as a PNG."""

from __future__ import annotations

from pathlib import Path

import matplotlib
import matplotlib.font_manager as fm
import matplotlib.pyplot as plt


OUTPUT = Path(__file__).resolve().parent.parent / "接线图-ILI9341.png"

ESP_LABEL = "ESP32-WROOM-32"
LCD_LABEL = "2.4\" ILI9341 SPI"

# (ESP32 pin, LCD pin, colour, note)
WIRES = [
    ("3V3", "VCC", "#e63946", "供电"),
    ("GND", "GND", "#495057", "共地"),
    ("GPIO23", "SDA / MOSI", "#1d7fd6", "数据"),
    ("GPIO18", "SCK / CLK", "#7048e8", "时钟"),
    ("GPIO5", "CS", "#0ca678", "片选"),
    ("GPIO27", "DC / RS", "#f08c00", "命令/数据"),
    ("GPIO14", "RESET / RST", "#c2255c", "复位"),
    ("GPIO21", "LED / BL", "#f59f00", "背光"),
    ("GPIO19", "SDO / MISO", "#868e96", "可选，不接也行"),
]


def pick_font() -> str | None:
    for name in ("Microsoft YaHei", "SimHei", "Noto Sans CJK SC"):
        try:
            fm.findfont(fm.FontProperties(family=name), fallback_to_default=False)
            return name
        except Exception:
            continue
    return None


def main() -> None:
    matplotlib.rcParams["axes.unicode_minus"] = False
    font = pick_font()
    if font:
        matplotlib.rcParams["font.family"] = font

    row_height = 0.9
    top = 1.0
    height = top + row_height * len(WIRES) + 1.6
    fig, ax = plt.subplots(figsize=(11.5, height * 1.05), dpi=160)
    ax.set_xlim(0, 10)
    ax.set_ylim(0, height)
    ax.axis("off")

    esp_x, esp_w = 0.35, 2.5
    lcd_x, lcd_w = 7.15, 2.5
    esp_top = top + row_height * len(WIRES) + 0.35
    lcd_top = esp_top
    box_h = row_height * len(WIRES) + 0.55

    ax.add_patch(
        plt.Rectangle((esp_x, esp_top - box_h), esp_w, box_h,
                      facecolor="#e7f5ff", edgecolor="#1971c2", linewidth=2,
                      zorder=1)
    )
    ax.add_patch(
        plt.Rectangle((lcd_x, lcd_top - box_h), lcd_w, box_h,
                      facecolor="#fff4e6", edgecolor="#e8590c", linewidth=2,
                      zorder=1)
    )

    ax.text(esp_x + esp_w / 2, esp_top + 0.16, ESP_LABEL, ha="center",
            va="bottom", fontsize=13, fontweight="bold", color="#1971c2")
    ax.text(lcd_x + lcd_w / 2, lcd_top + 0.16, LCD_LABEL, ha="center",
            va="bottom", fontsize=13, fontweight="bold", color="#e8590c")

    for index, (esp_pin, lcd_pin, colour, note) in enumerate(WIRES):
        y = esp_top - 0.42 - index * row_height
        ax.text(esp_x + esp_w - 0.15, y, esp_pin, ha="right", va="center",
                fontsize=11, fontweight="bold", zorder=3)
        ax.text(lcd_x + 0.15, y, lcd_pin, ha="left", va="center",
                fontsize=11, fontweight="bold", zorder=3)

        ax.plot([esp_x + esp_w, lcd_x], [y, y], color=colour, linewidth=2.6,
                solid_capstyle="round", zorder=2)
        ax.plot([esp_x + esp_w], [y], marker="o", color=colour, markersize=6,
                zorder=4)
        ax.plot([lcd_x], [y], marker="o", color=colour, markersize=6,
                zorder=4)
        ax.text((esp_x + esp_w + lcd_x) / 2, y + 0.16, note, ha="center",
                va="bottom", fontsize=8.5, color="#555555")

    notes = [
        "只做显示时，最少 8 根线：VCC、GND、MOSI、SCK、CS、DC、RESET、BL。",
        "SDO / MISO 可选；不读取屏幕数据时可以不接。",
        "触摸脚（T_CS/T_DIN/T_DO/T_CLK/T_IRQ）和 SD 卡脚全部不接。",
        "逻辑脚都是 3.3V；只有模块明确带稳压并标注可 5V 时才给 VCC 接 5V。",
        "以上 GPIO 与 tft_setup.h 中的定义一致；改线必须同时改 tft_setup.h。",
    ]
    y = esp_top - box_h - 0.35
    ax.text(0.35, y, "注意事项", ha="left", va="top", fontsize=11,
            fontweight="bold", color="#333333")
    for line in notes:
        y -= 0.42
        ax.text(0.35, y, "· " + line, ha="left", va="top", fontsize=10,
                color="#333333")

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(OUTPUT, bbox_inches="tight", facecolor="white")
    print(f"SAVED {OUTPUT}")


if __name__ == "__main__":
    main()
