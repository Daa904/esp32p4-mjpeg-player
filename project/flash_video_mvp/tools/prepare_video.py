"""Prepare one MP4 for this board; publish assets only after validation succeeds."""
import argparse
import hashlib
import json
import math
import os
import struct
import subprocess
import tempfile
from io import BytesIO
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
FFMPEG = Path(os.environ.get("VIDEO_FFMPEG", "ffmpeg"))
FFPROBE = Path(os.environ.get("VIDEO_FFPROBE", str(FFMPEG.with_name("ffprobe.exe")) if FFMPEG.suffix else "ffprobe"))
# Owner presets. End users choose only their source video and duration.
FPS = 30
VIDEO_WIDTH, VIDEO_HEIGHT = 1280, 720
PANEL_WIDTH, PANEL_HEIGHT = 1280, 800
JPEG_QUALITY = 12
MAX_CLIP_BYTES = 29 * 1024 * 1024  # Dedicated video data partition; app has its own 2 MiB.


def prepare(source, seconds):
    if seconds <= 0:
        raise ValueError("播放时长必须是大于 0 的整数秒")
    if not source.is_file() or source.suffix.lower() != ".mp4":
        raise ValueError("请选择存在的 MP4 文件")
    for folder in ("logs", "preview"):
        (ROOT / folder).mkdir(exist_ok=True)
    probe = subprocess.run([
        str(FFPROBE), "-v", "error", "-select_streams", "v:0",
        "-show_entries", "stream=duration:format=duration", "-of", "json", str(source)
    ], check=True, capture_output=True, text=True, encoding="utf-8")
    metadata = json.loads(probe.stdout)
    if not metadata.get("streams"):
        raise ValueError("MP4 文件中没有视频轨道")
    duration = metadata["streams"][0].get("duration", metadata.get("format", {}).get("duration"))
    if duration in (None, "N/A") or not math.isfinite(float(duration)):
        raise ValueError("无法确定视频时长，请先用电脑检查该视频")
    if seconds > float(duration) + 0.001:
        raise ValueError(f"播放时长超过原视频长度（{float(duration):.3f} 秒）")

    # Work separately so invalid/oversized input never replaces working assets.
    with tempfile.TemporaryDirectory(prefix="video-", dir=ROOT / "logs") as temporary:
        stage = Path(temporary)
        clip_path = stage / "clip.avi"
        subprocess.run([
            str(FFMPEG), "-hide_banner", "-loglevel", "error", "-nostdin", "-y",
            "-i", str(source), "-map", "0:v:0", "-t", str(seconds), "-an",
            "-vf", f"fps={FPS},scale={VIDEO_WIDTH}:{VIDEO_HEIGHT}:force_original_aspect_ratio=decrease,"
            f"pad={PANEL_WIDTH}:{PANEL_HEIGHT}:(ow-iw)/2:(oh-ih)/2,setsar=1,transpose=1",
            "-c:v", "mjpeg", "-q:v", str(JPEG_QUALITY), "-pix_fmt", "yuvj420p",
            "-fs", str(MAX_CLIP_BYTES + 1), "-f", "avi", str(clip_path)
        ], check=True)
        avi = clip_path.read_bytes()
        if len(avi) > MAX_CLIP_BYTES:
            raise ValueError(f"视频文件过大：转码后超过 {MAX_CLIP_BYTES // (1024 * 1024)} MiB 素材预算，请缩短播放时长；未编译或烧录")
        assert avi[:4] == b"RIFF" and avi[8:12] == b"AVI "

        def chunks(start, end):
            while start + 8 <= end:
                size = struct.unpack_from("<I", avi, start + 4)[0]
                assert start + 8 + size <= end
                yield avi[start:start + 4], start + 8, size
                start += 8 + size + (size & 1)
            assert start == end

        movi = next((p + 4, s - 4) for tag, p, s in chunks(12, len(avi))
                    if tag == b"LIST" and avi[p:p + 4] == b"movi")
        hdrl_pos, hdrl_size = next((p, s) for tag, p, s in chunks(12, len(avi))
                                  if tag == b"LIST" and avi[p:p + 4] == b"hdrl")
        headers = list(chunks(hdrl_pos + 4, hdrl_pos + hdrl_size))
        assert headers[0][0] == b"avih" and headers[0][2] == 56
        expected_frames = seconds * FPS
        assert struct.unpack_from("<I", avi, headers[0][1] + 16)[0] == expected_frames
        assert struct.unpack_from("<I", avi, headers[0][1] + 24)[0] == 1
        stream_pos, stream_size = next((p, s) for tag, p, s in headers
                                      if tag == b"LIST" and avi[p:p + 4] == b"strl")
        stream = list(chunks(stream_pos + 4, stream_pos + stream_size))
        assert stream[0][0] == b"strh" and stream[0][2] == 56
        assert avi[stream[0][1]:stream[0][1] + 8] == b"vidsMJPG"
        scale, rate = struct.unpack_from("<II", avi, stream[0][1] + 20)
        assert scale > 0 and rate == FPS * scale
        assert stream[1][0] == b"strf" and stream[1][2] == 40
        assert struct.unpack_from("<ii", avi, stream[1][1] + 4) == (PANEL_HEIGHT, PANEL_WIDTH)
        frames = []
        for tag, pos, size in chunks(movi[0], sum(movi)):
            assert tag == b"00dc", f"Unexpected AVI chunk: {tag!r}"
            with Image.open(BytesIO(avi[pos:pos + size])) as frame:
                frame.load()
                assert frame.size == (PANEL_HEIGHT, PANEL_WIDTH) and not frame.info.get("progressive")
                if not frames:
                    frame.transpose(Image.Transpose.ROTATE_90).save(stage / "first_frame_landscape.jpg")
            frames.append(size)
        assert len(frames) == expected_frames, f"Expected {expected_frames} frames, got {len(frames)}"
        diagnostic_size = (ROOT / "main" / "diagnostic.jpg").stat().st_size
        buffer_size = (max(max(frames) + 1, diagnostic_size, movi[0]) + 63) & ~63
        # The AVI parser initially reads a full input buffer from the mapped AVI.
        if buffer_size > len(avi):
            raise ValueError("片段过短，无法满足播放器缓冲要求，请增加播放时长")
        (stage / "clip_info.h").write_text(
            f"#pragma once\n#define CLIP_WIDTH {PANEL_HEIGHT}\n#define CLIP_HEIGHT {PANEL_WIDTH}\n"
            f"#define CLIP_FPS {FPS}\n#define CLIP_FRAMES {len(frames)}\n"
            f"#define CLIP_SECONDS {seconds}\n#define CLIP_BYTES {len(avi)}\n"
            f"#define CLIP_BUFFER_SIZE {buffer_size}\n", encoding="utf-8")
        report = {"source": str(source), "clip_sha256": hashlib.sha256(avi).hexdigest(),
                  "clip_bytes": len(avi), "frames": len(frames), "seconds": seconds, "fps": FPS,
                  "video_size": [VIDEO_WIDTH, VIDEO_HEIGHT], "native_size": [PANEL_HEIGHT, PANEL_WIDTH],
                  "mjpeg_q": JPEG_QUALITY, "max_jpeg_bytes": max(frames), "buffer_bytes": buffer_size,
                  "rotation": "clockwise 90 degrees before encoding", "all_frames_cpu_decoded": True}
        (stage / "video_validation.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        for name, folder in (("clip.avi", "main"), ("clip_info.h", "main"),
                             ("first_frame_landscape.jpg", "preview"), ("video_validation.json", "logs")):
            (stage / name).replace(ROOT / folder / name)
        print(f"素材校验通过：{seconds} 秒，{FPS} fps，{len(frames)} 帧，"
              f"转码后 {len(avi) / 1024 / 1024:.2f} MiB / {MAX_CLIP_BYTES // (1024 * 1024)} MiB。")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="MP4 to fixed-preset MJPEG AVI")
    parser.add_argument("--video", required=True, type=Path)
    parser.add_argument("--seconds", required=True, type=int)
    args = parser.parse_args()
    try:
        prepare(args.video.resolve(), args.seconds)
    except (ValueError, OSError, subprocess.CalledProcessError, AssertionError, StopIteration) as error:
        parser.exit(1, f"错误：{error or '转码结果校验失败'}\n")
