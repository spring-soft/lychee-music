#!/usr/bin/env python3
"""
把 sdkconfig.local / sdkconfig.forced 里的 CONFIG_* 强制写进 sdkconfig。

为什么需要它：IDF 的 SDKCONFIG_DEFAULTS 只在【sdkconfig 不存在】或【某个符号还没被
赋值】时才起作用。一旦 sdkconfig 生成过，改 sdkconfig.defaults / sdkconfig.local 都
不会再影响它 —— 踩过一次：改了 sdkconfig.local 里的 WiFi SSID，重新 build+flash 之后
设备还是去连旧网络（日志里 `STA 已启动，SSID="..."` 打的是旧名字）。

所以：在 cmake configure 之前跑一遍这个脚本，把这两个文件的值覆盖进 sdkconfig。
只在真的有差异时才写文件（否则会不停触发 cmake 重新 configure）。

两个文件的分工：
  sdkconfig.local   —— 只有敏感值（WiFi 账号密码、Navidrome 地址账号），不进版本库
  sdkconfig.forced  —— 非敏感但**必须压过 menuconfig 残留**的开关。
                        起因：M7 要让 NimBLE 的内存走 PSRAM、并把 BLE 控制器的
                        活动数从 6 降到 2 来省内部 SRAM，这些改 sdkconfig.defaults
                        对已生成的 sdkconfig 是无效的（就是上面那个坑），
                        改 sdkconfig 又会被下一次 confgen 覆盖掉来源不明的状态。
                        放这里最省事：和 local 走同一条覆盖路径，且能进版本库。
"""
import os
import re
import sys

PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SDKCONFIG = os.path.join(PROJ, "sdkconfig")
SOURCES = [
    ("sdkconfig.local", True),    # 值里有密码，只报键名
    ("sdkconfig.forced", False),  # 非敏感，可以连值一起报
]


def read_overrides(path: str) -> dict:
    out = {}
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = re.match(r"^(CONFIG_\w+)=(.*)$", line.rstrip("\n"))
            if m:
                out[m.group(1)] = m.group(2)
    return out


def main() -> int:
    if not os.path.exists(SDKCONFIG):
        return 0

    with open(SDKCONFIG, encoding="utf-8") as f:
        src = f.read()

    total_changed = []
    for name, secret in SOURCES:
        overrides = read_overrides(os.path.join(PROJ, name))
        if not overrides:
            continue
        changed = []
        for key, val in overrides.items():
            pat = re.compile(r"^" + re.escape(key) + r"=.*$", re.M)
            notset = re.compile(r"^# " + re.escape(key) + r" is not set$", re.M)
            existing = pat.search(src)
            if existing:
                if existing.group(0) != "%s=%s" % (key, val):
                    src = pat.sub(lambda _m, s="%s=%s" % (key, val): s, src)
                    changed.append(key)
            elif notset.search(src):
                # ⚠️ 这一条是关键：menuconfig 里"没开"的选项在 sdkconfig 里是
                #    "# CONFIG_X is not set"，光追加 "CONFIG_X=y" 会留下两行互相
                #    矛盾的定义，confgen 会以其中一行为准（不是你想要的那行）。
                src = notset.sub(lambda _m, s="%s=%s" % (key, val): s, src)
                changed.append(key)
            else:
                src = src.rstrip("\n") + "\n%s=%s\n" % (key, val)
                changed.append(key)
        if changed:
            if secret:
                print("esp-music: %s 覆盖了 %s" % (name, ", ".join(changed)))
                # 值本身不打印（里面可能有 WiFi/Navidrome 密码），只报键名
            else:
                print("esp-music: %s 覆盖了 %d 项: %s"
                      % (name, len(changed), ", ".join(changed)))
            total_changed += changed

    if total_changed:
        with open(SDKCONFIG, "w", encoding="utf-8") as f:
            f.write(src)
    return 0


if __name__ == "__main__":
    sys.exit(main())
