#!/usr/bin/env python3
"""核对：**会上屏的** UI 文案里用到的字符，字库里是不是真的有字形。

为什么要它：本项目字库的两个已记录坑（见 tools/gen_ui_text.py 的说明）——
  · charset.txt 不可信（lv_font_conv 对源字体没有的字形是**静默跳过**）
  · 缺字形只在真正画到屏幕上时才发现，表现为**空白**（不是方框），而开发时看不见屏幕

和 tools/gen_ui_text.py 的分工：
  gen_ui_text.py  扫描 ui*.c 的**所有**字符串字面量 → 生成开机自检表（保守，宁多不少）
  本脚本          同一批字面量里**剔掉 ESP_LOGx(...) 的参数** → 只留真正会上屏的
                  （日志字符串永远到不了 LVGL，混进来只会让开机自检刷一堆无关的"缺字"）

字库字符集直接复刻 tools/gen_font_charset.py 的构造（GB2312 全集 + 假名 + 若干标点块），
不读 charset.txt —— 因为 charset.txt 正是"不可信"的那一份。

⚠️ 注意 GB2312 的**符号行 0xA1 不在** gen_font_charset.py 的范围内（它从 0xB0 起），
   所以 ← ↑ → ↓、※、§ 这类符号**字库里确实没有**，靠 LVGL 内置 Montserrat 兜底。
   本脚本因此把它们当"真缺"报出来 —— 上屏就会是空白。这就是要抓的东西。

跑法：python3 tools/check_ui_glyphs.py
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))

SOURCES = [
    "components/ui/src/ui.c",
    "components/ui/src/ui_pages.c",
    "components/ui/src/ui_keys.c",
    "components/ui/src/ui_search.c",
    "components/ui/src/ui_display.c",
]

# ---- 1. 字库字符集（复刻 gen_font_charset.py）----
FONT = set()
for hi in range(0xB0, 0xF8):
    for lo in range(0xA1, 0xFF):
        try:
            FONT.add(bytes([hi, lo]).decode("gb2312"))
        except UnicodeDecodeError:
            pass
for a, b in [(0x3040, 0x309F), (0x30A0, 0x30FF), (0x3000, 0x303F),
             (0xFF00, 0xFFEF), (0x2010, 0x203B), (0x00B0, 0x00B7)]:
    for cp in range(a, b + 1):
        FONT.add(chr(cp))

# ---- 2. 找出所有 ESP_LOGx(...) 的括号范围（按括号配对扫，别用正则硬碰）----
CALL = re.compile(r"\bESP_LOG[A-Z]?\s*\(")
LITERAL = re.compile(r'"([^"\\\n]*)"')
LINE_COMMENT = re.compile(r"//[^\n]*")
BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)


def log_spans(src):
    """返回 ESP_LOGx(...) 调用覆盖的 [start, end) 区间列表。"""
    spans = []
    for m in CALL.finditer(src):
        i = m.end()            # 落在第一个 '(' 之后
        depth = 1
        in_str = False
        esc = False
        while i < len(src) and depth > 0:
            ch = src[i]
            if in_str:
                if esc:
                    esc = False
                elif ch == "\\":
                    esc = True
                elif ch == '"':
                    in_str = False
            elif ch == '"':
                in_str = True
            elif ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
            i += 1
        spans.append((m.start(), i))
    return spans


def main():
    on_screen, log_only = set(), set()
    for rel in SOURCES:
        path = os.path.join(ROOT, rel)
        if not os.path.exists(path):
            continue
        src = open(path, encoding="utf-8").read()
        src = BLOCK_COMMENT.sub("", src)
        src = LINE_COMMENT.sub("", src)
        spans = log_spans(src)

        def is_log(pos):
            return any(a <= pos < b for a, b in spans)

        for m in LITERAL.finditer(src):
            chars = {c for c in m.group(1) if ord(c) > 127}
            if not chars:
                continue
            (log_only if is_log(m.start()) else on_screen).update(chars)
        # 同一批字符若在屏上也用过，就不算 log-only
        log_only -= on_screen

    missing = sorted(on_screen - FONT, key=ord)
    print("会上屏的 UI 字符 %d 个（去重）" % len(on_screen))
    print("字库字符集        %d 个字位" % len(FONT))
    if log_only:
        print("（另有 %d 个只在日志里出现、永不上屏，已忽略：%s）"
              % (len(log_only), " ".join(sorted(log_only, key=ord))))
    if missing:
        print("FAILED: 有 %d 个**会上屏**的字符字库里没有字形 —— 屏上会是空白：" % len(missing))
        for c in missing:
            print("    %r U+%04X" % (c, ord(c)))
        return 1
    print("OK: 会上屏的字符字库全都有字形")
    return 0


if __name__ == "__main__":
    sys.exit(main())
