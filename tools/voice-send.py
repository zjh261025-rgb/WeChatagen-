#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把音频文件发成微信「语音条」（真语音消息，不是文件）。

用法：
  py <WORKSPACE>\\scripts\\voice-send.py --path <音频文件> --target <wxid或chatroom>
  py ... --path a.mp3 --target wxid_EXAMPLE --duration-ms 3000

做法：ffmpeg 转 24kHz/16bit/单声道 PCM → pysilk 编码成 silk（带 0x02 前缀）→
POST 到本机 hook 的 /SendVoiceMsg。返回 JSON，deliveryConfirmed=false 表示
hook 只有 native 调用回执，真正送达以微信端为准。
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile
import urllib.request

WORKSPACE = r"<WORKSPACE>"
HOOK_BASE = "http://127.0.0.1:30001"
FFMPEG_CANDIDATES = [
    r"<HOME>\ffmpeg\ffmpeg-9.0.2-essentials_build\bin\ffmpeg.exe",
    "ffmpeg",
]


def read_text(path: str) -> str:
    with open(path, "r", encoding="utf-8") as f:
        return f.read().strip()


def hook_token() -> str:
    p = os.path.join(WORKSPACE, "state", "hook", "native-token.txt")
    token = read_text(p)
    if len(token) != 64:
        raise SystemExit("hook token 无效：%s" % p)
    return token


def account_id() -> str:
    # 账号从 hook 健康检查里取，避免写死。
    req = urllib.request.Request(
        HOOK_BASE + "/WeChatAgent/health",
        headers={"Authorization": "Bearer " + hook_token()},
    )
    with urllib.request.urlopen(req, timeout=8) as r:
        data = json.load(r)
    accounts = data.get("databaseAccounts") or []
    if not accounts:
        raise SystemExit("微信未登录或账号信息不可用，先登录微信")
    return accounts[0]


def ffmpeg_path() -> str:
    for c in FFMPEG_CANDIDATES:
        if c == "ffmpeg":
            try:
                subprocess.run([c, "-version"], capture_output=True, check=True)
                return c
            except Exception:
                continue
        if os.path.isfile(c):
            return c
    raise SystemExit("找不到 ffmpeg")


def to_pcm24k(src: str, dst: str) -> int:
    cmd = [
        ffmpeg_path(), "-y", "-v", "error", "-i", src,
        "-ac", "1", "-ar", "24000", "-f", "s16le", dst,
    ]
    subprocess.run(cmd, check=True, capture_output=True)
    size = os.path.getsize(dst)
    if size < 2:
        raise SystemExit("ffmpeg 转出的 PCM 是空的")
    return size // 2  # 采样数


def encode_silk(pcm_path: str) -> bytes:
    import pysilk  # py -m pip install pysilk
    with open(pcm_path, "rb") as f:
        pcm = f.read()
    silk = pysilk.encode(pcm, sample_rate=24000)
    if not (silk.startswith(b"\x02#!SILK_V3") or silk.startswith(b"#!SILK_V3")):
        silk = b"\x02" + silk
    return silk


def send_voice(path: str, target: str, duration_ms: int) -> dict:
    body = json.dumps(
        {"wxidorgid": target, "path": path, "durationMs": int(duration_ms)},
        ensure_ascii=False,
    ).encode("utf-8")
    req = urllib.request.Request(
        HOOK_BASE + "/SendVoiceMsg",
        data=body,
        method="POST",
        headers={
            "Authorization": "Bearer " + hook_token(),
            "X-WeChatAgent-Account": account_id(),
            "Content-Type": "application/json; charset=utf-8",
        },
    )
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read().decode("utf-8"))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--path", required=True, help="音频文件（mp3/wav/m4a 都行）")
    ap.add_argument("--target", required=True, help="wxid（私聊）或 xxx@chatroom（群）")
    ap.add_argument("--duration-ms", type=int, default=0, help="不填则按音频自动算")
    ap.add_argument("--keep-silk", default="", help="可选：把生成的 silk 存到指定路径")
    a = ap.parse_args()

    src = os.path.abspath(a.path)
    if not os.path.isfile(src):
        raise SystemExit("音频文件不存在：%s" % src)

    tmpdir = tempfile.mkdtemp(prefix="voice-send-")
    pcm = os.path.join(tmpdir, "a.pcm")
    samples = to_pcm24k(src, pcm)
    duration = a.duration_ms or int(round(samples / 24000 * 1000))
    silk = encode_silk(pcm)
    silk_path = a.keep_silk or os.path.join(tmpdir, "a.silk")
    with open(silk_path, "wb") as f:
        f.write(silk)

    result = send_voice(silk_path, a.target, duration)
    result["durationMs"] = duration
    result["silkBytes"] = len(silk)
    print(json.dumps(result, ensure_ascii=False))
    return 0 if result.get("ret") == 0 else 2


if __name__ == "__main__":
    sys.exit(main())
