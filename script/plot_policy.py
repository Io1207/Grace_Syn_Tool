#!/usr/bin/env python3
"""
plot_policy.py — Visualise adaptive-strategy play from the CSV written by --csv.

Usage:
    python3 plot_policy.py <play.csv> [--out fig.png] [--show] [--window 50] [--ainf "0|1"] [--threshold 0.99] [--no-heatmap]
"""

import sys
import argparse
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt

def parse_idx(s):
    if pd.isna(s) or str(s) in ("", "empty"):
        return []
    return [int(x) for x in str(s).split("|") if x]

def load_csv(path):
    df = pd.read_csv(path, dtype={"b_sub": str, "a_sub": str, "g_sub": str})
    df["g_list"] = df["g_sub"].apply(parse_idx)
    return df

def panel_cumulative(ax, df, g_cols):
    for j, col in enumerate(g_cols):
        ax.plot(df["step"], df[col].cumsum(), label=f"G{j}", linewidth=1.5)
    ax.set_xlabel("Step"); ax.set_ylabel("Cumulative visits")
    ax.set_title("Guarantee Büchi set visits (cumulative)")
    ax.legend(fontsize=8); ax.grid(alpha=0.3)

def panel_rolling(ax, df, g_cols, window):
    for j, col in enumerate(g_cols):
        ax.plot(df["step"], df[col].rolling(window, min_periods=1).mean(),
                label=f"G{j}", linewidth=1.5)
    ax.set_xlabel("Step"); ax.set_ylabel(f"Visit rate (window={window})")
    ax.set_title("Rolling guarantee visit rate")
    ax.legend(fontsize=8); ax.grid(alpha=0.3)

def panel_heatmap(ax, df, n_g):
    eve = df[df["player"] == "E"]
    data = np.zeros((n_g, len(eve)))
    for t, g_list in enumerate(eve["g_list"]):
        for j in g_list:
            if 0 <= j < n_g:
                data[j, t] = 1
    ax.imshow(data, aspect="auto", interpolation="none", cmap="Blues",
              vmin=0, vmax=1, origin="lower")
    ax.set_yticks(range(n_g))
    ax.set_yticklabels([f"G{j}" for j in range(n_g)], fontsize=8)
    ax.set_xlabel("Eve step")
    ax.set_title("Guarantees targeted by the deployed strategy")

def panel_monitors(ax, df, w_cols):
    ax.plot(df["step"], df["w_eta"], label="w_η (union)", linewidth=1.5,
            linestyle="--", color="black")
    for i, col in enumerate(w_cols):
        ax.plot(df["step"], df[col], label=f"w_A{i}", linewidth=1.2)
    ax.set_xlabel("Step"); ax.set_ylabel("Monitor score")
    ax.set_title("Liveness monitor scores")
    ax.legend(fontsize=8); ax.grid(alpha=0.3); ax.set_ylim(-0.05, 1.05)

def panel_probs(ax, df, p_cols, ainf_col):
    for col in p_cols:
        lw = 2.4 if col == ainf_col else 1.0
        ax.plot(df["step"], df[col], label="B={" + col[2:].replace("|", ",") + "}",
                linewidth=lw)
    ax.set_xlabel("Step"); ax.set_ylabel("p_B")
    ax.set_title("Sampling probability per assumption set")
    ax.legend(fontsize=8, ncol=2); ax.grid(alpha=0.3); ax.set_ylim(-0.05, 1.05)

def ainf_column(spec):
    if spec is None:
        return None
    s = spec.replace(",", "|").strip()
    if s in ("", "empty"):
        return "p_empty"
    return "p_" + "|".join(str(i) for i in sorted(int(x) for x in s.split("|") if x))

def convergence_step(df, col, thr):
    above = (df[col] > thr).to_numpy()
    if len(above) == 0 or not above[-1]:
        return None
    below = np.where(~above)[0]
    first = 0 if len(below) == 0 else below[-1] + 1
    return int(df["step"].iloc[first])

def main():
    ap = argparse.ArgumentParser(description="Plot adaptive-strategy play CSV")
    ap.add_argument("csv")
    ap.add_argument("--out", default=None)
    ap.add_argument("--show", action="store_true")
    ap.add_argument("--window", type=int, default=50)
    ap.add_argument("--no-heatmap", action="store_true")
    ap.add_argument("--ainf", default=None, help='true A_inf, e.g. "0|1" or "empty"')
    ap.add_argument("--threshold", type=float, default=0.99)
    args = ap.parse_args()

    df = load_csv(args.csv)
    if df.empty:
        print("CSV is empty.", file=sys.stderr); sys.exit(1)

    g_cols = [c for c in df.columns if c.startswith("in_G")]
    w_cols = [c for c in df.columns if c.startswith("w_A")]
    p_cols = [c for c in df.columns if c.startswith("p_")]
    ainf_col = ainf_column(args.ainf)
    if ainf_col is not None and ainf_col not in df.columns:
        print(f"--ainf column {ainf_col} not in CSV", file=sys.stderr); sys.exit(1)

    if ainf_col is not None:
        step = convergence_step(df, ainf_col, args.threshold)
        if step is None:
            print(f"[rq2] {ainf_col} does not end above {args.threshold} — not converged")
        else:
            print(f"[rq2] converged at step {step} ({ainf_col} > {args.threshold} thereafter)")

    panels = [lambda ax: panel_cumulative(ax, df, g_cols),
              lambda ax: panel_rolling(ax, df, g_cols, args.window)]
    if not args.no_heatmap:
        panels.append(lambda ax: panel_heatmap(ax, df, len(g_cols)))
    panels += [lambda ax: panel_monitors(ax, df, w_cols),
               lambda ax: panel_probs(ax, df, p_cols, ainf_col)]

    fig, axes = plt.subplots(len(panels), 1, figsize=(12, 3.5 * len(panels)))
    fig.suptitle(f"Adaptive strategy — {args.csv}", fontsize=11, y=0.995)
    for ax, draw in zip(axes, panels):
        draw(ax)
    plt.tight_layout(rect=[0, 0, 1, 0.98])

    if args.out:
        plt.savefig(args.out, dpi=150, bbox_inches="tight")
        print(f"Saved: {args.out}")
    if args.show or not args.out:
        plt.show()


if __name__ == "__main__":
    main()