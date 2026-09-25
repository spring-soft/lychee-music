#!/usr/bin/env python3
"""
生成 ESP-Music 的预渲染 UI 底图（浅色系）—— 160x128 ST7735。

设计原则（对应方案里的"避免整屏重绘三条铁律"）：
  - 所有静态部件（顶栏、卡片、封面框、进度槽、圆角、描边）都画进底图，
    运行时只叠加动态内容（歌名、歌词、时间、封面、进度填充）。
  - 动态控件所在区域的父对象一律无填充/无圆角/无阴影 —— 圆角和阴影会把
    LVGL 的重绘区域撑到整块，这是最常见的性能陷阱；视觉上的圆角由底图提供。

输出到 assets_src/ui/*.png；再由 gen_assets.py 打包成 RGB565 的 assets.bin。
构图改这里，别改 C 代码。
"""
import os
from PIL import Image, ImageDraw

W, H = 160, 128
OUT = os.path.join(os.path.dirname(__file__), "..", "assets_src", "ui")

# ---------------- 浅色系调色板（暖中性，刻意避开蓝紫倾向）----------------
# ⚠️ 别再用冷灰（如 #F2F4F7）：蓝分量偏高，在 ST7735 这类 TN 屏上会整体泛紫，
#    用户实测直接说"页面是紫的太丑"。同理顶栏图标不要用蓝灰(148,163,184)。
BG        = (255, 255, 255)   # 页面底：纯白，大面积最不容易偏色
CARD      = (247, 247, 245)   # 卡片：近白暖灰
BORDER    = (232, 230, 226)   # 细描边（暖灰）
TOPBAR    = (255, 255, 255)
TOPBAR_LN = (236, 234, 230)   # 顶栏下分隔线
TRACK     = (237, 237, 234)   # 进度槽
ACCENT    = (26, 115, 232)    # 强调蓝（偏青的蓝，不像紫）
ICON_IDLE = (150, 150, 152)   # 中性灰图标（不用蓝灰）
HILITE    = (232, 240, 254)   # 列表选中（很淡的蓝）

TOPBAR_H = 20          # 加高：16px 高的顶栏放 16px 中文字会溢出到封面，实测重叠


def base():
    img = Image.new("RGB", (W, H), BG)
    return img, ImageDraw.Draw(img)


def card(d, box, radius=4, fill=CARD, outline=BORDER):
    d.rounded_rectangle(box, radius=radius, fill=fill, outline=outline, width=1)


def topbar(d, left_pad=4):
    """顶栏：白底 + 底部 1px 分隔线。左侧留出符号位（动态画）。"""
    d.rectangle((0, 0, W - 1, TOPBAR_H - 1), fill=TOPBAR)
    d.line((0, TOPBAR_H - 1, W - 1, TOPBAR_H - 1), fill=TOPBAR_LN)
    # 符号占位（画一个淡色音符，作为"没有图标时的兜底"，运行时会被真图标覆盖）
    cx, cy = left_pad + 4, 10
    d.ellipse((cx - 3, cy + 1, cx + 1, cy + 6), fill=ICON_IDLE)
    d.line((cx + 1, cy + 3, cx + 1, cy - 5), fill=ICON_IDLE, width=2)
    d.line((cx + 1, cy - 5, cx + 5, cy - 3), fill=ICON_IDLE, width=2)


def progress_slot(d, x, y, w, h=6):
    d.rounded_rectangle((x, y, x + w - 1, y + h - 1), radius=h // 2, fill=TRACK)


# ============================================================ 播放页（封面为主）
# 垂直预算（160x128，每行都按实际行高留够，别再出现"文字被切"）：
#   0..19    顶栏 20px   ：♪ + 歌名(16px 行高约19) + ★(12x12)
#   22..94   左封面 72x72；右信息卡 (81,22)-(155,92)
#              歌手 12px 两行 y=26/42（行高 16）
#              专辑 12px 两行 y=62/78（灰）
#   100..115 时间行 12px：已播(左) / 总时长(右)（行高 15）
#   118..123 全宽进度条 6px 圆角（贴底留 4px 边）
def bg_play_a():
    img, d = base()
    topbar(d)
    card(d, (5, 22, 5 + 71, 22 + 71), radius=5)
    card(d, (81, 22, 155, 92), radius=5)
    progress_slot(d, 5, 118, 150)
    return img


# ============================================================ 列表页（5 行）
def bg_list():
    img, d = base()
    topbar(d)
    ROW_H = 22
    y0 = TOPBAR_H
    for i in range(5):
        y = y0 + i * ROW_H
        if i:
            d.line((6, y, W - 7, y), fill=(232, 236, 241))
    return img


def hl_row():
    img = Image.new("RGBA", (W, 22), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((3, 1, W - 4, 20), radius=4, fill=HILITE + (255,))
    return img


# ============================================================ 菜单页（6 行）
def bg_menu():
    img, d = base()
    topbar(d)
    ROW_H = 17
    y0 = TOPBAR_H + 2
    for i in range(6):
        y = y0 + i * ROW_H
        if i:
            d.line((10, y, W - 11, y), fill=(232, 236, 241))
    return img


def hl_menu():
    img = Image.new("RGBA", (W, 17), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((3, 0, W - 4, 16), radius=4, fill=HILITE + (255,))
    return img


# ============================================================ 音量页
def bg_vol():
    img, d = base()
    topbar(d)
    card(d, (5, 26, 154, 96), radius=6)
    progress_slot(d, 12, 74, 136, h=10)
    return img


# ============================================================ 系统信息页
# 7 行 × 15px：12px 中文字的行高约 15，塞得下。
# 之前"系统信息"是一行 toast，用户反馈"根本显示不全"（IP + 内存 + 欠载一行放不下），
# 所以改成整页。
#   0..19    顶栏 20px
#   20..124  7 行 × 15px（y = 20/35/50/65/80/95/110）
INFO_ROWS = 7
INFO_ROW_H = 15


def bg_info():
    img, d = base()
    topbar(d)
    for i in range(INFO_ROWS):
        if i:
            y = TOPBAR_H + i * INFO_ROW_H
            d.line((6, y, W - 7, y), fill=(238, 238, 235))
    return img


# ============================================================ 图标（10x10 / 12x12）
def icon_canvas(size=12):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    return img, ImageDraw.Draw(img)


def ic_play(size=12, color=ACCENT):
    img, d = icon_canvas(size)
    d.polygon([(3, 2), (size - 2, size // 2), (3, size - 2)], fill=color + (255,))
    return img


def ic_pause(size=12, color=ACCENT):
    img, d = icon_canvas(size)
    d.rounded_rectangle((3, 2, 5, size - 3), radius=1, fill=color + (255,))
    d.rounded_rectangle((size - 6, 2, size - 4, size - 3), radius=1, fill=color + (255,))
    return img


def ic_next(size=12, color=ACCENT):
    img, d = icon_canvas(size)
    d.polygon([(2, 2), (8, size // 2), (2, size - 2)], fill=color + (255,))
    d.rectangle((size - 4, 2, size - 3, size - 3), fill=color + (255,))
    return img


def ic_prev(size=12, color=ACCENT):
    img, d = icon_canvas(size)
    d.polygon([(size - 3, 2), (3, size // 2), (size - 3, size - 2)], fill=color + (255,))
    d.rectangle((2, 2, 3, size - 3), fill=color + (255,))
    return img


def ic_star(filled, size=12, color=(250, 204, 21), off=(203, 213, 225)):
    img, d = icon_canvas(size)
    import math
    cx, cy, r = size / 2, size / 2 + 0.5, size / 2 - 1
    pts = []
    for i in range(10):
        ang = -math.pi / 2 + i * math.pi / 5
        rr = r if i % 2 == 0 else r * 0.45
        pts.append((cx + rr * math.cos(ang), cy + rr * math.sin(ang)))
    d.polygon(pts, fill=(color if filled else off) + (255,),
              outline=(color if filled else off) + (255,))
    return img


def ic_bt(size=12, color=(37, 99, 235)):
    img, d = icon_canvas(size)
    cx = size // 2
    d.line((cx, 1, cx, size - 2), fill=color + (255,), width=1)
    d.line((cx, 1, cx + 3, 4), fill=color + (255,), width=1)
    d.line((cx, size - 2, cx + 3, size - 5), fill=color + (255,), width=1)
    d.line((cx - 3, 4, cx + 3, size - 5), fill=color + (255,), width=1)
    d.line((cx - 3, size - 5, cx + 3, 4), fill=color + (255,), width=1)
    return img


def ic_wifi(size=12, color=(16, 185, 129)):
    img, d = icon_canvas(size)
    d.arc((0, 2, size - 1, size + 4), start=200, end=340, fill=color + (255,), width=2)
    d.arc((3, 5, size - 4, size + 1), start=200, end=340, fill=color + (255,), width=2)
    d.ellipse((size // 2 - 1, size - 4, size // 2 + 1, size - 2), fill=color + (255,))
    return img


def ic_note(size=12, color=(100, 116, 139)):
    img, d = icon_canvas(size)
    d.ellipse((1, size - 5, 5, size - 1), fill=color + (255,))
    d.ellipse((size - 6, size - 7, size - 2, size - 3), fill=color + (255,))
    d.line((5, size - 3, 5, 2), fill=color + (255,), width=1)
    d.line((size - 3, size - 5, size - 3, 1), fill=color + (255,), width=1)
    d.line((5, 2, size - 3, 1), fill=color + (255,), width=2)
    return img


# ---- 大号播放状态按钮（32x32，浅蓝圆底 + 蓝色图标；12x12 那个太小看不清）----
def ic_play_big(size=32):
    img, d = icon_canvas(size)
    d.ellipse((0, 0, size - 1, size - 1), fill=HILITE + (255,))
    d.polygon([(12, 9), (12, 23), (24, 16)], fill=ACCENT + (255,))
    return img


def ic_pause_big(size=32):
    img, d = icon_canvas(size)
    d.ellipse((0, 0, size - 1, size - 1), fill=HILITE + (255,))
    d.rounded_rectangle((11, 9, 14, 23), radius=2, fill=ACCENT + (255,))
    d.rounded_rectangle((18, 9, 21, 23), radius=2, fill=ACCENT + (255,))
    return img


# ============================================================ 启动页
# 开机要连 WiFi、拉歌单，实测 10~19 秒。没有反馈用户会以为卡死，所以给一个
# 看得见的进度：中间 logo，下面一行状态文字（动态），底部一条进度条（动态）。
def draw_note(d, cx, cy):
    """一个蓝色音符：两个实心圆（符头）+ 符杆 + 符梁。
    ⚠️ 单独抽出来是为了让启动页的它**能单独动** —— 底图是静态的，
       画在底图里就没法做"落下来弹一下"。所以底图只留浅蓝圆底，
       音符做成独立的 splash_note.png（带 alpha），LVGL 里叠在圆心上做动画。"""
    d.ellipse((cx - 12, cy + 2, cx - 2, cy + 12), fill=ACCENT)
    d.ellipse((cx + 2, cy - 6, cx + 12, cy + 4), fill=ACCENT)
    d.rectangle((cx - 4, cy - 12, cx - 2, cy + 8), fill=ACCENT)
    d.rectangle((cx + 10, cy - 20, cx + 12, cy), fill=ACCENT)
    d.polygon([(cx - 4, cy - 14), (cx + 12, cy - 22), (cx + 12, cy - 16), (cx - 4, cy - 8)],
              fill=ACCENT)


def splash_note():
    """音符单独一张图（带 alpha）。尺寸就是音符的外接框，见下面的注释。"""
    # 音符相对圆心：x ∈ [cx-12, cx+12]（25 px）、y ∈ [cy-22, cy+12]（35 px）
    img = Image.new("RGBA", (25, 35), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    draw_note(d, 12, 22)          # 把圆心挪到 (12, 22)，正好套进 25x35
    return img


def bg_splash():
    img, d = base()
    # 居中 logo：浅蓝圆底（音符单独一张图，见 splash_note()）
    cx, cy, r = W // 2, 40, 26
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(232, 240, 254))
    # 底部进度槽（动态填充叠上去）
    progress_slot(d, 5, 104, 150, h=6)
    return img


def placeholder_cover(size=100):
    img = Image.new("RGB", (size, size), (241, 245, 249))
    d = ImageDraw.Draw(img)
    d.ellipse((size * 0.30, size * 0.30, size * 0.70, size * 0.70), outline=(203, 213, 225), width=2)
    d.ellipse((size * 0.46, size * 0.46, size * 0.54, size * 0.54), fill=(203, 213, 225))
    return img


def main():
    os.makedirs(OUT, exist_ok=True)
    items = {
        "bg_play_a.png": bg_play_a(),
        "bg_list.png": bg_list(),
        "hl_row.png": hl_row(),
        "bg_menu.png": bg_menu(),
        "hl_menu.png": hl_menu(),
        "bg_vol.png": bg_vol(),
        "bg_info.png": bg_info(),
        "bg_splash.png": bg_splash(),
        "splash_note.png": splash_note(),
        "ic_play.png": ic_play(),
        "ic_play_big.png": ic_play_big(),
        "ic_pause_big.png": ic_pause_big(),
        "ic_pause.png": ic_pause(),
        "ic_next.png": ic_next(),
        "ic_prev.png": ic_prev(),
        "ic_star_on.png": ic_star(True),
        "ic_star_off.png": ic_star(False),
        "ic_bt.png": ic_bt(),
        "ic_wifi.png": ic_wifi(),
        "ic_note.png": ic_note(),
        "placeholder_cover.png": placeholder_cover(100),
        "placeholder_cover_72.png": placeholder_cover(72),
        "placeholder_cover_96.png": placeholder_cover(96),
    }
    total = 0
    for name, im in items.items():
        p = os.path.join(OUT, name)
        im.save(p)
        total += os.path.getsize(p)
        print(f"  {name:24s} {im.size[0]:3d}x{im.size[1]:<3d}")
    print(f"共 {len(items)} 个素材 → {os.path.normpath(OUT)}")


if __name__ == "__main__":
    main()
