#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
UDP 音频发送端（协议 v2）—— 用 PC 当"主机"，把 PCM 推给从机。

整条链路：

    WAV / 正弦波 -> 按 10ms 切片 -> 打 t_play_us 时刻戳 -> UDP 单播
                                                              |
                                     从机 recvfrom -> jitter buffer
                                                   -> 按 t_play_us 对齐出声
                                                   -> I2S1 -> NS4168 -> 喇叭

线格式（小端，20 字节头 + PCM）：

    offset 0   uint16 magic        固定 0xAA55
    offset 2   uint8  version      固定 2
    offset 3   uint8  flags        bit0 = 本次流的首帧
    offset 4   uint16 stream_id    一次流唯一标识（每次运行随机，从机据此清缓冲）
    offset 6   uint16 seq          包序号
    offset 8   uint64 t_play_us    该帧的基准播放时刻（**本机时钟**，微秒）
    offset 16  uint16 payload_len  PCM 字节数（不含头）
    offset 18  uint16 lead_ms      发送端提前量 D（ms）
    offset 20  int16  pcm[]        44100Hz / 单声道 / 16bit 小端

节奏：每帧在 `t_play_us - lead` 时刻发出。也就是说数据总是提前 `lead`
毫秒送到从机，从机攒进 jitter buffer，到点再播。lead 必须 ≥ 网络抖动 +
缓冲深度，默认 200ms。

发送用**绝对时刻**调度（不是"每帧 sleep 10ms"），所以某次 sleep 晚了，
后续帧会自动追回，误差不累积。如果落后到这一帧已经赶不上从机的出声时刻
（超过 `lead + 60ms` 余量），脚本会换一个 stream_id 重新锚定时间轴，
从机看到新流会清空缓冲、重新对齐。

这是**代替**主节点的测试工具，不要和主节点同时往同一台从机发 ——
两个不同的 stream_id 会让从机反复清缓冲。

用法：

    # 先发一段 440Hz 正弦，确认链路通不通（ip 看从机串口日志）
    python udp_audio_sender.py --host 192.168.31.45 --tone

    # 发 WAV（自动转单声道、自动重采样到 44.1k）
    python udp_audio_sender.py --host 192.168.31.45 --file song.wav

    # 发裸 PCM（s16le，默认 44.1k 单声道；其他格式用 --raw-rate/--raw-channels）
    python udp_audio_sender.py --host 192.168.31.45 --raw test.pcm

    # 循环播放
    python udp_audio_sender.py --host 192.168.31.45 --file song.wav --loop

    # 完全模拟主节点的帧节奏（15ms 一帧，661/662 采样交替）
    python udp_audio_sender.py --host 192.168.31.45 --file song.wav --master

    # 丢 2% 的包 + 0~15ms 抖动，测从机的 PLC 和 jitter buffer
    python udp_audio_sender.py --host 192.168.31.45 --tone --loop \
        --drop-pct 2 --jitter-ms 15
"""

import argparse
import array
import atexit
import math
import random
import socket
import struct
import sys
import time

# ======================== 与固件共用的常量 ========================

MAGIC = 0xAA55
VERSION = 2
RATE = 44100                    # 固件里 Amplifier 的 I2S 采样率，不能改
LEAD_US_DEFAULT = 200_000       # 发送端提前量 D
RX_MARGIN_US = 60_000           # 必须与固件的 UDIO_MARGIN_US 一致

# magic, version, flags, stream_id, seq, t_play_us, payload_len, lead_ms
HDR = struct.Struct("<HBBHHQHH")

FLAG_STREAM_START = 0x01

# 单个 UDP 载荷上限。以太网 MTU 1500 - IP/UDP 头 28 = 1472，
# 再留一点余量，避免 IP 分片（WiFi 上分片很伤）。
MAX_PAYLOAD = 1400

try:
    import numpy as np
    _HAS_NUMPY = True
except ImportError:
    _HAS_NUMPY = False


def now_us():
    return time.perf_counter_ns() // 1000


def _enable_high_res_timer():
    """Windows 默认定时器粒度约 15.6ms，`sleep(0.001)` 可能睡 15ms。
    timeBeginPeriod(1) 之后 sleep 才有 1ms 精度，发包节奏才稳。"""
    if sys.platform != "win32":
        return
    try:
        import ctypes
        ctypes.windll.winmm.timeBeginPeriod(1)
        atexit.register(ctypes.windll.winmm.timeEndPeriod, 1)
    except Exception:
        pass


def _sleep_until_us(target_us):
    """睡到绝对时刻 target_us。最后 ~1ms 忙等，别让 sleep 粒度毁掉节奏。"""
    while True:
        remain = target_us - now_us()
        if remain <= 0:
            return
        if remain > 1200:
            time.sleep((remain - 800) / 1_000_000.0)


# ======================== 音频准备 ========================


def _downmix(samples, nch):
    """多声道 -> 单声道（按声道平均）"""
    if nch == 1:
        return samples
    if _HAS_NUMPY:
        a = np.frombuffer(samples.tobytes(), dtype="<i2").reshape(-1, nch)
        return array.array("h", a.mean(axis=1).astype("<i2").tobytes())
    frames = len(samples) // nch
    mono = array.array("h", bytes(2 * frames))
    for i in range(frames):
        base = i * nch
        mono[i] = sum(samples[base + c] for c in range(nch)) // nch
    return mono


def _resample(samples, src_rate):
    """线性插值重采样到 RATE"""
    if src_rate == RATE or len(samples) == 0:
        return samples
    n_out = int(len(samples) * RATE / src_rate)
    if _HAS_NUMPY:
        x = np.frombuffer(samples.tobytes(), dtype="<i2").astype(np.float64)
        pos = np.linspace(0, len(x) - 1, n_out)
        y = np.interp(pos, np.arange(len(x)), x)
        return array.array("h", y.astype("<i2").tobytes())

    # 没装 numpy 时的纯 Python 版本：慢，但只跑一次
    step = src_rate / RATE
    out = array.array("h", bytes(2 * n_out))
    last = len(samples) - 1
    for i in range(n_out):
        pos = i * step
        i0 = int(pos)
        if i0 >= last:
            out[i] = samples[last]
            continue
        frac = pos - i0
        s0 = samples[i0]
        out[i] = int(s0 + (samples[i0 + 1] - s0) * frac)
    return out


def load_wav(path):
    """读 WAV -> array('h')，单声道 44100Hz"""
    import wave

    with wave.open(path, "rb") as w:
        nch = w.getnchannels()
        sw = w.getsampwidth()
        fr = w.getframerate()
        raw = w.readframes(w.getnframes())

    if sw != 2:
        sys.exit(f"只支持 16-bit PCM WAV，当前是 {sw * 8}-bit。\n"
                 f"先用 ffmpeg 转一下：\n"
                 f"  ffmpeg -i \"{path}\" -ac 1 -ar 44100 -sample_fmt s16 out.wav")

    samples = array.array("h")
    samples.frombytes(raw)
    if sys.byteorder != "little":
        samples.byteswap()

    print(f"  源文件: {nch} 声道 / {fr} Hz / 16 bit / "
          f"{len(samples) / nch / fr:.1f} 秒")

    samples = _downmix(samples, nch)
    if fr != RATE:
        print(f"  重采样: {fr} Hz -> {RATE} Hz"
              f"{'' if _HAS_NUMPY else '（无 numpy，纯 Python，慢一点）'}")
        samples = _resample(samples, fr)
    return samples


def load_raw(path, rate, nch):
    """读裸 PCM（16-bit little-endian）-> array('h')，单声道 44100Hz"""
    with open(path, "rb") as f:
        raw = f.read()

    raw = raw[:len(raw) // 2 * 2]           # 丢掉半个采样
    samples = array.array("h")
    samples.frombytes(raw)
    if sys.byteorder != "little":
        samples.byteswap()

    nch = max(1, nch)
    print(f"  源文件: {nch} 声道 / {rate} Hz / 16 bit / "
          f"{len(samples) / nch / rate:.1f} 秒")

    samples = _downmix(samples, nch)
    if rate != RATE:
        print(f"  重采样: {rate} Hz -> {RATE} Hz"
              f"{'' if _HAS_NUMPY else '（无 numpy，纯 Python，慢一点）'}")
        samples = _resample(samples, rate)
    return samples


def make_tone(seconds, freq):
    """生成正弦测试音"""
    n = int(RATE * seconds)
    amp = 0.3 * 32767      # 留足余量，别削顶
    buf = array.array("h", bytes(2 * n))
    for i in range(n):
        buf[i] = int(amp * math.sin(2 * math.pi * freq * i / RATE))
    return buf


def apply_gain(samples, volume):
    """0~100 的线性衰减。固件的软件音量用的是平方曲线，这里不复刻 ——
    PC 端只是想"小声试一下"，够用就行。"""
    if volume >= 100:
        return samples
    g = volume / 100.0
    if _HAS_NUMPY:
        a = np.frombuffer(samples.tobytes(), dtype="<i2").astype(np.float64) * g
        return array.array("h", a.astype("<i2").tobytes())
    return array.array("h", (int(s * g) for s in samples))


# ======================== 发送 ========================


def build_chunks(samples, packet_ms, master_cadence=False):
    """切片。每片字节数必须是偶数（16bit 对齐）。

    master_cadence=True 时复刻主节点的帧节奏：15ms 一帧 = 661.5 采样，
    661/662 交替（平均正好 44100 采样/秒）。
    """
    raw = samples.tobytes()

    if master_cadence:
        chunks = []
        pos = 0
        idx = 0
        while pos < len(raw):
            ns = 661 if (idx & 1) == 0 else 662
            c = raw[pos:pos + ns * 2]
            if len(c) % 2:
                c = c[:-1]
            if c:
                chunks.append(c)
            pos += ns * 2
            idx += 1
        return chunks

    n_samples = max(1, RATE * packet_ms // 1000)
    payload_bytes = n_samples * 2
    if payload_bytes > MAX_PAYLOAD:
        sys.exit(f"--packet-ms {packet_ms} 会产生 {payload_bytes} 字节的 UDP 载荷，"
                 f"超过 {MAX_PAYLOAD}（会触发 IP 分片）。请改成 "
                 f"{MAX_PAYLOAD // 2 * 1000 // RATE} ms 或更小。")

    chunks = []
    for i in range(0, len(raw), payload_bytes):
        c = raw[i:i + payload_bytes]
        if len(c) % 2:          # 尾片如果是奇数，砍掉半个采样
            c = c[:-1]
        if c:
            chunks.append(c)
    return chunks


def stream(sock, addr, chunks, lead_us, loop, drop_pct=0.0, jitter_us=0):
    """按 t_play_us 的绝对时刻发送。

    返回 (包数, 字节数, 耗时, 丢包数, 重锚次数, 时间轴覆盖的采样数)
    """
    stream_id = random.randint(1, 0xFFFF)
    seq = 0
    total_pkts = 0
    total_bytes = 0
    dropped = 0
    reanchors = 0
    samples_done = 0
    first_pkt = True

    t_start = time.perf_counter()
    # 第一帧的基准播放时刻：现在 + 提前量。发送时刻 = t_play - lead = 现在
    t0_play_us = now_us() + lead_us
    last_report = t_start

    print(f"  stream_id: {stream_id}")

    try:
        while True:
            for chunk in chunks:
                # 这一帧第一样本的基准播放时刻（本机时钟）。
                # 用累计采样数换算，尾片不是整帧长也不会累积误差。
                t_play_us = t0_play_us + samples_done * 1_000_000 // RATE
                send_at_us = t_play_us - lead_us

                now = now_us()
                late = now - send_at_us
                if late > lead_us + RX_MARGIN_US:
                    # 已经落后到"这一帧即使立刻发出，也赶不上从机的出声时刻"。
                    # 重新锚定时间轴并换流，从机看到新 stream_id 会清缓冲重来。
                    t_play_us = now + lead_us
                    t0_play_us = t_play_us - samples_done * 1_000_000 // RATE
                    send_at_us = now
                    stream_id = random.randint(1, 0xFFFF)
                    first_pkt = True
                    reanchors += 1
                    print(f"\n  [重锚] 落后 {late / 1000:.0f}ms，"
                          f"换流 {stream_id}（从机会重新对齐）")

                if jitter_us:
                    send_at_us += random.randint(0, jitter_us)
                _sleep_until_us(send_at_us)

                if drop_pct > 0 and random.random() * 100.0 < drop_pct:
                    dropped += 1
                else:
                    flags = FLAG_STREAM_START if first_pkt else 0
                    pkt = HDR.pack(MAGIC, VERSION, flags, stream_id,
                                   seq & 0xFFFF,
                                   t_play_us & 0xFFFFFFFFFFFFFFFF,
                                   len(chunk), lead_us // 1000) + chunk
                    sock.sendto(pkt, addr)
                    first_pkt = False
                    total_pkts += 1
                    total_bytes += len(chunk)

                seq = (seq + 1) & 0xFFFF
                samples_done += len(chunk) // 2

                now = time.perf_counter()
                if now - last_report >= 1.0:
                    last_report = now
                    audio_s = samples_done / RATE
                    wall_s = now - t_start
                    drift_ms = (wall_s - audio_s) * 1000
                    extra = f" / 丢 {dropped}" if dropped else ""
                    print(f"\r  已发 {total_pkts} 包 / {total_bytes / 1024:.0f} KB / "
                          f"音频 {audio_s:6.1f}s / 实际 {wall_s:6.1f}s / "
                          f"漂移 {drift_ms:+7.0f}ms{extra}", end="", flush=True)

            if not loop:
                break
            print()

    except KeyboardInterrupt:
        print("\n  已停止 (Ctrl+C)")

    return (total_pkts, total_bytes, time.perf_counter() - t_start,
            dropped, reanchors, samples_done)


# ======================== 入口 ========================


def main():
    # Windows 控制台默认按 GBK 输出，Git Bash / mintty 期望 UTF-8，
    # 不重设的话中文会变成乱码（"目标" → "Ŀ��"）。
    for _s in (sys.stdout, sys.stderr):
        try:
            _s.reconfigure(encoding="utf-8")
        except Exception:
            pass

    ap = argparse.ArgumentParser(
        description="把 PCM 通过 UDP 推给 Play_Slave 从机（协议 v2）",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    ap.add_argument("--host", required=True, help="从机 IP（看它的串口日志）")
    ap.add_argument("--port", type=int, default=5680, help="从机监听端口")
    ap.add_argument("--file", help="WAV 文件路径")
    ap.add_argument("--raw", help="裸 PCM 文件路径（16-bit little-endian）")
    ap.add_argument("--raw-rate", type=int, default=44100, help="裸 PCM 采样率 (Hz)")
    ap.add_argument("--raw-channels", type=int, default=1, help="裸 PCM 声道数")
    ap.add_argument("--tone", action="store_true", help="不读文件，直接发正弦测试音")
    ap.add_argument("--freq", type=float, default=440.0, help="测试音频率 (Hz)")
    ap.add_argument("--seconds", type=float, default=5.0, help="测试音时长 (秒)")
    ap.add_argument("--packet-ms", type=int, default=10, help="每个 UDP 包的音频时长")
    ap.add_argument("--volume", type=int, default=100, help="发送前衰减 0~100")
    ap.add_argument("--lead-ms", type=int, default=LEAD_US_DEFAULT // 1000,
                    help="发送端提前量 D（必须与固件约定一致）")
    ap.add_argument("--loop", action="store_true", help="循环播放")
    ap.add_argument("--master", action="store_true",
                    help="复刻主节点节奏：15ms 一帧，661/662 采样交替")
    ap.add_argument("--drop-pct", type=float, default=0.0,
                    help="随机丢包百分比，用来测试从机的 PLC")
    ap.add_argument("--jitter-ms", type=float, default=0.0,
                    help="每个包额外加 0~N ms 随机发送延迟，模拟网络抖动")
    args = ap.parse_args()

    if int(bool(args.file)) + int(bool(args.tone)) + int(bool(args.raw)) != 1:
        ap.error("--file / --tone / --raw 三选一")

    if args.master:
        args.packet_ms = 15
    args.drop_pct = max(0.0, min(100.0, args.drop_pct))
    args.jitter_ms = max(0.0, args.jitter_ms)

    lead_us = max(20, args.lead_ms) * 1000
    _enable_high_res_timer()

    print(f"目标: udp://{args.host}:{args.port}")
    print(f"格式: {RATE} Hz / 单声道 / 16 bit / 每包 {args.packet_ms}ms / "
          f"提前量 {lead_us // 1000}ms")

    if args.tone:
        print(f"音源: {args.freq:g} Hz 正弦波，{args.seconds:g} 秒")
        samples = make_tone(args.seconds, args.freq)
    elif args.raw:
        print(f"音源: {args.raw}（裸 PCM）")
        samples = load_raw(args.raw, args.raw_rate, args.raw_channels)
    else:
        print(f"音源: {args.file}")
        samples = load_wav(args.file)

    samples = apply_gain(samples, max(0, min(100, args.volume)))
    chunks = build_chunks(samples, args.packet_ms,
                          master_cadence=args.master)
    if not chunks:
        sys.exit("音频数据为空")

    payload = len(chunks[0])
    if args.master:
        print(f"  节奏: 主节点模式，{len(chunks)} 帧，661/662 采样交替 "
              f"(首帧 {payload} 字节)")
    else:
        print(f"  切片: {len(chunks)} 包，每包 {payload} 字节 PCM "
              f"({payload / 2:.0f} 采样 ≈ {payload / 2 / RATE * 1000:.1f}ms)")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 256 * 1024)
    addr = (args.host, args.port)

    print("开始发送（Ctrl+C 停止）")
    pkts, nbytes, elapsed, dropped, reanchors, samples_done = stream(
        sock, addr, chunks, lead_us, args.loop,
        drop_pct=args.drop_pct, jitter_us=int(args.jitter_ms * 1000))
    sock.close()

    audio_s = samples_done / RATE
    print(f"\n合计: {pkts} 包 / {nbytes / 1024:.0f} KB / "
          f"音频 {audio_s:.1f}s / 实际 {elapsed:.1f}s")
    if dropped or reanchors:
        print(f"  丢包 {dropped} 个，时间轴重锚 {reanchors} 次")


if __name__ == "__main__":
    main()
