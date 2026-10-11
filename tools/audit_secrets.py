#!/usr/bin/env python3
"""推 GitHub 之前的凭据审计。

一旦把带密码的提交推上去，**事后删掉也没用** —— 历史里还在，只能改写历史或删仓库。
所以推之前必须查：密码有没有在**任何**提交里出现过。

做法：从 sdkconfig.local 拿出真实值（WiFi 密码、Navidrome 密码等），
用 `git log --all -S<值>` 在**全部分支**的历史里找。只打印"命中几个提交"，
**不打印密码本身**（日志会被看到）。
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, ".."))
LOCAL = os.path.join(REPO, "sdkconfig.local")

# 哪些键算敏感（值是密码/令牌）
SENSITIVE = re.compile(r"PASS|PASSWORD|TOKEN|SECRET|KEY|CRED", re.I)


def read_local(path):
    """从 sdkconfig 风格的文件里取出 K=V（值可能带引号）。"""
    out = {}
    if not os.path.exists(path):
        return out
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        v = v.strip().strip('"')
        out[k.strip()] = v
    return out


def git(*args):
    p = subprocess.run(["git", "-C", REPO] + list(args),
                       capture_output=True, text=True, errors="replace")
    return p.returncode, p.stdout, p.stderr


def main():
    vals = read_local(LOCAL)
    print("sdkconfig.local 里共 %d 个键" % len(vals))

    # ---- 1. 敏感文件本身有没有被提交过 ----
    print("\n--- 敏感文件是否曾被提交（任何分支）---")
    for name in ("sdkconfig", "sdkconfig.local", "sdkconfig.local.*", ".env"):
        rc, out, _ = git("log", "--all", "--oneline", "--", name)
        n = len([l for l in out.splitlines() if l.strip()])
        print("  %-22s %s" % (name, "命中 %d 个提交  <== 要看！" % n if n else "从未提交 OK"))

    # ---- 2. 敏感值有没有在历史里出现过（字符串检索）----
    print("\n--- 敏感值是否出现在任何提交的 diff 里 ---")
    checked = 0
    bad = 0
    for k, v in sorted(vals.items()):
        if not SENSITIVE.search(k):
            continue
        if len(v) < 4:              # 空值/占位不值一查
            continue
        checked += 1
        rc, out, _ = git("log", "--all", "--oneline", "-S", v)
        n = len([l for l in out.splitlines() if l.strip()])
        status = "命中 %d 个提交  <== 要看！" % n if n else "未出现 OK"
        if n:
            bad += 1
        # 只打印键名，不打印值
        print("  %-26s (长度 %d)  %s" % (k, len(v), status))

    print("\n检查了 %d 个敏感键" % checked)
    if bad:
        print("FAILED: 有 %d 个敏感值在历史里出现过 —— 先处理再推！" % bad)
        return 1
    print("OK: 历史里没有找到任何敏感值")
    return 0


if __name__ == "__main__":
    sys.exit(main())
