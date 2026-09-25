#!/usr/bin/env python3
"""
生成字库字符集。

⚠️⚠️ 为什么退回"GB2312 + 假名"而不是 CJK 全集（2026-09-25 实测教训）：
  把所有 CJK 统一表意文字（20992）+ 扩展 A（6582）一起塞进一个字库（28080 字形、
  1.75MB）之后，**屏上的中文变成了看不懂的字** —— 字形能找到（不变问号），
  但画出来是别的字。字库文件的结构（cmap 范围/gid/loca 单调性）解析出来是自洽的，
  所以不是"文件坏了"，而是这个规模下 lv_font_conv 的 bin 输出 / LVGL 的 binfont
  加载器之间有对不上的地方（未能定位到具体是哪一处）。
  用户在意的实际是【日文假名】—— GB2312 里根本没有假名，日文歌名会显示成 "紅?呪?"。
  所以这里加假名，不追求 CJK 全集。要再扩必须**边扩边用眼睛验收**（见 README 的排查手法）。
"""
import sys

chars = set()
# ---- GB2312 全集（一级 3755 + 二级 3008 汉字）----
for hi in range(0xB0, 0xF8):
    for lo in range(0xA1, 0xFF):
        try:
            chars.add(bytes([hi, lo]).decode('gb2312'))
        except UnicodeDecodeError:
            pass
# ---- Unicode 块 ----
RANGES = [
    (0x3040, 0x309F),   # ★ 平假名（GB2312 没有！日文歌名必需）
    (0x30A0, 0x30FF),   # ★ 片假名
    (0x3000, 0x303F),   # CJK 符号和标点（、。《》「」【】）
    (0xFF00, 0xFFEF),   # 全角 ASCII + 半角片假名
    (0x2010, 0x203B),   # 破折号/引号/省略号（注：DroidSansFallback 里大多是空的，
                        #   实际靠 LVGL 内置 Montserrat 兜底；列在这里只为兼容)
    (0x00B0, 0x00B7),
]
for a, b in RANGES:
    for cp in range(a, b + 1):
        chars.add(chr(cp))

s = ''.join(sorted(chars))
open(sys.argv[1], 'w', encoding='utf-8').write(s)
print(f"字符集 {len(s)} 个字位（GB2312 汉字 + 假名 + 标点）")
