#!/usr/bin/env python3
"""
GitHub Actions IG/FB Reel Audio + 35s-Chunked Frame-by-Frame Chart Vision Extractor.
- Zero local/VM video retention (deletes raw video after extraction).
- Uses yt-dlp (reels) + gallery-dl (/p/ image carousels) + faster-whisper + ffmpeg + Gemini Flash Vision.
- Syncs extracted transcript, chunked visual observations, strategy_spec.md/.json, and C++ plugin draft to GDrive.
"""
import argparse, base64, glob, json, math, os, random, re, shutil, subprocess, sys, time, urllib.error, urllib.request
from pathlib import Path

QUANT_CHUNK_SYSTEM_PROMPT = """You are an institutional CME Futures Quantitative Strategist & Chart Vision Analyst.
You are inspecting a 35-second window of a trading strategy video/reel (sequential 1fps/keyframe chart screenshots + exact timestamped audio transcript for this window).

Your job is to extract EVERY visual and spoken trading rule with ZERO hand-waving:
1. **Chart & Context:** Exact ticker/asset shown (NQ, ES, YM, SPY, etc.), chart timeframe(s) (1m/5m/15m/1H/4H/Daily), session/killzone time-of-day (e.g. 09:30 ET, 10:00 macro, London, Asian), and visible indicators + settings (EMA lengths, VWAP, ATR, RSI, FVG/OB boxes, session H/L lines).
2. **Cursor & Candle Grounding:** Whenever the speaker says "here", "this candle", "this sweep", "that gap", "below this low", identify the EXACT visual candle/wick/FVG/IFVG/Order Block/liquidity level/trendline drawn or pointed to in the corresponding timestamped frame(s).
3. **Causal Entry Mechanics:** Exact trigger (market close above/below level vs touch/stop vs pullback retest), displacement/candle-body requirements, HTF->LTF alignment, and filters.
4. **Stop, Target & Risk Rules:** Exact SL anchor (swing H/L, FVG edge, ATR, fixed pts/ticks), TP target (R:R multiple, opposing liquidity, trailing ratchet/BE), max trades/day, cooldown, session flatten.
5. **Ambiguity Audit:** Explicitly list anything implied visually on the chart that wasn't spoken aloud, and anything still ambiguous/missing.

Be concise, technical, and 100% faithful to what is visibly on the chart and spoken in the transcript."""

SYNTHESIS_PROMPT_TMPL = """Synthesize all chunk visual analyses, the full timestamped transcript, the post caption, and the user's special instructions into ONE complete, unambiguous quantitative strategy specification.

Reel Index: {idx} | URL: {url} | Kind: {kind}
User Special Note / Test Instruction: {user_note}

=== POST CAPTION / METADATA ===
{caption}

=== FULL TIMESTAMPED TRANSCRIPT ===
{transcript}

=== 35s CHUNKED FRAME-BY-FRAME VISUAL OBSERVATIONS ===
{chunks_text}

Return a JSON object (valid JSON only, no markdown fences) with these exact keys:
{{
  "strategy_name": "short_snake_case_name",
  "title": "Human readable title",
  "creator": "creator handle if visible",
  "is_standalone_strategy": true,
  "is_filter_overlay": false,
  "sub_strategies_count": 1,
  "asset_focus": "NQ/ES/YM/Multi",
  "timeframes": ["1m", "5m"],
  "sessions_et": "e.g. NY_AM 09:30-11:30 ET",
  "indicators_and_settings": ["..."],
  "entry_rules_exact": ["..."],
  "filter_rules_exact": ["..."],
  "stop_loss_rules": "...",
  "take_profit_and_trailing_rules": "...",
  "trade_management": "max trades/day, cooldown, session flatten",
  "user_note_plan": "How to implement/test the user's specific note above",
  "ambiguities_or_missing_details": ["..."],
  "tunable_parameters": [
    {{"name": "param_name", "default": 1.0, "min": 0.5, "max": 3.0, "scale": "LINEAR"}}
  ],
  "cpp_logic_pseudocode": "Clear causal bar-by-bar C++ logic using CausalBarView (lag <= 0 only, no lookahead)"
}}"""


def run_cmd(cmd, timeout=180):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


def extract_code(url):
    m = re.search(r"/(?:reel|p|r)/([A-Za-z0-9_-]+)", url)
    return m.group(1) if m else f"item_{abs(hash(url)) % 100000}"


def download_media(item, workdir: Path, cookies_path: Path):
    url = item["url"]
    kind = item["kind"]
    meta = {"url": url, "kind": kind, "caption": "", "uploader": ""}
    video_path = None
    image_paths = []

    # Jittered delay to avoid IG rate limits
    time.sleep(random.uniform(3.5, 7.5))

    if kind == "ig_post":
        # Try gallery-dl first for image carousels, fallback to yt-dlp if video post
        cmd = ["gallery-dl", "--cookies", str(cookies_path), "-D", str(workdir / "dl"), url]
        r = run_cmd(cmd, timeout=120)
        if r.returncode == 0:
            imgs = sorted(glob.glob(str(workdir / "dl" / "*.jpg")) + glob.glob(str(workdir / "dl" / "*.png")) + glob.glob(str(workdir / "dl" / "*.webp")))
            vids = sorted(glob.glob(str(workdir / "dl" / "*.mp4")))
            if vids:
                video_path = Path(vids[0])
            elif imgs:
                image_paths = [Path(p) for p in imgs]

    if video_path is None and not image_paths:
        out_tmpl = str(workdir / "video.%(ext)s")
        for attempt in range(3):
            cmd = [
                "yt-dlp", "--cookies", str(cookies_path),
                "-f", "bv*[height<=1080]+ba/b[height<=1080]/b",
                "--merge-output-format", "mp4",
                "--no-playlist", "--write-info-json",
                "-o", out_tmpl, url
            ]
            r = run_cmd(cmd, timeout=180)
            vids = sorted(glob.glob(str(workdir / "video*.mp4")) + glob.glob(str(workdir / "video*.mkv")) + glob.glob(str(workdir / "video*.webm")))
            if vids:
                video_path = Path(vids[0])
                break
            err = r.stderr or ""
            if "429" in err or "rate" in err.lower() or "wait a few minutes" in err.lower():
                time.sleep(15 * (attempt + 1))
            else:
                break

    info_jsons = glob.glob(str(workdir / "*.info.json"))
    if info_jsons:
        try:
            d = json.load(open(info_jsons[0]))
            meta["caption"] = d.get("description") or d.get("title") or ""
            meta["uploader"] = d.get("uploader") or d.get("channel") or ""
            meta["duration"] = d.get("duration") or 0
        except Exception:
            pass
    if not meta["caption"]:
        try:
            req = urllib.request.Request("https://r.jina.ai/" + url, headers={"User-Agent": "Mozilla/5.0"})
            meta["caption"] = urllib.request.urlopen(req, timeout=25).read().decode("utf8", "ignore")[:3000]
        except Exception:
            pass
    return video_path, image_paths, meta


def transcribe_audio(video_path: Path, workdir: Path):
    wav = workdir / "audio.wav"
    r = run_cmd(["ffmpeg", "-y", "-i", str(video_path), "-vn", "-ac", "1", "-ar", "16000", "-f", "wav", str(wav)], timeout=120)
    if r.returncode != 0 or not wav.exists():
        return [], ""
    from faster_whisper import WhisperModel
    model = WhisperModel("small.en", device="cpu", compute_type="int8", cpu_threads=4)
    segments, _ = model.transcribe(str(wav), beam_size=3, vad_filter=True)
    seg_list = []
    lines = []
    for s in segments:
        t0, t1, txt = float(s.start), float(s.end), s.text.strip()
        if txt:
            seg_list.append({"start": t0, "end": t1, "text": txt})
            lines.append(f"[{t0:05.1f}s - {t1:05.1f}s] {txt}")
    return seg_list, "\n".join(lines)


def extract_frames_dedup(video_path: Path, workdir: Path):
    fdir = workdir / "frames"
    fdir.mkdir(parents=True, exist_ok=True)
    # Extract 1 fps sharp frames (max width/height 1280)
    r = run_cmd([
        "ffmpeg", "-y", "-i", str(video_path),
        "-vf", "fps=1,scale='min(1280,iw)':-2",
        "-q:v", "3", str(fdir / "f_%04d.jpg")
    ], timeout=120)
    raw_frames = sorted(fdir.glob("f_*.jpg"))
    if not raw_frames:
        return []

    # Deduplicate near-identical static frames while always keeping >= 1 frame every 3s
    from PIL import Image
    import numpy as np

    kept = []
    prev_arr = None
    last_kept_t = -99
    for idx, fp in enumerate(raw_frames):
        t_sec = idx  # 1 fps -> f_0001 = 0s, f_0002 = 1s, ...
        im = Image.open(fp).convert("L").resize((96, 96))
        arr = np.asarray(im, dtype=np.float32)
        diff = 999.0 if prev_arr is None else float(np.mean(np.abs(arr - prev_arr)))
        if prev_arr is None or diff >= 3.0 or (t_sec - last_kept_t) >= 3:
            kept.append((t_sec, fp))
            prev_arr = arr
            last_kept_t = t_sec
    return kept


def call_gemini(api_key: str, parts: list, temperature: float = 0.1, json_mode: bool = False):
    models = ["gemini-2.5-flash", "gemini-3.8-flash", "gemini-2.5-flash-lite"]
    gen_cfg = {"temperature": temperature, "maxOutputTokens": 8192}
    if json_mode:
        gen_cfg["responseMimeType"] = "application/json"
    payload = json.dumps({
        "contents": [{"parts": parts}],
        "generationConfig": gen_cfg
    }).encode("utf-8")

    last_err = ""
    for m in models:
        url = f"https://generativelanguage.googleapis.com/v1beta/models/{m}:generateContent?key={api_key}"
        for attempt in range(5):
            req = urllib.request.Request(url, data=payload, headers={"Content-Type": "application/json"}, method="POST")
            try:
                with urllib.request.urlopen(req, timeout=120) as resp:
                    data = json.loads(resp.read().decode("utf-8"))
                    return data["candidates"][0]["content"]["parts"][0]["text"]
            except urllib.error.HTTPError as e:
                body = e.read().decode("utf-8", "ignore")
                last_err = f"{m} HTTP {e.code}: {body[:250]}"
                if e.code in (429, 500, 503):
                    time.sleep(10 * (attempt + 1) + random.uniform(1, 5))
                    continue
                break
            except Exception as e:
                last_err = f"{m} ERR: {e}"
                time.sleep(6)
    raise RuntimeError(f"Gemini API failed: {last_err}")


def img_part(fp: Path):
    b64 = base64.b64encode(fp.read_bytes()).decode("ascii")
    mime = "image/png" if fp.suffix.lower() == ".png" else "image/jpeg"
    return {"inline_data": {"mime_type": mime, "data": b64}}


def process_one(item, out_root: Path, cookies_path: Path, api_key: str):
    code = extract_code(item["url"])
    tag = f"{item['idx']:03d}_{code}"
    item_out = out_root / tag
    spec_file = item_out / "strategy_spec.json"
    if spec_file.exists():
        try:
            if not json.loads(spec_file.read_text()).get("parse_error"):
                print(f"[{tag}] SKIP (already extracted)", flush=True)
                return True
        except Exception:
            pass
    failed_marker = item_out / "FAILED.txt"
    if failed_marker.exists():
        failed_marker.unlink()

    work = Path(f"/tmp/work_{tag}")
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True, exist_ok=True)
    item_out.mkdir(parents=True, exist_ok=True)

    try:
        chunks_path = item_out / "chunks_vision.json"
        if chunks_path.exists() and (item_out / "transcript.txt").exists() and (item_out / "meta.json").exists():
            chunks_obs = json.loads(chunks_path.read_text())
            transcript_txt = (item_out / "transcript.txt").read_text().strip()
            meta = json.loads((item_out / "meta.json").read_text())
            frames = []
        else:
            video_path, image_paths, meta = download_media(item, work, cookies_path)
            if video_path is None and not image_paths:
                (item_out / "FAILED.txt").write_text(f"Download failed for {item['url']}\n")
                print(f"[{tag}] FAILED download", flush=True)
                return False

            segs, transcript_txt = ([], "")
            frames = []
            if video_path is not None:
                segs, transcript_txt = transcribe_audio(video_path, work)
                frames = extract_frames_dedup(video_path, work)
            else:
                frames = [(i * 5, p) for i, p in enumerate(image_paths)]
                transcript_txt = "(Image carousel post - visual slides only)"

            (item_out / "transcript.txt").write_text(transcript_txt + "\n")
            (item_out / "meta.json").write_text(json.dumps({**meta, "user_note": item.get("note", "")}, indent=2))

            # Chunk frames every 35s with 5s overlap (<= 16 frames per Vision call)
            max_t = frames[-1][0] if frames else 0
            win = 35
            step = 30
            chunks_obs = []
            t0 = 0
            chunk_idx = 0
            while t0 <= max_t or chunk_idx == 0:
                t1 = t0 + win
                c_frames = [(t, fp) for (t, fp) in frames if t0 <= t <= t1]
                if len(c_frames) > 16:
                    stride = len(c_frames) / 16.0
                    c_frames = [c_frames[int(i * stride)] for i in range(16)]
                c_segs = [f"[{s['start']:05.1f}s - {s['end']:05.1f}s] {s['text']}"
                          for s in segs if s["end"] >= max(0, t0 - 2) and s["start"] <= t1 + 2]
                seg_txt = "\n".join(c_segs) if c_segs else "(No speech in this window)"

                parts = [{"text": QUANT_CHUNK_SYSTEM_PROMPT + f"\n\n=== WINDOW {chunk_idx+1}: {t0}s to {t1}s ===\nTranscript slice:\n{seg_txt}\n\nUser note for this reel: {item.get('note', 'None')}\nFrames attached below in chronological order:"}]
                for t, fp in c_frames:
                    parts.append({"text": f"[Frame @ t={t}s]"})
                    parts.append(img_part(fp))

                obs = call_gemini(api_key, parts, temperature=0.1)
                chunks_obs.append({"chunk": chunk_idx + 1, "t0": t0, "t1": t1, "n_frames": len(c_frames), "observation": obs})
                time.sleep(2.0)
                chunk_idx += 1
                if t1 >= max_t:
                    break
                t0 += step

            chunks_path.write_text(json.dumps(chunks_obs, indent=2))

        # Final Synthesis Pass -> strict JSON + Markdown spec
        chunks_joined = "\n\n".join([f"--- CHUNK {c['chunk']} ({c['t0']}s..{c['t1']}s) ---\n{c['observation']}" for c in chunks_obs])
        synth_prompt = SYNTHESIS_PROMPT_TMPL.format(
            idx=item["idx"], url=item["url"], kind=item["kind"],
            user_note=item.get("note") or "None",
            caption=(meta.get("caption") or "")[:2500],
            transcript=transcript_txt or "(No speech)",
            chunks_text=chunks_joined
        )
        raw_json = call_gemini(api_key, [{"text": synth_prompt}], temperature=0.05, json_mode=True).strip()
        raw_json = re.sub(r"^```json\s*|\s*```$", "", raw_json, flags=re.MULTILINE).strip()
        try:
            spec = json.loads(raw_json)
        except Exception:
            spec = {"raw_synthesis": raw_json, "parse_error": True}

        (item_out / "strategy_spec.json").write_text(json.dumps(spec, indent=2))
        md = [
            f"# {spec.get('title', tag)}",
            f"- **URL:** {item['url']} (`{item['kind']}`)",
            f"- **User Note:** {item.get('note') or 'None'}",
            f"- **Asset / Timeframes / Sessions:** {spec.get('asset_focus', '')} | {spec.get('timeframes', '')} | {spec.get('sessions_et', '')}",
            f"- **Indicators:** {spec.get('indicators_and_settings', [])}",
            "\n## Entry Rules\n" + "\n".join(f"- {x}" for x in spec.get("entry_rules_exact", [])),
            "\n## Filter Rules\n" + "\n".join(f"- {x}" for x in spec.get("filter_rules_exact", [])),
            f"\n## Stop Loss\n{spec.get('stop_loss_rules', '')}",
            f"\n## Take Profit & Trailing\n{spec.get('take_profit_and_trailing_rules', '')}",
            f"\n## Trade Management\n{spec.get('trade_management', '')}",
            f"\n## User Note Execution Plan\n{spec.get('user_note_plan', '')}",
            "\n## Ambiguities / Missing Details\n" + "\n".join(f"- {x}" for x in spec.get("ambiguities_or_missing_details", [])),
            f"\n## Causal C++ Logic Pseudocode\n```cpp\n{spec.get('cpp_logic_pseudocode', '')}\n```",
            f"\n## Full Transcript\n```text\n{transcript_txt}\n```",
        ]
        (item_out / "strategy_spec.md").write_text("\n".join(md) + "\n")
        print(f"[{tag}] OK ({len(chunks_obs)} chunks, {len(frames)} frames)", flush=True)
        return True
    except Exception as e:
        (item_out / "FAILED.txt").write_text(f"Exception: {e}\n")
        print(f"[{tag}] ERROR: {e}", flush=True)
        return False
    finally:
        if work.exists():
            shutil.rmtree(work, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--queue", required=True)
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--cookies", required=True)
    ap.add_argument("--shard", type=int, default=0)
    ap.add_argument("--num-shards", type=int, default=20)
    args = ap.parse_args()

    api_key = os.environ.get("GEMINI_API_KEY", "").strip()
    assert api_key, "GEMINI_API_KEY env var missing"
    items = json.load(open(args.queue))
    my_items = [it for i, it in enumerate(items) if (i % args.num_shards) == args.shard]
    print(f"Shard {args.shard}/{args.num_shards}: processing {len(my_items)} / {len(items)} URLs", flush=True)

    out_root = Path(args.out_dir)
    out_root.mkdir(parents=True, exist_ok=True)
    ok_cnt = 0
    for it in my_items:
        if process_one(it, out_root, Path(args.cookies), api_key):
            ok_cnt += 1
    print(f"Shard {args.shard} DONE: {ok_cnt}/{len(my_items)} succeeded", flush=True)


if __name__ == "__main__":
    main()
