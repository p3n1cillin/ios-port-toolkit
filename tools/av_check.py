"""Is a movie the port's camera recorded any good? Frame rate, A/V sync, gaps and an unfinished file, from ffprobe.

    python av_check.py IMG_0031.MOV [more.MOV ...] [--fps 30]

Pull the movie from the phone first (DCIM/100APPLE/IMG_*.MOV once Photos imported it; a movie still in
DCIM/.MISC/Incoming/ never finished). Needs ffprobe (FFmpeg) on PATH.

What it flags, each one a real failure on the S10 Lite (CAMERA-PLAN.md, Tests 430-434):
  * picture behind the sound: the video track is much shorter than the audio. The encoder (or the ISP before it) ran
    slower than real time (r5q Test 430: HEVC 1080p in software at ~5 fps; 55 frames, 10.1 s of video in 17 s of
    audio). iOS then waits at stop for frames that never come, and the movie is never handed to Photos;
  * a frame rate far under the one asked for (--fps), or long gaps between frames (dropped frames, a stalled sensor);
  * one track starting late (audio or video start_time over 100 ms);
  * no moov / no streams: the file was never finalised.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import shutil
import subprocess
import sys


def probe(path):
    exe = shutil.which('ffprobe')
    if exe is None:
        raise SystemExit('ffprobe is not on PATH (install FFmpeg)')
    r = subprocess.run([exe, '-v', 'error', '-show_streams', '-show_format', '-of', 'json', str(path)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        return None, r.stderr.strip()
    return json.loads(r.stdout), ''


def video_times(path):
    r = subprocess.run([shutil.which('ffprobe'), '-v', 'error', '-select_streams', 'v:0', '-show_entries',
                        'packet=pts_time', '-of', 'csv=p=0', str(path)], capture_output=True, text=True)
    times = []
    for line in r.stdout.split():
        try:
            times.append(float(line.strip(',')))
        except ValueError:
            pass
    return sorted(times)


def check(path, want_fps=None):
    info, err = probe(path)
    problems, facts = [], []
    if info is None or not info.get('streams'):
        return [f'not readable ({err or "no streams"}): never finalised (no moov), or not a movie'], facts
    v = next((s for s in info['streams'] if s.get('codec_type') == 'video'), None)
    a = next((s for s in info['streams'] if s.get('codec_type') == 'audio'), None)
    if v is None:
        problems.append('no video track')
    if a is None:
        facts.append('no audio track')
    vd = float(v.get('duration', 0) or 0) if v else 0
    ad = float(a.get('duration', 0) or 0) if a else 0
    if v:
        frames = int(v.get('nb_frames', 0) or 0)
        times = video_times(path)
        frames = frames or len(times)
        span = (times[-1] - times[0]) if len(times) > 1 else vd
        fps = (frames - 1) / span if span > 0 and frames > 1 else 0
        facts.append(f'video {v.get("codec_name")} {v.get("width")}x{v.get("height")}, {frames} frames in {vd:.2f} s '
                     f'({fps:.1f} fps), starts {float(v.get("start_time", 0) or 0):.3f} s')
        if want_fps and fps < 0.8 * want_fps:
            problems.append(f'{fps:.1f} fps where {want_fps} was asked: frames are dropped or come slower than real time')
        if len(times) > 2:
            gaps = [b - t for t, b in zip(times, times[1:])]
            big = max(gaps)
            typical = sorted(gaps)[len(gaps) // 2]
            if typical > 0 and big > 4 * typical and big > 0.15:
                at = times[gaps.index(big)]
                problems.append(f'a {big * 1000:.0f} ms gap between frames at {at:.2f} s (typical {typical * 1000:.0f} ms)')
        if float(v.get('start_time', 0) or 0) > 0.1:
            problems.append('the video starts late')
    if a:
        facts.append(f'audio {a.get("codec_name")} {a.get("sample_rate")} Hz, {ad:.2f} s, starts '
                     f'{float(a.get("start_time", 0) or 0):.3f} s')
        if float(a.get('start_time', 0) or 0) > 0.1:
            problems.append('the audio starts late')
    if v and a and ad > 0:
        if vd < 0.9 * ad:
            problems.append(f'the picture is behind the sound: {vd:.2f} s of video in {ad:.2f} s of audio -- the encoder '
                            f'or ISP is slower than real time (iOS will wait at stop and the movie may never reach Photos)')
        elif abs(vd - ad) > 0.25:
            facts.append(f'video and audio differ by {abs(vd - ad) * 1000:.0f} ms in length')
    return problems, facts


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('movies', nargs='+', type=pathlib.Path)
    ap.add_argument('--fps', type=float, help='the frame rate the recording was set to (30, 60)')
    a = ap.parse_args(argv)
    bad = 0
    for m in a.movies:
        problems, facts = check(m, a.fps)
        print(f'{m.name}: {"OK" if not problems else "PROBLEMS"}')
        for f in facts:
            print(f'    {f}')
        for p in problems:
            print(f'  ! {p}')
        bad += bool(problems)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
