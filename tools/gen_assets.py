#!/usr/bin/env python3
"""
把 assets_src/ 下的 PNG 和字体打包成一个 assets.bin（带索引头），
由 ui 组件用 EMBED_FILES 嵌进固件，运行时零拷贝取指针。

为什么打成一个 blob 而不是生成 .c 数组：
  - 262KB 素材生成成 C 源会有 ~1.5MB 文本，编译慢、链接慢
  - 二进制 blob 用 EMBED_FILES 直接嵌，编译期开销几乎为零
  - 运行时只解析一次索引头，之后 asset_get(id) 就是指针运算

blob 布局（小端）：
  [0]  assets_hdr_t { char magic[4]='AMUS'; u16 ver; u16 count; u32 total; u32 resv; }
  [16] entry[count] { char name[28]; u32 off; u32 size; u16 w; u16 h; u8 fmt; u8 resv[3]; }
  [...] 各 blob（4 字节对齐）

fmt: 0=RGB565  1=LVGL bin 字体  2=RGB565A8（图标带 alpha）  3=文本资源（已 gzip）

同时生成 ui_assets_index.h 里的 enum，C 代码用 ASSET_BG_PLAY_A 这种名字取图。
"""
import gzip
import os
import struct
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
UI_DIR = os.path.join(ROOT, "assets_src", "ui")
FONT_DIR = os.path.join(ROOT, "assets_src", "font")
WEB_DIR = os.path.join(ROOT, "assets_src", "web")
PINYIN_DIR = os.path.join(ROOT, "assets_src", "pinyin")
OUT_BIN = os.path.join(ROOT, "components", "assets", "assets.bin")
OUT_HDR = os.path.join(ROOT, "components", "assets", "include", "assets_index.h")

FMT_RGB565, FMT_FONT, FMT_RGB565A8, FMT_WEB_GZ, FMT_RAW = 0, 1, 2, 3, 4
ENTRY_SZ = 44
HDR_SZ = 16


def name_to_id(name: str) -> str:
    """文件名 → C 枚举名。
    ⚠️ 要去掉【所有】扩展名并把非法字符换掉：网页是 "index.html.gz"，
    只 splitext 一次会得到 "index.html"，生成出 ASSET_INDEX.HTML 这种语法错误的枚举名。"""
    base = name.split(".")[0]
    safe = "".join(c if c.isalnum() else "_" for c in base)
    return "ASSET_" + safe.upper()


def png_to_rgb565(path):
    """RGB 源 → RGB565；RGBA 源 → RGB565A8（颜色平面 + 独立 alpha 平面）。

    RGB565A8 让 LVGL 把图标真正混合到任意背景上。之前直接丢掉 alpha，透明区
    变成黑色，上屏就是"图标外面一圈黑方块"。"""
    im0 = Image.open(path)
    has_alpha = im0.mode in ("RGBA", "LA") or (im0.mode == "P" and "transparency" in im0.info)
    im = im0.convert("RGBA") if has_alpha else im0.convert("RGB")
    w, h = im.size
    px = im.load()
    buf = bytearray(w * h * (3 if has_alpha else 2))
    i = 0
    for y in range(h):
        for x in range(w):
            p = px[x, y]
            r, g, b = p[0], p[1], p[2]
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            buf[i] = v & 0xFF
            buf[i + 1] = (v >> 8) & 0xFF
            i += 2
    if has_alpha:
        for y in range(h):
            for x in range(w):
                buf[i] = px[x, y][3]
                i += 1
    return bytes(buf), w, h, (2 if has_alpha else 0)


def main():
    items = []   # (name, bytes, fmt, w, h)

    # --- 图片 ---
    ui_names = sorted(f for f in os.listdir(UI_DIR) if f.endswith(".png")) if os.path.isdir(UI_DIR) else []
    for f in ui_names:
        data, w, h, fmt = png_to_rgb565(os.path.join(UI_DIR, f))
        items.append((f, data, fmt, w, h))

    # --- 字体（LVGL bin）---
    font_files = []
    if os.path.isdir(FONT_DIR):
        font_files = sorted(f for f in os.listdir(FONT_DIR) if f.endswith(".bin"))
        for f in font_files:
            with open(os.path.join(FONT_DIR, f), "rb") as fh:
                data = fh.read()
            items.append((f, data, FMT_FONT, 0, 0))

    # --- 网页（gzip 后放分区，httpd 直接发，零拷贝）---
    if os.path.isdir(WEB_DIR):
        for f in sorted(os.listdir(WEB_DIR)):
            if not f.endswith(".html"):
                continue
            with open(os.path.join(WEB_DIR, f), "rb") as fh:
                raw = fh.read()
            gz = gzip.compress(raw, 9)
            # 名字记成 "<原名>.gz"，http_console 按 "index.html" 也能查到
            items.append((f + ".gz", gz, FMT_WEB_GZ, 0, 0))
            print(f"  网页 {f}: {len(raw)} → gzip {len(gz)} 字节（{100*len(gz)//max(len(raw),1)}%）")

    # --- 拼音表（tools/gen_pinyin.py 生成的通用 blob）---
    if os.path.isdir(PINYIN_DIR):
        for f in sorted(f for f in os.listdir(PINYIN_DIR) if f.endswith(".bin")):
            with open(os.path.join(PINYIN_DIR, f), "rb") as fh:
                data = fh.read()
            items.append((f, data, FMT_RAW, 0, 0))

    if not items:
        raise SystemExit("assets_src/ 下没有素材，先跑 gen_ui_png.py / gen_font.sh")

    # --- 打包 ---
    body = bytearray()
    entries = []
    for name, data, fmt, w, h in items:
        while len(body) % 4:
            body.append(0)
        off = len(body)
        body += data
        entries.append((name, off, len(data), w, h, fmt))

    blob = bytearray()
    blob += struct.pack("<4sHHII", b"AMUS", 1, len(entries), len(body), 0)   # 16 字节
    for name, off, size, w, h, fmt in entries:
        nm = name.encode()[:27]
        blob += struct.pack("<28sIIHHB3s", nm, off, size, w, h, fmt, b"\0\0\0")
    assert len(blob) == HDR_SZ + ENTRY_SZ * len(entries)
    blob += body

    os.makedirs(os.path.dirname(OUT_BIN), exist_ok=True)
    with open(OUT_BIN, "wb") as f:
        f.write(blob)

    # --- 生成 C 头（asset id 枚举）---
    os.makedirs(os.path.dirname(OUT_HDR), exist_ok=True)
    with open(OUT_HDR, "w") as f:
        f.write("/* 本文件由 tools/gen_assets.py 生成，不要手改 */\n")
        f.write("#pragma once\n\n")
        f.write("typedef enum {\n")
        for name, _, _, _, _, _ in entries:
            f.write(f"    {name_to_id(name)},\n")
        f.write("    ASSET_COUNT,\n")
        f.write("} asset_id_t;\n\n")
        f.write("/* 便于调试：id → 文件名 */\n")
        f.write("static const char *asset_names[] = {\n")
        for name, _, _, _, _, _ in entries:
            f.write(f'    "{name}",\n')
        f.write("};\n")

    print(f"打包 {len(entries)} 个素材 → {os.path.relpath(OUT_BIN, ROOT)}  "
          f"({len(blob)/1024:.1f} KB)")
    for name, off, size, w, h, fmt in entries:
        kind = {FMT_FONT: "font", FMT_WEB_GZ: "web.gz", FMT_RAW: "blob"}.get(fmt, f"{w}x{h}")
        print(f"  {name:26s} {kind:>9s} {size/1024:7.1f} KB")
    print(f"索引头 → {os.path.relpath(OUT_HDR, ROOT)}")


if __name__ == "__main__":
    main()
