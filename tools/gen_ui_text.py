#!/usr/bin/env python3
"""
把 UI 文案里用到的**非 ASCII 字符**抽出来，生成一个头文件给固件开机自检。

为什么需要它：这个项目的字库有两个坑，各浪费过一轮排查
（见记忆 esp-music-fonts / esp-music-lcd-text-and-state）：
  1. **charset.txt 不可信** —— 它是由「传给 lv_font_conv 的 range」推导出来的，
     而 lv_font_conv 对源字体里没有的字形是【静默跳过】。于是文件里写着有 `…`，
     字体里其实没有 → 屏上一片空白（不是方框）。
  2. 缺字形只会在真正画到屏幕上时才发现，而我看不见屏幕，只能靠用户反馈。

所以：**把用到的字符列出来，开机时逐个查字体，缺一个就报一行**。
（`font_has_glyph()` 会连 fallback 一起查 —— 中文字库刻意排除了 ASCII，
拉丁字母靠 fallback 到 Montserrat。）

输出 components/ui/include/ui_text_chars.h：
    static const char UI_TEXT_CHARS[] = "…所有非 ASCII 字符…";
    #define UI_TEXT_CHARS_COUNT n

跑法：python3 tools/gen_ui_text.py   （改了 UI 文案之后要重跑）
⚠️ 生成的这个头文件是**构建输入**，不是素材；忘跑不会出错，只是自检会漏掉新字符。
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
OUT = os.path.join(ROOT, "components", "ui", "include", "ui_text_chars.h")

# 要扫的源码：所有会把字符串画到屏上的地方
SOURCES = [
    "components/ui/src/ui.c",
    "components/ui/src/ui_pages.c",
    "components/ui/src/ui_keys.c",
    "components/ui/src/ui_search.c",
    "components/ui/src/ui_display.c",
]

# C 字符串字面量（不含转义、不跨行）
PROBES = "…—×→★▶·〇⚠"

LITERAL = re.compile(r'"([^"\\\n]*)"')
# 注释（// 和 /* */）—— 注释不上屏，里面的字符不用查
LINE_COMMENT = re.compile(r"//[^\n]*")
BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)


def main():
    chars = set()
    for rel in SOURCES:
        path = os.path.join(ROOT, rel)
        if not os.path.exists(path):
            continue
        src = open(path, encoding="utf-8").read()
        src = BLOCK_COMMENT.sub("", src)
        src = LINE_COMMENT.sub("", src)
        for lit in LITERAL.findall(src):
            for ch in lit:
                if ord(ch) > 127:
                    chars.add(ch)

    ordered = sorted(chars, key=ord)
    # 按码点升序排好，方便日志里一眼看出是哪一段缺
    body = "".join(ordered)

    with open(OUT, "w", encoding="utf-8") as f:
        f.write("/* 本文件由 tools/gen_ui_text.py 生成，不要手改 */\n")
        f.write("/* UI 文案里用到的全部非 ASCII 字符（开机自检用：缺字形就报日志）*/\n")
        f.write("#pragma once\n\n")
        f.write('static const char UI_TEXT_CHARS[] = "%s";\n' % body)
        f.write("#define UI_TEXT_CHARS_COUNT %d\n\n" % len(ordered))
        f.write("/* 固定探针：这些符号历史上踩过坑或容易踩，开机各报一次状态。\n")
        f.write(" * 放在这里而不是源码里，就是为了不被上面的扫描当成\"UI 文案\"。 */\n")
        f.write('static const char UI_FONT_PROBES[] = "%s";\n' % PROBES)
        f.write("#define UI_FONT_PROBES_COUNT %d\n" % len(PROBES))

    print("UI 文案里的非 ASCII 字符 %d 个 → %s"
          % (len(ordered), os.path.relpath(OUT, ROOT)))
    print("  " + body)


if __name__ == "__main__":
    main()
