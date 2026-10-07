#!/usr/bin/env python3
"""
主机侧核对 ae_normalize() 的 24/32bit → 16bit 转换算法。

为什么需要它：这段转换在音频链路上，而这条链路只有真机才能实测 —— 板子还没到。
但"取高 16 位"这一步是纯算术，可以在这边把算法验干净：
  · 舍入是否等价于 floor(v/2^8 + 0.5) 再夹到 16bit
  · 正满幅进位会不会翻成 -32768（那是"最大正峰跳到最大负峰"的一声爆音）
  · 和旧的"直接截断"到底差多少（直流偏移 / 均方误差 / 受影响采样占比）

⚠️ 它验证的是**算法**，不是 C 代码本身 —— 改 C 的时候要同步改这里（两边都写着同样的
   字节运算）。真机侧另有 MUSICLIB_SELFTEST 那类开关，但这段没法用耳朵验。

跑法：python3 tools/check_pcm_round.py
"""
import math
import random
import statistics

FS24_MIN, FS24_MAX = -0x800000, 0x7FFFFF      # 24bit 有符号的有效范围


def pack24(v):
    """24bit 有符号 -> 3 字节小端（与解码器输出一致）"""
    return bytes([v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF])


def c_trunc(payload, in_bytes, off):
    """旧实现：直接取高 16 位（截断）"""
    hi = payload[off] | (payload[off + 1] << 8)
    return hi - 0x10000 if hi >= 0x8000 else hi


def c_round(payload, in_bytes, off):
    """新实现：逐字照抄 audio_engine.c 里 ae_normalize() 的字节运算"""
    hi = payload[off] | (payload[off + 1] << 8)
    if payload[off - 1] >= 0x80 and hi != 0x7FFF:      # 四舍五入 + 唯一溢出点夹住
        hi = (hi + 1) & 0xFFFF
    return hi - 0x10000 if hi >= 0x8000 else hi


def ref(v, shift=8):
    """参考实现：floor(v / 2^shift + 0.5)，再夹到 16bit"""
    return max(-32768, min(32767, math.floor(v / (1 << shift) + 0.5)))


def main():
    random.seed(20261006)
    # 有效 24bit 值：上下边界附近密集 + 大样本随机
    cases = list(range(-70000, 70001))
    cases += [FS24_MAX, FS24_MAX - 1, 0x7FFF80, 0x7FFF7F, 0x7FFF00,
              FS24_MIN, FS24_MIN + 1, -0x100, -0xFF, -1, 0, 1, 0xFF, 0x100]
    cases += [random.randint(FS24_MIN, FS24_MAX) for _ in range(400000)]

    bad, glitch, n = 0, 0, len(cases)
    err2_old = err2_new = 0.0
    diff = 0
    for v in cases:
        p = pack24(v)
        got_new, got_old = c_round(p, 3, 1), c_trunc(p, 3, 1)
        if got_new != ref(v):
            bad += 1
            if bad <= 5:
                print("  舍入不符: v=%d got=%d want=%d" % (v, got_new, ref(v)))
        # 正峰翻负 = 爆音，绝不允许
        if got_new == -32768 and v > 0:
            glitch += 1
        if got_new != got_old:
            diff += 1
        err2_old += (got_old * 256 - v) ** 2
        err2_new += (got_new * 256 - v) ** 2

    print("有效 24bit 样本数: %d" % n)
    print("舍入 == floor(v/256+0.5) 夹住 : %s（不符 %d）"
          % ("OK" if bad == 0 else "*** 失败 ***", bad))
    print("正满幅进位成 -32768（爆音）  : %s（%d 例）"
          % ("OK" if glitch == 0 else "*** 失败 ***", glitch))
    print("新旧结果不同的采样占比        : %.2f%%（低字节 ≥0x80 就进位，理论 50%%）"
          % (100.0 * diff / n))
    print("均方量化误差（24bit LSB^2）    : 旧截断 %.0f   新舍入 %.0f（RMS 量级相同）"
          % (err2_old / n, err2_new / n))
    print("等效 RMS 误差（16bit LSB）     : 旧 %.4f   新 %.4f（理论 1/√12 = %.4f）"
          % (math.sqrt(err2_old / n) / 256, math.sqrt(err2_new / n) / 256, 1 / math.sqrt(12)))

    # 直流偏移：抽一段"幅度远大于 1 LSB16"的信号，误差均值应分别为 -0.5 LSB16 和 0
    seg = [random.randint(-0x400000, 0x3FFFFF) for _ in range(200000)]
    e_old = statistics.fmean(c_trunc(pack24(v), 3, 1) * 256 - v for v in seg)
    e_new = statistics.fmean(c_round(pack24(v), 3, 1) * 256 - v for v in seg)
    print("误差均值（直流偏移，16bit LSB）: 旧截断 %+.4f   新舍入 %+.4f"
          % (e_old / 256, e_new / 256))

    # 32bit 输入走同一段逻辑（off=2，被丢的最高字节是 f[1]，参考要按 2^16 归一）
    bad32 = 0
    for _ in range(100000):
        v = random.randint(-0x80000000, 0x7FFFFFFF)
        p = bytes([v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF])
        if c_round(p, 4, 2) != ref(v, 16):
            bad32 += 1
    print("32bit 分支（取高 16 位 + 舍入）: %s（不符 %d）"
          % ("OK" if bad32 == 0 else "*** 失败 ***", bad32))


if __name__ == "__main__":
    main()
