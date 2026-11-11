#!/usr/bin/env python3
"""Build report.html for the rowdb M1 report from the raw crash-test runs.

Inputs (raw data, never modified; runs/ is gitignored, so the numbers used are copied to data/):
  ../runs/r6-20261110-2229-*/summary.json, results.jsonl   final suite (22:29-22:34 JST)
  ../runs/m1-*-20261110-2020/summary.json                  first suite (20:20-20:22 JST)
  ../runs/r3-20261110-2119-steal*/, r4-20261110-2140-steal*/  earlier steal-control runs
  ../runs/*/summary.json                                    every suite, for the WAL-tail total

Outputs:
  data/aggregates.json   every number used in the report
  fig1.svg fig2.svg fig3.svg
  report.html            report_src.html with {{KEY}} placeholders and figures filled in
"""
import glob
import json
import pathlib
import re
import statistics

HERE = pathlib.Path(__file__).resolve().parent
PROJ = HERE.parent
RUNS = PROJ / "runs"
R6 = "r6-20261110-2229"

ACCENT = "#1f5f46"
RED = "#a33b2f"
GRAY = "#8a8f94"
LIGHT = "#e3e6e8"
INK = "#1a1a1a"
FONT = "Hiragino Sans, Hiragino Kaku Gothic ProN, sans-serif"

ORDER = ["none", "none-asan", "none-small", "none-small-asan", "nofsync", "nowal", "skipredo", "steal", "steal-small"]
LABEL = {
    "none": "正常版・32枠",
    "none-asan": "正常版・32枠・ASan+UBSan",
    "none-small": "正常版・8枠",
    "none-small-asan": "正常版・8枠・ASan+UBSan",
    "nofsync": "fsync抜き・32枠",
    "nowal": "WAL抜き・32枠",
    "skipredo": "復旧飛ばし・32枠",
    "steal": "コミット前書き込み・32枠",
    "steal-small": "コミット前書き込み・8枠",
}


def summary(path):
    return json.loads(pathlib.Path(path).read_text())


def results(path):
    return [json.loads(line) for line in pathlib.Path(path).read_text().splitlines() if line.strip()]


def aggregate():
    A = {"r6": {}, "m1": {}, "steal_history": {}}
    for name in ORDER:
        s = summary(RUNS / f"{R6}-{name}" / "summary.json")
        A["r6"][name] = {k: s.get(k) for k in (
            "bug", "frames", "checkpoint_wal_bytes", "cycles", "killed_by_sigkill", "exited_before_kill",
            "killed_before_load", "txn_phase_kills", "checks", "pass", "fail", "harness_errors", "stalled_cycles",
            "indoubt_seen", "indoubt_present", "indoubt_absent", "acked_txns", "wal_tail_seen", "pages_redone_total",
            "resets", "data.db_bytes", "verdict", "elapsed_s", "seed")}
    for name in ("none", "none-asan", "nowal", "skipredo"):
        s = summary(RUNS / f"m1-{name}-20261110-2020" / "summary.json")
        A["m1"][name] = {k: s.get(k) for k in ("cycles", "checks", "pass", "fail")}
    for suite in ("r3-20261110-2119", "r4-20261110-2140", R6):
        for name in ("steal", "steal-small"):
            s = summary(RUNS / f"{suite}-{name}" / "summary.json")
            A["steal_history"][f"{suite}/{name}"] = {k: s.get(k) for k in ("checks", "fail", "stalled_cycles")}
    # what each detected mismatch looked like (from results.jsonl diff records)
    A["fail_kinds"] = {}
    for name in ("nowal", "skipredo", "steal", "steal-small"):
        kinds = {"startup_error": 0, "ledger_conflict": 0, "extra_rows": 0, "acked_inserts_missing": 0,
                 "acked_txn_partially_present": 0, "other_rows_missing": 0}
        for r in results(RUNS / f"{R6}-{name}" / "results.jsonl"):
            if r.get("result") != "fail":
                continue
            d = r["diff"]
            if d.get("dump_error"):
                kinds["startup_error"] += 1
            elif d.get("ledger_conflict"):
                kinds["ledger_conflict"] += 1
            else:
                kinds["extra_rows"] += bool(d["extra_rows"])
                kinds["acked_inserts_missing"] += bool(d["acked_inserts_missing"])
                kinds["acked_txn_partially_present"] += bool(d.get("acked_txns_partially_present"))
                kinds["other_rows_missing"] += bool(d["missing_rows"]) and not d["acked_inserts_missing"]
        A["fail_kinds"][name] = kinds
    # every suite ever run today: total SIGKILLs and WAL-tail observations
    kills = tails = suites = 0
    for f in glob.glob(str(RUNS / "*" / "summary.json")):
        s = summary(f)
        kills += s.get("killed_by_sigkill", 0)
        tails += s.get("wal_tail_seen", 0)
        suites += 1
    A["all_runs"] = {"summary_files": suites, "sigkills": kills, "wal_tail_seen": tails}
    # per-cycle timing of the main correct-engine run
    rows = results(RUNS / f"{R6}-none" / "results.jsonl")
    A["timing_none"] = [{"cycle": r["cycle"], "kill_at_ms": r["kill_at_ms"], "loaded_ms": r.get("loaded_ms")} for r in rows]
    k = sorted(r["kill_at_ms"] for r in rows)
    A["kill_at_none"] = {"median": round(statistics.median(k), 1), "p90": k[int(0.9 * (len(k) - 1))], "max": k[-1], "min": k[0]}
    loaded = [r["loaded_ms"] for r in rows if r.get("loaded_ms") is not None]
    A["loaded_none"] = {"first10_max": max(r["loaded_ms"] for r in rows[:10] if r.get("loaded_ms") is not None),
                        "last50_median": round(statistics.median([r["loaded_ms"] for r in rows[-50:] if r.get("loaded_ms") is not None]), 1),
                        "max": max(loaded)}
    half = len(rows) // 2
    A["before_load_halves"] = [sum(1 for r in rows[:half] if r.get("loaded_ms") is None),
                               sum(1 for r in rows[half:] if r.get("loaded_ms") is None)]
    return A


def t(x, y, s, size=12, anchor="start", fill=INK, weight="normal", extra=""):
    return (f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" text-anchor="{anchor}" fill="{fill}" '
            f'font-weight="{weight}" {extra}>{s}</text>')


def fig1(A):
    """Two separately scaled panels: match rate for runs that must match, detection rate for broken controls."""
    W = 680
    groups = [
        ("正常版とfsync抜き：チェックのうち、台帳から計算した状態と一致した回数", ["none", "none-asan", "none-small", "none-small-asan", "nofsync"], "pass", ACCENT),
        ("わざと壊した版：チェックのうち、食い違いを検出した回数", ["nowal", "skipredo", "steal", "steal-small"], "fail", RED),
    ]
    x0, x1, bh, gap = 214, 600, 13, 6
    y = 8
    out = []
    for title, names, key, color in groups:
        y += 16
        out.append(t(4, y, title, 12.5, "start", INK, "bold"))
        y += 10
        top = y
        for name in names:
            r = A["r6"][name]
            frac = r[key] / r["checks"]
            out.append(t(x0 - 8, y + bh - 2.5, LABEL[name], 11.5, "end"))
            out.append(f'<rect x="{x0}" y="{y}" width="{x1 - x0}" height="{bh}" fill="{LIGHT}"/>')
            out.append(f'<rect x="{x0}" y="{y}" width="{(x1 - x0) * frac:.1f}" height="{bh}" fill="{color}"/>')
            out.append(t(x1 + 6, y + bh - 2.5, f'{r[key]}/{r["checks"]}', 11.5))
            y += bh + gap
        for p in (0, 50, 100):
            xx = x0 + (x1 - x0) * p / 100
            out.append(f'<line x1="{xx:.1f}" y1="{top - 2}" x2="{xx:.1f}" y2="{y - gap + 2}" stroke="#9aa0a5" stroke-width="0.6" stroke-dasharray="2,2"/>')
            out.append(t(xx, y + 8, f"{p}%", 10.5, "middle", "#555"))
        y += 22
    H = y
    return (f'<svg viewBox="0 0 {W} {H}" xmlns="http://www.w3.org/2000/svg" font-family="{FONT}" role="img">'
            + "".join(out) + "</svg>")


def box(x, y, w, h, lines, fill="#fff", stroke=INK, size=11.5, bold_first=False):
    out = [f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="3" fill="{fill}" stroke="{stroke}" stroke-width="0.9"/>']
    n = len(lines)
    lh = size + 4.5
    y0 = y + h / 2 - (n - 1) * lh / 2 + size * 0.36
    for i, s in enumerate(lines):
        out.append(t(x + w / 2, y0 + i * lh, s, size, "middle", INK, "bold" if (bold_first and i == 0) else "normal"))
    return "".join(out)


def arrow(x1, y1, x2, y2, color=INK, dash=""):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    return (f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{color}" stroke-width="1.1"{d} marker-end="url(#ah)"/>')


def fig2(A):
    """Commit path, checkpoint and recovery in the engine, and what the test writes around commit()."""
    W, H = 680, 292
    out = ['<defs><marker id="ah" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto">'
           f'<path d="M0,0 L10,5 L0,10 z" fill="{INK}"/></marker></defs>']
    out.append(t(4, 16, "エンジン（1度に1トランザクション）", 12.5, "start", INK, "bold"))
    bw, bh, y1 = 150, 54, 26
    xs = [4, 178, 352, 526]
    out.append(box(xs[0], y1, bw, bh, ["挿入・削除", "ページをBuffer Poolで変更"]))
    out.append(box(xs[1], y1, bw, bh, ["コミット前は", "データファイルに書かない", "（NO-STEAL）"]))
    out.append(box(xs[2], y1, bw, bh, ["WALに変更ページの写しと", "COMMIT記録を追記", "→ fsync"], "#eef4f1", ACCENT))
    out.append(box(xs[3], y1, bw, bh, ["コミット完了を返す", "（ACK）"]))
    for i in range(3):
        out.append(arrow(xs[i] + bw, y1 + bh / 2, xs[i + 1] - 3, y1 + bh / 2))
    y2 = 100
    out.append(box(xs[1], y2, 324, 44, ["チェックポイント（WALが512KBを超えたコミットで）", "ページをデータファイルへ書く → fsync → WALを空にする"], "#fff", GRAY))
    out.append(box(xs[1], y2 + 52, 324, 44, ["起動時の復旧", "WALにあるCOMMIT済みのページだけをデータファイルへ書き戻す"], "#fff", GRAY))
    out.append(t(xs[0] + 2, y2 + 27, "ACKを返す前に", 11, "start", "#555"))
    out.append(t(xs[0] + 2, y2 + 78, "kill -9 の後", 11, "start", "#555"))
    yt = 228
    out.append(f'<line x1="4" y1="{yt - 22}" x2="{W - 4}" y2="{yt - 22}" stroke="#c8cdd0" stroke-width="0.6"/>')
    out.append(t(4, yt - 6, "テスト側の記録（台帳、1行ごとにfsync）", 12.5, "start", INK, "bold"))
    out.append(box(xs[0], yt + 4, bw, 46, ["操作の内容と", "「S」（コミット開始）"]))
    out.append(box(xs[1], yt + 4, bw + 174, 46, ["commit() を呼ぶ（中でACKが返る）", "Cを書く前に殺されたものは「未確定」"], "#fff", GRAY))
    out.append(box(xs[3], yt + 4, bw, 46, ["「C」（ACKを受けた）"]))
    out.append(arrow(xs[0] + bw, yt + 27, xs[1] - 3, yt + 27))
    out.append(arrow(xs[1] + bw + 174, yt + 27, xs[3] - 3, yt + 27))
    return (f'<svg viewBox="0 0 {W} {H}" xmlns="http://www.w3.org/2000/svg" font-family="{FONT}" role="img">'
            + "".join(out) + "</svg>")


def fig3(A):
    """Per-cycle kill time vs end of the startup scan in the main correct-engine run."""
    W, H = 680, 238
    x0, x1, ytop, ybot = 52, 668, 28, 192
    ymax = 400
    rows = A["timing_none"]
    n = len(rows)
    sx = (x1 - x0) / (n - 1)
    sy = (ybot - ytop) / ymax
    out = []
    for v in range(0, ymax + 1, 100):
        yy = ybot - v * sy
        out.append(f'<line x1="{x0}" y1="{yy:.1f}" x2="{x1}" y2="{yy:.1f}" stroke="#d5d9dc" stroke-width="0.6"/>')
        out.append(t(x0 - 6, yy + 4, str(v), 10.5, "end", "#444"))
    for c in range(0, n, 50):
        xx = x0 + c * sx
        out.append(t(xx, ybot + 15, str(c), 10.5, "middle", "#444"))
    mb = A["r6"]["none"]["data.db_bytes"] / 2**20
    out.append(t((x0 + x1) / 2, ybot + 36, f"サイクル（同じDBを使い続ける。データファイルは最後に約{mb:.0f}MB）", 11.5, "middle"))
    out.append(t(14, (ytop + ybot) / 2, "起動からの時間（ms）", 11.5, "middle", INK, "normal",
                 f'transform="rotate(-90 14 {(ytop + ybot) / 2})"'))
    for r in rows:
        xx = x0 + r["cycle"] * sx
        before = r["loaded_ms"] is None
        if r["loaded_ms"] is not None:
            out.append(f'<circle cx="{xx:.1f}" cy="{ybot - r["loaded_ms"] * sy:.1f}" r="1.7" fill="{ACCENT}"/>')
        k = min(r["kill_at_ms"], ymax)
        if before:
            out.append(f'<path d="M{xx - 2.6:.1f},{ybot - k * sy - 2.6:.1f} l5.2,5.2 m0,-5.2 l-5.2,5.2" stroke="{RED}" stroke-width="1.2"/>')
        else:
            out.append(f'<circle cx="{xx:.1f}" cy="{ybot - k * sy:.1f}" r="1.6" fill="none" stroke="{GRAY}" stroke-width="0.8"/>')
    lx = x0 + 6
    out.append(f'<circle cx="{lx}" cy="14" r="2.2" fill="{ACCENT}"/>')
    out.append(t(lx + 7, 18, "起動時の全行読み込みが終わった時刻", 11))
    out.append(f'<circle cx="{lx + 232}" cy="14" r="2.2" fill="none" stroke="{GRAY}" stroke-width="0.9"/>')
    out.append(t(lx + 239, 18, "kill（読み込み後）", 11))
    out.append(f'<path d="M{lx + 357:.1f},11.4 l5.2,5.2 m0,-5.2 l-5.2,5.2" stroke="{RED}" stroke-width="1.2"/>')
    out.append(t(lx + 368, 18, "kill（読み込みが終わる前）", 11))
    return (f'<svg viewBox="0 0 {W} {H}" xmlns="http://www.w3.org/2000/svg" font-family="{FONT}" role="img">'
            + "".join(out) + "</svg>")


def fmt(n):
    return f"{n:,}"


def values(A):
    r6 = A["r6"]
    V = {}
    must = ["none", "none-asan", "none-small", "none-small-asan"]
    V["CORRECT_CYCLES"] = fmt(sum(r6[n]["cycles"] for n in must))
    V["CORRECT_CHECKS"] = fmt(sum(r6[n]["checks"] for n in must))
    V["CORRECT_PASS"] = fmt(sum(r6[n]["pass"] for n in must))
    V["CORRECT_INDOUBT"] = fmt(sum(r6[n]["indoubt_seen"] for n in must))
    V["CORRECT_ACKED"] = fmt(sum(r6[n]["acked_txns"] for n in must))
    V["R6_KILLS"] = fmt(sum(r6[n]["killed_by_sigkill"] for n in ORDER))
    V["R6_CYCLES"] = fmt(sum(r6[n]["cycles"] for n in ORDER))
    V["R6_EXITED"] = str(sum(r6[n]["exited_before_kill"] for n in ORDER))
    for n in ORDER:
        k = n.replace("-", "_").upper()
        r = r6[n]
        for f in ("cycles", "checks", "pass", "fail", "indoubt_seen", "indoubt_present", "indoubt_absent",
                  "acked_txns", "killed_before_load", "txn_phase_kills"):
            V[f"{k}_{f.upper()}"] = fmt(r[f])
    V["NONE_DB_MB"] = f'{r6["none"]["data.db_bytes"] / 2**20:.0f}'  # binary MB, like 512KB = 524,288 bytes
    V["NONE_DB_BYTES"] = fmt(r6["none"]["data.db_bytes"])
    V["KILL_MEDIAN"] = f'{A["kill_at_none"]["median"]:.0f}'
    V["KILL_P90"] = f'{A["kill_at_none"]["p90"]:.0f}'
    V["KILL_MAX"] = f'{A["kill_at_none"]["max"]:.0f}'
    V["LOADED_FIRST10_MAX"] = f'{A["loaded_none"]["first10_max"]:.1f}'
    V["LOADED_LAST50_MEDIAN"] = f'{A["loaded_none"]["last50_median"]:.0f}'
    V["LOADED_MAX"] = f'{A["loaded_none"]["max"]:.0f}'
    V["BEFORE_LOAD_FIRST_HALF"] = str(A["before_load_halves"][0])
    V["BEFORE_LOAD_SECOND_HALF"] = str(A["before_load_halves"][1])
    for n in ("steal", "steal-small"):
        k = n.replace("-", "_").upper()
        for f, v in A["fail_kinds"][n].items():
            V[f"FK_{k}_{f.upper()}"] = str(v)
    V["NOFSYNC_STALLED"] = str(A["r6"]["nofsync"]["stalled_cycles"])
    V["ALL_SIGKILLS"] = fmt(A["all_runs"]["sigkills"])
    V["ALL_WAL_TAIL"] = str(A["all_runs"]["wal_tail_seen"])
    m1 = A["m1"]
    for n in ("none", "none-asan", "nowal", "skipredo"):
        k = "M1_" + n.replace("-", "_").upper()
        V[f"{k}_CHECKS"] = str(m1[n]["checks"])
        V[f"{k}_PASS"] = str(m1[n]["pass"])
        V[f"{k}_FAIL"] = str(m1[n]["fail"])
    sh = A["steal_history"]
    for suite, tag in (("r3-20261110-2119", "R3"), ("r4-20261110-2140", "R4"), (R6, "R6")):
        for n, nt in (("steal", "S32"), ("steal-small", "S8")):
            s = sh[f"{suite}/{n}"]
            V[f"{tag}_{nt}"] = f'{s["fail"]}/{s["checks"]}'
    return V


def table_r6(A):
    r6 = A["r6"]
    head = ("<thead><tr><th style='width:27%'>実行</th><th class='r' style='width:9%'>サイクル</th>"
            "<th class='r' style='width:9%'>チェック</th><th class='r' style='width:12%'>一致／検出</th>"
            "<th class='r' style='width:17%'>コミット中のkill<br>（反映・未反映）</th><th class='r' style='width:12%'>ACK済み</th>"
            "<th class='r' style='width:14%'>読み込み前<br>のkill</th></tr></thead>")
    body = []
    for n in ORDER:
        r = r6[n]
        res = (f'一致 {r["pass"]}' if n in ("none", "none-asan", "none-small", "none-small-asan", "nofsync")
               else f'検出 {r["fail"]}')
        body.append(f'<tr><td>{LABEL[n]}</td><td class="num r">{fmt(r["cycles"])}</td><td class="num r">{r["checks"]}</td>'
                    f'<td class="num r">{res}</td><td class="num r">{r["indoubt_seen"]}（{r["indoubt_present"]}・{r["indoubt_absent"]}）</td>'
                    f'<td class="num r">{fmt(r["acked_txns"])}</td><td class="num r">{r["killed_before_load"]}</td></tr>')
    return f"<table class='compact keep'>{head}<tbody>{''.join(body)}</tbody></table>"


def main():
    A = aggregate()
    (HERE / "data").mkdir(exist_ok=True)
    (HERE / "data" / "aggregates.json").write_text(json.dumps(A, ensure_ascii=False, indent=1))
    figs = {"FIG1": fig1(A), "FIG2": fig2(A), "FIG3": fig3(A), "TABLE_R6": table_r6(A)}
    for i in (1, 2, 3):
        (HERE / f"fig{i}.svg").write_text(figs[f"FIG{i}"])
    V = values(A)
    V.update(figs)
    src = (HERE / "report_src.html").read_text()
    out = re.sub(r"\{\{([A-Z0-9_]+)\}\}", lambda m: V[m.group(1)], src)
    left = re.findall(r"\{\{[^}]*\}\}", out)
    assert not left, left
    (HERE / "report.html").write_text(out)
    print(json.dumps({k: v for k, v in V.items() if not k.startswith(("FIG", "TABLE"))}, ensure_ascii=False, indent=1))


if __name__ == "__main__":
    main()
