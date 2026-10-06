"""Integration checks using a generated test pattern; never touches a device."""
import contextlib
import hashlib
import io
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import prepare_video

PROJECT = prepare_video.ROOT
SAMPLE = None


class PrepareVideoTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        global SAMPLE
        cls.sample_directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.sample_directory.cleanup)
        SAMPLE = Path(cls.sample_directory.name) / "sample.mp4"
        subprocess.run([str(prepare_video.FFMPEG), "-hide_banner", "-loglevel", "error",
                        "-nostdin", "-y", "-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30",
                        "-t", "20", "-c:v", "mpeg4", "-q:v", "5",
                        "-pix_fmt", "yuv420p", str(SAMPLE)], check=True)

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        for folder in ("main", "preview", "logs"):
            (self.root / folder).mkdir()
        shutil.copyfile(PROJECT / "main" / "diagnostic.jpg", self.root / "main" / "diagnostic.jpg")
        self.published = [self.root / folder / name for folder, name in (
            ("main", "clip.avi"), ("main", "clip_info.h"),
            ("logs", "video_validation.json"), ("preview", "first_frame_landscape.jpg"))]
        for path in self.published:
            path.write_bytes(b"previous working asset")
        self.root_patch = patch.object(prepare_video, "ROOT", self.root)
        self.root_patch.start()
        self.addCleanup(self.root_patch.stop)

    def rejected(self, source, seconds, message):
        previous = [path.read_bytes() for path in self.published]
        with self.assertRaisesRegex(ValueError, message):
            prepare_video.prepare(source, seconds)
        self.assertEqual(previous, [path.read_bytes() for path in self.published])

    def test_oversize_preserves_previous_assets(self):
        with patch.object(prepare_video, "MAX_CLIP_BYTES", 1024 * 1024):
            self.rejected(SAMPLE, 20, "视频文件过大.*1 MiB")

    def test_longer_clip_is_accepted(self):
        with contextlib.redirect_stdout(io.StringIO()):
            prepare_video.prepare(SAMPLE, 10)
        report = json.loads((self.root / "logs" / "video_validation.json").read_text())
        self.assertEqual((report["seconds"], report["frames"]), (10, 300))
        self.assertLess(report["clip_bytes"], prepare_video.MAX_CLIP_BYTES)

    def test_duration_exceeds_source(self):
        self.rejected(SAMPLE, 21, "超过原视频长度")

    def test_zero_duration(self):
        self.rejected(SAMPLE, 0, "大于 0")

    def test_missing_source(self):
        self.rejected(self.root / "missing.mp4", 5, "存在的 MP4")

    def test_valid_unicode_filename_and_user_duration(self):
        source = self.root / "测试 视频.mp4"
        shutil.copyfile(SAMPLE, source)
        with contextlib.redirect_stdout(io.StringIO()):
            prepare_video.prepare(source, 2)
        report = json.loads((self.root / "logs" / "video_validation.json").read_text())
        self.assertEqual((report["seconds"], report["frames"], report["fps"]), (2, 60, 30))
        self.assertEqual(report["video_size"], [1280, 720])
        self.assertEqual(report["clip_sha256"], hashlib.sha256((self.root / "main" / "clip.avi").read_bytes()).hexdigest())
        self.assertIn("#define CLIP_SECONDS 2", (self.root / "main" / "clip_info.h").read_text())
        self.assertIn(f"#define CLIP_BYTES {report['clip_bytes']}",
                      (self.root / "main" / "clip_info.h").read_text())


if __name__ == "__main__":
    unittest.main()
