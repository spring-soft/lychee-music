#!/bin/bash
# 生成 ESP-Music 用的中文点阵字体（LVGL bin 格式，零 RAM 占用 → 放 assets 分区 mmap）
#
# 源字体：DroidSansFallbackFull.ttf（单面 TTF、全 CJK；系统里 Noto CJK 是 .ttc，lv_font_conv 读不了）
# ⚠️ 它自带的 ASCII 是【全角】的，所以字符集里【排除 ASCII】，
#    拉丁字母/数字/带音标的西文靠 LVGL 内置 Montserrat 兜底（代码里给 binfont 设了 fallback）。
#
# ⚠️ 用 --range 而不是 --symbols：字符集有 3 万多个字，全塞进命令行是个 90KB 的参数；
#    Unicode 块本来就是连续的，用区间既快又不容易出错。
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
TTF=/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf
OUT="$ROOT/assets_src/font"
SIZE=${SIZE:-16}
# 小字号也要 4bpp：★ 12px 汉字用 2bpp 只有 4 级灰度，笔画糊成一团、认不出字
# （用户反馈"中文变成看不懂的字"，16px 经 1:1 回读核对是对的，12px 是薄弱点）
# 代价：文件大一倍（291KB → 约 580KB），assets 分区 4MB 完全放得下。
BPP=${BPP:-4}

mkdir -p "$OUT"

# 每个块都必须记录【为什么加】——以后再补字库时才知道边界在哪
# 字符集由 tools/gen_font_charset.py 生成（里面有"为什么不用 CJK 全集"的实测记录）
python3 "$HERE/gen_font_charset.py" "$OUT/charset.txt"

ARGS=(--symbols "$(cat "$OUT/charset.txt")")

npx --yes lv_font_conv \
  --font "$TTF" \
  "${ARGS[@]}" \
  --size "$SIZE" \
  --bpp "$BPP" \
  --format bin \
  --no-compress \
  -o "$OUT/font_cjk_${SIZE}.bin"

ls -l "$OUT/font_cjk_${SIZE}.bin" | awk '{printf "生成 font_cjk_%s.bin（%dpx/%dbpp） %.0f KB\n", ENVIRON["SIZE"], ENVIRON["SIZE"], ENVIRON["BPP"], $5/1024}'
