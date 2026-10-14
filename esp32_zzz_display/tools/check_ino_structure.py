#!/usr/bin/env python3
"""Structural check for the ESP32 sketch: brace balance and preprocessor pairs."""

from __future__ import annotations

import pathlib
import re
import sys


def scan(src: str) -> tuple[int, list[tuple[int, int]]]:
    """Walk the source once, skipping comments, strings and char literals."""
    depth = 0
    depths: list[tuple[int, int]] = []
    index = 0
    line = 1
    length = len(src)

    while index < length:
        char = src[index]

        if char == "\n":
            line += 1
            index += 1
            continue

        if char == "/" and index + 1 < length and src[index + 1] == "/":
            while index < length and src[index] != "\n":
                index += 1
            continue

        if char == "/" and index + 1 < length and src[index + 1] == "*":
            index += 2
            while index + 1 < length and not (
                src[index] == "*" and src[index + 1] == "/"
            ):
                if src[index] == "\n":
                    line += 1
                index += 1
            index += 2
            continue

        if char in ('"', "'"):
            quote = char
            index += 1
            while index < length:
                if src[index] == "\\":
                    index += 2
                    continue
                if src[index] == quote:
                    index += 1
                    break
                if src[index] == "\n":
                    line += 1
                index += 1
            continue

        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1

        depths.append((line, depth))
        index += 1

    return depth, depths


def main() -> None:
    path = pathlib.Path(sys.argv[1])
    src = path.read_text(encoding="utf-8")
    depth, depths = scan(src)

    print("final_depth", depth)
    opens = len(re.findall(r"^#if 0", src, flags=re.M))
    closes = len(re.findall(r"^#endif", src, flags=re.M))
    print("if0", opens, "endif", closes)
    print("lines", len(src.splitlines()))
    print("braces", sum(1 for n, d in depths if d > 0) and "checked")
    for number, value in depths:
        if value < 0:
            print("negative depth at line", number, value)
            break


if __name__ == "__main__":
    main()
