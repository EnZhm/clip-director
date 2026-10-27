#!/usr/bin/env python3
"""cd-ingest: prepare a clip folder for Clip Director (spec section 5). Stdlib only.

  python cd-ingest.py "D:/Clips/cyber" --dry-run
  python cd-ingest.py "D:/Clips/cyber" -j 3

For every video in SRC it writes, into SRC/_cd/ (never touching the originals):
  <name>.mp4                     1920x1080 cover-crop, 30 fps, H.264, GOP 15 (0.5 s), no B-frames,
                                 closed GOP, limited range, no audio, faststart
  .clipdirector/thumbs/<name>.jpg   320x180 thumbnail (the most typical frame)
  .clipdirector/index.json       duration, fps, keyframe interval, levels stats per clip
  .clipdirector/originals.sha256 hashes of the originals before the run (checked again after)

Resumable: a clip is skipped when its output exists and the source size/mtime and recipe hash
match the index. Levels are measured on the encoded output (what Clip Director will play) at
1 frame per second, 64x36 grey.
"""
import argparse
import concurrent.futures as cf
import hashlib
import json
import os
import shutil
import subprocess
import sys
import threading
import time

VIDEO_EXT = {".mp4", ".mov", ".mkv", ".webm", ".m4v"}
W, H, FPS, GOP = 1920, 1080, 30, 15

RECIPE = {
    "v": 1,
    "size": f"{W}x{H}",
    "fit": "cover",
    "fps": FPS,
    "gop": GOP,
    "bframes": 0,
    "cq": 23,
    "maxrate": "15M",
    "bufsize": "30M",
    "range": "tv",
}
RECIPE_HASH = hashlib.sha1(json.dumps(RECIPE, sort_keys=True).encode()).hexdigest()[:12]

lock = threading.Lock()


def log(msg):
    with lock:
        print(time.strftime("%H:%M:%S"), msg, flush=True)


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, **kw)


def probe(ff, path):
    r = run([ff["ffprobe"], "-v", "error", "-select_streams", "v:0", "-show_entries",
             "stream=width,height,r_frame_rate,pix_fmt:format=duration", "-of", "json", path], text=True,
            encoding="utf-8", errors="replace")
    d = json.loads(r.stdout or "{}")
    s = (d.get("streams") or [{}])[0]
    num, _, den = (s.get("r_frame_rate") or "0/1").partition("/")
    fps = float(num) / float(den or 1) if float(den or 1) else 0.0
    return {"w": s.get("width"), "h": s.get("height"), "fps": fps, "pix_fmt": s.get("pix_fmt"),
            "duration": float((d.get("format") or {}).get("duration") or 0.0)}


def keyframe_interval(ff, path):
    r = run([ff["ffprobe"], "-v", "error", "-select_streams", "v:0", "-skip_frame", "nokey", "-show_entries",
             "frame=pts_time", "-read_intervals", "%+20", "-of", "csv=p=0", path], text=True)
    t = []
    for x in r.stdout.split():
        try:
            t.append(float(x.strip(",")))
        except ValueError:
            pass
    return (t[-1] - t[0]) / (len(t) - 1) if len(t) > 1 else 0.0


def encoder_args(encoder):
    common = ["-g", str(GOP), "-bf", "0"]
    if encoder == "h264_nvenc":
        return ["-c:v", "h264_nvenc", "-preset", "p5", "-rc", "vbr", "-cq", str(RECIPE["cq"]), "-b:v", "0",
                "-maxrate", RECIPE["maxrate"], "-bufsize", RECIPE["bufsize"], "-strict_gop", "1",
                "-no-scenecut", "1", "-forced-idr", "1"] + common
    # fallback (no NVIDIA GPU): same GOP structure, comparable quality
    return ["-c:v", "libx264", "-preset", "medium", "-crf", "21", "-maxrate", RECIPE["maxrate"], "-bufsize",
            RECIPE["bufsize"], "-x264-params", "scenecut=0:open-gop=0", "-pix_fmt", "yuv420p"] + common


def encode(ff, src, dst, encoder):
    vf = (f"scale={W}:{H}:force_original_aspect_ratio=increase:flags=lanczos:out_range=tv,"
          f"crop={W}:{H},fps={FPS},format=yuv420p")
    tmp = dst + ".part.mp4"
    cmd = [ff["ffmpeg"], "-y", "-hide_banner", "-loglevel", "error", "-i", src, "-an", "-sn", "-dn",
           "-map_metadata", "-1", "-vf", vf, "-color_range", "tv"] + encoder_args(encoder) + [
           "-movflags", "+faststart", tmp]
    r = run(cmd, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0 or not os.path.exists(tmp) or os.path.getsize(tmp) < 1000:
        if os.path.exists(tmp):
            os.remove(tmp)
        raise RuntimeError("encode failed: " + (r.stderr or "")[-400:])
    os.replace(tmp, dst)


def measure(ff, path, hw):
    """Levels at 1 fps on 64x36 grey frames: overall stats + per-second mean curve."""
    cmd = [ff["ffmpeg"], "-hide_banner", "-loglevel", "error"] + (["-hwaccel", "cuda"] if hw else []) + [
        "-i", path, "-vf", "fps=1,scale=64:36:flags=area,format=gray", "-f", "rawvideo", "-"]
    r = run(cmd)
    raw = r.stdout
    n = 64 * 36
    frames = [raw[i:i + n] for i in range(0, len(raw) - n + 1, n)]
    if not frames:
        return None, []
    allp = sorted(b for f in frames for b in f)
    cnt = len(allp)
    q = lambda a: allp[min(cnt - 1, int(a * cnt))] / 255.0
    mean = sum(allp) / cnt / 255.0
    var = sum((b / 255.0 - mean) ** 2 for b in allp[::7]) / max(1, len(allp[::7]))
    curve = [round(sum(f) / n / 255.0, 4) for f in frames]
    stats = {"mean": round(mean, 4), "std": round(var ** 0.5, 4), "p02": round(q(0.02), 4),
             "p50": round(q(0.50), 4), "p98": round(q(0.98), 4),
             "dark": round(sum(1 for b in allp if b < 40) / cnt, 4)}
    return stats, curve


def thumbnail(ff, path, dst, curve, duration):
    if curve:
        med = sorted(curve)[len(curve) // 2]
        t = min(range(len(curve)), key=lambda i: abs(curve[i] - med)) + 0.5
    else:
        t = duration / 2
    run([ff["ffmpeg"], "-y", "-hide_banner", "-loglevel", "error", "-ss", f"{min(t, max(0, duration - 0.5)):.2f}",
         "-i", path, "-frames:v", "1", "-vf", "scale=320:180", "-q:v", "4", dst])


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 22), b""):
            h.update(chunk)
    return h.hexdigest()


def hash_all(paths, jobs):
    with cf.ThreadPoolExecutor(max(1, jobs)) as ex:
        return dict(zip(paths, ex.map(sha256, paths)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("--out", help="output folder (default SRC/_cd)")
    ap.add_argument("-j", "--jobs", type=int, default=3, help="parallel encodes (default 3)")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--encoder", choices=["auto", "h264_nvenc", "libx264"], default="auto")
    ap.add_argument("--limit", type=int, default=0, help="only the first N clips (testing)")
    ap.add_argument("--no-hash", action="store_true", help="skip the before/after originals hash check")
    a = ap.parse_args()

    ff = {k: shutil.which(k) for k in ("ffmpeg", "ffprobe")}
    if not all(ff.values()):
        sys.exit("ffmpeg/ffprobe not found on PATH")
    src = os.path.abspath(a.src)
    out = os.path.abspath(a.out or os.path.join(src, "_cd"))
    meta = os.path.join(out, ".clipdirector")
    clips = sorted(f for f in os.listdir(src)
                   if os.path.isfile(os.path.join(src, f)) and os.path.splitext(f)[1].lower() in VIDEO_EXT)
    if a.limit:
        clips = clips[:a.limit]

    encoder = a.encoder
    if encoder == "auto":
        t = run([ff["ffmpeg"], "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
                 "testsrc2=s=256x144:r=30", "-t", "0.2", "-c:v", "h264_nvenc", "-f", "null", "-"])
        encoder = "h264_nvenc" if t.returncode == 0 else "libx264"
    hw = encoder == "h264_nvenc"

    index_path = os.path.join(meta, "index.json")
    index = {"recipe": RECIPE, "recipe_hash": RECIPE_HASH, "clips": {}}
    if os.path.exists(index_path):
        old = json.load(open(index_path, encoding="utf-8"))
        if old.get("recipe_hash") == RECIPE_HASH:
            index["clips"] = old.get("clips", {})

    todo, skip = [], []
    for f in clips:
        st = os.stat(os.path.join(src, f))
        e = index["clips"].get(f)
        dst = os.path.join(out, os.path.splitext(f)[0] + ".mp4")
        if e and e.get("src_size") == st.st_size and e.get("src_mtime") == int(st.st_mtime) and os.path.exists(dst):
            skip.append(f)
        else:
            todo.append(f)
    total_src = sum(os.path.getsize(os.path.join(src, f)) for f in todo)
    log(f"src {src}\nout {out}\nencoder {encoder} (decode {'cuda' if hw else 'cpu'}), jobs {a.jobs}, "
        f"recipe {RECIPE_HASH}\n{len(clips)} clips: {len(todo)} to encode ({total_src / 1e9:.1f} GB source), "
        f"{len(skip)} already done")
    names = [os.path.splitext(f)[0] for f in clips]
    if len(set(n.lower() for n in names)) != len(names):
        sys.exit("two source files share a base name; outputs would collide")
    if a.dry_run:
        for f in todo[:10]:
            log(f"  would encode: {f}")
        if len(todo) > 10:
            log(f"  ... and {len(todo) - 10} more")
        free = shutil.disk_usage(os.path.dirname(out) if not os.path.exists(out) else out).free
        log(f"estimated output ~{total_src * 1.25 / 1e9:.1f} GB, free {free / 1e9:.0f} GB")
        return 0

    os.makedirs(os.path.join(meta, "thumbs"), exist_ok=True)
    originals = [os.path.join(src, f) for f in clips]
    before = {}
    if not a.no_hash:
        t0 = time.time()
        before = hash_all(originals, 4)
        with open(os.path.join(meta, "originals.sha256"), "w", encoding="utf-8") as fh:
            for p in originals:
                fh.write(f"{before[p]}  {os.path.basename(p)}\n")
        log(f"hashed {len(before)} originals in {time.time() - t0:.0f}s")

    failures = []
    done = [0]

    def work(f):
        s = os.path.join(src, f)
        base = os.path.splitext(f)[0]
        dst = os.path.join(out, base + ".mp4")
        t0 = time.time()
        info = probe(ff, s)
        encode(ff, s, dst, encoder)
        o = probe(ff, dst)
        kf = keyframe_interval(ff, dst)
        stats, curve = measure(ff, dst, hw)
        thumbnail(ff, dst, os.path.join(meta, "thumbs", base + ".jpg"), curve, o["duration"])
        st = os.stat(s)
        entry = {"file": base + ".mp4", "src_size": st.st_size, "src_mtime": int(st.st_mtime),
                 "src": {"w": info["w"], "h": info["h"], "fps": round(info["fps"], 3), "pix_fmt": info["pix_fmt"],
                         "duration": round(info["duration"], 3)},
                 "duration": round(o["duration"], 3), "fps": round(o["fps"], 3), "keyframe_s": round(kf, 4),
                 "w": o["w"], "h": o["h"], "pix_fmt": o["pix_fmt"], "size": os.path.getsize(dst),
                 "stats": stats, "curve_1hz": curve, "thumb": "thumbs/" + base + ".jpg"}
        with lock:
            index["clips"][f] = entry
            done[0] += 1
            tmp = index_path + ".tmp"
            json.dump(index, open(tmp, "w", encoding="utf-8"), indent=1, ensure_ascii=False)
            os.replace(tmp, index_path)
        log(f"[{done[0]}/{len(todo)}] {base[:60]}  {time.time() - t0:.1f}s  kf {kf:.3f}s  "
            f"{info['duration']:.1f}->{o['duration']:.1f}s  mean {stats['mean'] if stats else '?'}")

    t_all = time.time()
    with cf.ThreadPoolExecutor(max(1, a.jobs)) as ex:
        futs = {ex.submit(work, f): f for f in todo}
        for fu in cf.as_completed(futs):
            try:
                fu.result()
            except Exception as e:  # keep going; report at the end
                failures.append((futs[fu], str(e)))
                log(f"FAIL {futs[fu]}: {e}")
    log(f"encoded {len(todo) - len(failures)}/{len(todo)} in {(time.time() - t_all) / 60:.1f} min")

    # verification
    problems = []
    for f in clips:
        e = index["clips"].get(f)
        if not e:
            problems.append(f"{f}: no index entry")
            continue
        if abs(e["keyframe_s"] - GOP / FPS) > 0.02:
            problems.append(f"{f}: keyframe interval {e['keyframe_s']}")
        if (e["w"], e["h"]) != (W, H) or abs(e["fps"] - FPS) > 0.01:
            problems.append(f"{f}: {e['w']}x{e['h']}@{e['fps']}")
        if abs(e["duration"] - e["src"]["duration"]) > 0.2:
            problems.append(f"{f}: duration {e['src']['duration']} -> {e['duration']}")
    if before:
        after = hash_all(originals, 4)
        changed = [os.path.basename(p) for p in originals if after.get(p) != before.get(p)]
        log(f"originals unchanged: {len(originals) - len(changed)}/{len(originals)}")
        problems += [f"{c}: ORIGINAL CHANGED" for c in changed]
    for f, err in failures:
        problems.append(f"{f}: {err[:200]}")
    log("verification: " + ("OK" if not problems else f"{len(problems)} problem(s)"))
    for p in problems:
        log("  " + p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
