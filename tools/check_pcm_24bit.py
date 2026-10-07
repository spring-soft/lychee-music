#!/usr/bin/env python3
"""
主机侧核对 ae_normalize() 的新输出格式转换（24bit 档）。

为什么需要它：这段转换在音频链路上，而这条链路只有真机才能实测。但"把采样摆到
32bit 槽里的哪个位置、舍入怎么做、正满幅会不会溢成爆音"全是纯算术 —— 可以在这边
验干净，把真机上要查的东西压缩到只剩"时钟对不对"和"寄存器写没写进去"。

它验的是【算法】，不是 C 代码本身：下面 c_* 三个函数是逐行照抄 audio_engine.c 里的
ae_msb32 / to24 / to16 / ae_normalize，改 C 时必须同步改这里。

核心保证（改动前后都必须成立）：
  1. 24bit 源 → 24bit 输出是【恒等】的（低 8 位补 0，不丢信息）→ 这才是本次改动的意义
  2. 32bit 源 → 24bit 输出 = 四舍五入到 24bit，并夹住正满幅
  3. 16bit 源 → 16bit 输出与改动前逐字节相同（memcpy 快路径 + 通用路径一致）
  4. 24bit 源 → 16bit 输出与 check_pcm_round.py 的结论一致（无回归）
  5. 正满幅进位【绝不】翻成负满幅（那是"最大正峰跳到最大负峰"的一声爆音）
  6. 字节序：内存里小端存放，I2S 按位从 MSB 串出 → 串出流的前 N 位必须等于源采样
  7. 单声道源复制成两声道

跑法：python3 tools/check_pcm_24bit.py
"""
import math
import random

MASK32 = 0xFFFFFFFF
FMT_16 = 16
FMT_24 = 24


# ---------------------------------------------------------------- C 侧镜像

def c_msb32(p, n):
    """照抄 ae_msb32()：小端拼装 + 左移到 MSB 对齐（无符号左移）"""
    v = 0
    for i in range(n):
        v |= p[i] << (8 * i)
    v &= MASK32
    return (v << (8 * (4 - n))) & MASK32


def c_land24(v):
    """照抄 24bit 档的落位：取 [31:8] + 按 bit7 四舍五入 + 夹住正满幅"""
    w = v & 0xFFFFFF00
    if (v & 0x80) and w != 0x7FFFFF00:
        w = (w + 0x100) & MASK32
    return w


def c_land16(v):
    """照抄 16bit 档的落位：取 [31:16] + 按 bit15 四舍五入 + 夹住正满幅"""
    w = v >> 16
    if (v & 0x8000) and w != 0x7FFF:
        w += 1
    return w & 0xFFFF


def c_normalize(payload, n, ch, out_fmt):
    """照抄 ae_normalize() 的输出形状（含 16bit 立体声快路径）"""
    if n == 2 and ch == 2 and out_fmt == FMT_16:
        return bytes(payload)                       # memcpy 快路径
    frame_in = n * ch
    frame_out = 8 if out_fmt == FMT_24 else 4
    frames = len(payload) // frame_in
    out = bytearray()
    for i in range(frames):
        f = payload[i * frame_in:(i + 1) * frame_in]
        u = [c_msb32(f, n), c_msb32(f[n:], n) if ch == 2 else None]
        if ch == 1:
            u[1] = u[0]
        for c in u:
            if out_fmt == FMT_24:
                w = c_land24(c)
                out += bytes([w & 0xFF, (w >> 8) & 0xFF, (w >> 16) & 0xFF, (w >> 24) & 0xFF])
            else:
                w = c_land16(c)
                out += bytes([w & 0xFF, (w >> 8) & 0xFF])
    return bytes(out)


# ---------------------------------------------------------------- 独立参考实现

def to_signed(u, bits=32):
    u &= (1 << bits) - 1
    return u - (1 << bits) if u >> (bits - 1) else u


def ref_land(v, drop_bits, out_bits):
    """独立参考：把 MSB 对齐值四舍五入到 out_bits 位，返回 MSB 对齐的位型"""
    s = to_signed(v)
    r = math.floor(s / (1 << drop_bits) + 0.5)
    lo, hi = -(1 << (out_bits - 1)), (1 << (out_bits - 1)) - 1
    r = max(lo, min(hi, r))
    return (r << drop_bits) & MASK32


def pack_le(v, n):
    return bytes([(v >> (8 * i)) & 0xFF for i in range(n)])


def pack_signed_le(s, n):
    return pack_le(s & ((1 << (8 * n)) - 1), n)


def read_pair(frame, off, n):
    return int.from_bytes(frame[off:off + n], 'little')


# ---------------------------------------------------------------- 主检查

def main():
    random.seed(20261007)
    fails = []
    def chk(cond, msg):
        if not cond:
            fails.append(msg)

    # ---- 1) 24bit 源 → 24bit 输出：必须恒等（逐位不丢） ----
    src24 = [0, 1, -1, 0x7FFFFF, -0x800000, 0x123456, -0x123456, 0x7FFFFF00 >> 8]
    src24 += [random.randint(-0x800000, 0x7FFFFF) for _ in range(200000)]
    bad = 0
    for s in src24:
        v = c_msb32(pack_signed_le(s, 3), 3)
        w = c_land24(v)
        if (to_signed(w) >> 8) != s or (w & 0xFF) != 0:
            bad += 1
    chk(bad == 0, "24bit 源 → 24bit 输出不是恒等（%d 例）" % bad)
    print("① 24bit 源 → 24bit 输出恒等          : %s（%d 例）"
          % ("OK" if bad == 0 else "*** 失败 ***", len(src24)))

    # ---- 2) 落位与独立参考一致（24bit 档 / 16bit 档） ----
    cases = [0, 1, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF,
             0x7FFFFF, 0x800000, 0xFFFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF]
    cases += [random.randint(0, MASK32 - 1) for _ in range(400000)]
    b24 = b16 = 0
    for v in cases:
        if c_land24(v) != ref_land(v, 8, 24):
            b24 += 1
        if c_land16(v) != (ref_land(v, 16, 16) >> 16):
            b16 += 1
    chk(b24 == 0, "24bit 落位与参考不符（%d 例）" % b24)
    chk(b16 == 0, "16bit 落位与参考不符（%d 例）" % b16)
    print("② 落位 == 四舍五入(参考实现)         : 24bit %s / 16bit %s"
          % ("OK" if b24 == 0 else "*** %d 例不符 ***" % b24,
             "OK" if b16 == 0 else "*** %d 例不符 ***" % b16))

    # ---- 3) 正满幅绝不翻成负满幅（爆音） ----
    glitch = 0
    for v in cases:
        s = to_signed(v)
        if s > 0:
            if to_signed(c_land24(v)) < 0:
                glitch += 1
            if c_land16(v) == 0x8000:
                glitch += 1
    # 专门打正满幅附近
    for v in list(range(0x7FFFFF00, 0x80000000, 0x1000)) + list(range(0x7FFF0000, 0x80000000, 0x10000)):
        if to_signed(v) > 0:
            if to_signed(c_land24(v)) < 0 or c_land16(v) == 0x8000:
                glitch += 1
    chk(glitch == 0, "正满幅翻成负满幅（%d 例）" % glitch)
    print("③ 正满幅进位不翻负满幅（无爆音）     : %s（%d 例）"
          % ("OK" if glitch == 0 else "*** 失败 ***", glitch))

    # ---- 4) 16bit 源 → 16bit 输出：快路径与通用路径都必须逐字节等于输入 ----
    bad = 0
    for _ in range(20000):
        a = random.randint(-32768, 32767)
        b = random.randint(-32768, 32767)
        pay = pack_signed_le(a, 2) + pack_signed_le(b, 2)
        fast = c_normalize(pay, 2, 2, FMT_16)
        # 通用路径（关掉快路径的方式：单声道走不了快路径，这里手工构造 stereo 通用路径）
        v0 = c_msb32(pay[0:2], 2)
        v1 = c_msb32(pay[2:4], 2)
        gen = bytes([c_land16(v0) & 0xFF, (c_land16(v0) >> 8) & 0xFF,
                     c_land16(v1) & 0xFF, (c_land16(v1) >> 8) & 0xFF])
        if fast != pay or gen != pay:
            bad += 1
    chk(bad == 0, "16bit→16bit 不是恒等（%d 例）" % bad)
    print("④ 16bit 源 → 16bit 输出逐字节不变     : %s（快路径与通用路径都验了）"
          % ("OK" if bad == 0 else "*** 失败 ***"))

    # ---- 5) 16bit 源 → 24bit 输出：数值必须等于 16bit 值左移到 [31:16] ----
    bad = 0
    for _ in range(50000):
        s = random.randint(-32768, 32767)
        v = c_msb32(pack_signed_le(s, 2), 2)
        w = c_land24(v)
        if to_signed(w) != (s << 16) or (w & 0xFFFF) != 0:
            bad += 1
    chk(bad == 0, "16bit 源 → 24bit 输出值不对（%d 例）" % bad)
    print("⑤ 16bit 源 → 24bit 输出 == s<<16     : %s" % ("OK" if bad == 0 else "*** 失败 ***"))

    # ---- 6) 32bit 源 → 24bit：等于四舍五入到 24bit，且不丢符号 ----
    bad = 0
    for _ in range(200000):
        s = random.randint(-0x80000000, 0x7FFFFFFF)
        v = c_msb32(pack_signed_le(s, 4), 4)
        w = c_land24(v)
        want = ref_land(v, 8, 24)
        if w != want:
            bad += 1
    chk(bad == 0, "32bit 源 → 24bit 与参考不符（%d 例）" % bad)
    print("⑥ 32bit 源 → 24bit 与参考一致        : %s" % ("OK" if bad == 0 else "*** 失败 ***"))

    # ---- 7) 字节序 / MSB-first 串出：串出流的前 N 位必须等于源采样 ----
    # I2S 外设把内存里的 32bit 字按位从 MSB 开始串出，所以把输出字节展开成位流后，
    # 前 24 位必须正好是源 24bit 采样。
    bad = 0
    for s in [0x7FFFFF, -0x800000, 0x123456, -1, 1, 0x400000, -0x400000]:
        out = c_normalize(pack_signed_le(s, 3) + pack_signed_le(s, 3), 3, 2, FMT_24)
        word = int.from_bytes(out[0:4], 'little')
        bits = format(word, '032b')
        got = to_signed(int(bits[:24], 2), 24)
        if got != s:
            bad += 1
    chk(bad == 0, "MSB-first 串出流前 24 位不等于源采样（%d 例）" % bad)
    print("⑦ MSB-first 串出：前 24 位 == 源采样 : %s" % ("OK" if bad == 0 else "*** 失败 ***"))

    # ---- 8) 单声道源复制成两声道 ----
    bad = 0
    for _ in range(20000):
        s = random.randint(-0x800000, 0x7FFFFF)
        out = c_normalize(pack_signed_le(s, 3), 3, 1, FMT_24)
        if len(out) != 8:
            bad += 1
            continue
        l = int.from_bytes(out[0:4], 'little')
        r = int.from_bytes(out[4:8], 'little')
        if l != r or (to_signed(l) >> 8) != s:
            bad += 1
    chk(bad == 0, "单声道复制成两声道不对（%d 例）" % bad)
    print("⑧ 单声道源 → 两声道内容相同且正确    : %s" % ("OK" if bad == 0 else "*** 失败 ***"))

    # ---- 9) 输出长度 / 每帧字节数 ----
    bad = 0
    for n, ch, fmt, want_frame in [(2, 2, FMT_16, 4), (3, 2, FMT_24, 8),
                                   (4, 2, FMT_24, 8), (2, 1, FMT_16, 4), (3, 1, FMT_24, 8)]:
        frames = 37
        pay = bytes([random.randint(0, 255) for _ in range(frames * n * ch)])
        out = c_normalize(pay, n, ch, fmt)
        if len(out) != frames * want_frame:
            bad += 1
    chk(bad == 0, "输出长度不对（%d 例）" % bad)
    print("⑨ 输出长度 == 帧数 × 每帧字节数       : %s" % ("OK" if bad == 0 else "*** 失败 ***"))

    # ---- 10) 24bit 源 → 16bit 输出：与 check_pcm_round.py 的旧结论一致（无回归） ----
    def old_c_round(p, off=1):
        hi = p[off] | (p[off + 1] << 8)
        if p[off - 1] >= 0x80 and hi != 0x7FFF:
            hi = (hi + 1) & 0xFFFF
        return hi
    bad = 0
    for _ in range(200000):
        s = random.randint(-0x800000, 0x7FFFFF)
        pay = pack_signed_le(s, 3)
        v = c_msb32(pay, 3)
        got = c_land16(v)
        want = old_c_round(pay)
        if got != want:
            bad += 1
    chk(bad == 0, "24bit→16bit 与旧实现不一致（%d 例）" % bad)
    print("⑩ 24bit→16bit == 改动前的舍入实现     : %s" % ("OK" if bad == 0 else "*** 失败 ***"))

    print()
    if fails:
        print("*** 有 %d 项失败 ***" % len(fails))
        for f in fails:
            print("   - " + f)
        return 1
    print("全部通过。")
    print()
    print("注意：本脚本验的是【算法】。真机上仍需确认的是另外两件事 ——")
    print("  · 实测 WS 是否仍等于采样率（槽位宽从 16 改到 32 时 bclk_div 必须跟着改）")
    print("  · ES8388 寄存器 0x17 是否真的写成 0x00（24bit）/ 0x18（16bit）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
