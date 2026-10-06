# Local changes

- Build as a vendored component without the package-manager helper. Define version 2.0.0 locally, matching the upstream component manifest.
- Check the end of the `movi` list before reading another chunk. Its size includes the four-byte list type; FFmpeg's following `idx1` is an index, not a video frame. Stop correctly after all 60 frames, and fail on a zero/oversized read.

Only the PC-validated embedded, video-only MJPEG AVI is used by this MVP. This is not a general untrusted-file parser hardening patch.
