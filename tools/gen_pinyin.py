#!/usr/bin/env python3
"""
生成屏上拼音输入法要用的表：**音节 → 候选汉字（按常用度排序）**。

为什么要自己造这张表：设备只有一个 160×128 的小屏和 K1-K4 四个键，
**打不出汉字**；而 Navidrome 服务端**不支持拼音搜索**（2026-09-25 实测：
搜 yue/ylkbd/zjl/caijianya 全部返回 0 结果，官方也没这功能）。
所以"拼音搜中文歌"这条路只能设备自己走：把字母在本地变成真汉字，再把汉字交给服务器搜。
这样搜索始终由服务器完成 —— **用户新加的歌立刻能搜到，不存在本地索引过时的问题**
（这正是用户否掉"本地建索引"方案的原因）。

数据来源用本机现成的：用户的中文输入法是 Rime 雾凇拼音，
`~/.local/share/fcitx5/rime/cn_dicts/` 下就是现成的表：
  8105.dict.yaml    通用规范汉字表，格式 `汉字<TAB>拼音<TAB>权重` —— 权重正好当常用度
  41448.dict.yaml   41448 字（含生僻字），**没有权重**，只用来兜底补 8105 没覆盖到的字
（这两句"格式是什么"是实测看的，不是猜的；词典里还有词语条目，靠"只取单字"过滤掉。）

⚠️ **只保留字库能显示的汉字**（与 assets_src/font/charset.txt 取交集）。
字库缺字在屏上是【一片空白】而不是方框 —— 候选列表里混进一个看不见的字，
用户选上去就是灾难。

输出 assets_src/pinyin/pinyin.bin（小端）：
    [0]  u32 magic "EPY1"
    [4]  u16 音节数 N
    [6]  u16 保留
    [8]  u32 候选总数 M
    [12] u32 保留
    [16] 音节表：N × { char name[8]; u16 off; u16 cnt; }   ← name 升序，供二分查找
    [...] 汉字段：M × u16（Unicode 码点，按音节分组、组内已按常用度降序）
    （off/cnt 是相对汉字段【元素】下标，不是字节偏移）

跑法：python3 tools/gen_pinyin.py
"""
import collections
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
DICT_MAIN = os.path.expanduser("~/.local/share/fcitx5/rime/cn_dicts/8105.dict.yaml")
DICT_FALLBACK = os.path.expanduser("~/.local/share/fcitx5/rime/cn_dicts/41448.dict.yaml")
CHARSET = os.path.join(ROOT, "assets_src", "font", "charset.txt")
OUT = os.path.join(ROOT, "assets_src", "pinyin", "pinyin.bin")

MAGIC = 0x31595045            # "EPY1" 小端读出来
NAME_LEN = 8                  # 最长音节是 chuang/zhuang = 6 个字母，8 字节留余量
MAX_SYL_LEN = 6


def is_hanzi(c: str) -> bool:
    """只认 CJK 表意文字（含扩展 A）—— 假名/标点/全角都不要"""
    o = ord(c)
    return (0x4E00 <= o <= 0x9FFF) or (0x3400 <= o <= 0x4DBF)


def load_charset() -> set:
    with open(CHARSET, encoding="utf-8") as f:
        return {c for c in f.read() if is_hanzi(c)}


def parse_dict(path: str, want_weight: bool):
    """产出 (汉字, 音节, 权重)。只取单字条目，跳过词语。"""
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line[:1] in ("#", "\n") or line.startswith("---") or line.startswith("..."):
                continue
            p = line.rstrip("\n").split("\t")
            if len(p) < 2:
                continue
            w, py = p[0], p[1]
            if len(w) != 1 or not is_hanzi(w):
                continue                 # 词语条目（或别的字符）跳过
            if " " in py or not py or len(py) > MAX_SYL_LEN:
                continue                 # 只要单音节；多音节拼音是词语的
            if any(ch < "a" or ch > "z" for ch in py):
                continue
            wt = 0
            if want_weight and len(p) > 2 and p[2].isdigit():
                wt = int(p[2])
            out.append((w, py, wt))
    return out


def main():
    charset = load_charset()
    print(f"字库里的汉字: {len(charset)}")

    # 音节 → {汉字: 权重}；同一 (音节,汉字) 取最大权重
    syl2han = collections.defaultdict(dict)
    for w, py, wt in parse_dict(DICT_MAIN, True):
        if w not in charset:
            continue
        syl2han[py][w] = max(syl2han[py].get(w, 0), wt)

    covered_main = {w for d in syl2han.values() for w in d}
    print(f"8105 主表覆盖: {len(covered_main)} 字（{100*len(covered_main)/max(len(charset),1):.1f}%）")

    # 生僻字兜底：主表没有的字从 41448 补（无权重 → 排在每个音节的最后）
    extra = 0
    for w, py, _ in parse_dict(DICT_FALLBACK, False):
        if w not in charset or w in covered_main:
            continue
        if w not in syl2han[py]:
            syl2han[py][w] = 0
            extra += 1
    print(f"41448 兜底补了: {extra} 字")

    covered = {w for d in syl2han.values() for w in d}
    missing = charset - covered
    print(f"最终覆盖: {len(covered)} / {len(charset)} 字"
          f"（{100*len(covered)/max(len(charset),1):.1f}%）")
    if missing:
        # 覆盖不到的（拼音表里没有的字）就是打不出来的字 —— 报出来，别装作没有
        print(f"  ⚠️ 打不出来的 {len(missing)} 个字: {''.join(sorted(missing))[:60]}…")

    # 每个音节内按 (权重降序, 码点升序) 排 —— 常用字在前，顺序稳定可复现
    sylls = sorted(syl2han.keys())
    for s in sylls:
        if len(s) > MAX_SYL_LEN:
            raise SystemExit(f"音节太长: {s}")

    cand = []
    table = []
    for s in sylls:
        items = sorted(syl2han[s].items(), key=lambda kv: (-kv[1], ord(kv[0])))
        off = len(cand)
        cand.extend(ord(w) for w, _ in items)
        table.append((s, off, len(items)))

    # 打包
    blob = bytearray()
    blob += struct.pack("<IHHII", MAGIC, len(table), 0, len(cand), 0)
    for name, off, cnt in table:
        assert off < 65536 and cnt < 65536, f"{name} 的 off/cnt 超 u16"
        blob += struct.pack("<8sHH", name.encode(), off, cnt)
    for cp in cand:
        blob += struct.pack("<H", cp)

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(blob)

    print(f"\n音节 {len(table)} 个，候选 {len(cand)} 条 → "
          f"{os.path.relpath(OUT, ROOT)}（{len(blob)/1024:.1f} KB）")
    for probe in ("yue", "yi", "hang", "dong", "lai", "bu"):
        for s, off, cnt in table:
            if s == probe:
                got = [chr(cand[off + i]) for i in range(min(cnt, 8))]
                print(f"  {probe:6s} 共 {cnt:3d} 个，前 8: {' '.join(got)}")
                break
        else:
            print(f"  {probe:6s} ⚠️ 表里没有这个音节")
    biggest = max(table, key=lambda t: t[2])
    print(f"  候选最多的音节: {biggest[0]}（{biggest[2]} 个）")


if __name__ == "__main__":
    main()
