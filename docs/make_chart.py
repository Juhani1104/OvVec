"""Draw docs/speedup-light.svg and docs/speedup-dark.svg.

    python3 docs/make_chart.py

Needs matplotlib. The numbers are the medians in the README's Results tables
(stock ms, OvVec ms).
"""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.transforms import blended_transform_factory

ROWS = [  # group, label, stock ms, OvVec ms
    ("Read", "8 MB value, fragmented on disk", 127.7, 60.4),
    ("Read", "256 KB value, in memory", 0.017, 0.012),
    ("Delete", "half of the 256 KB rows", 20.3, 8.0),
    ("Delete", "half of the 1 MB rows", 18.6, 5.6),
    ("Rewrite", "20 × 256 KB, new size", 59.1, 36.6),
    ("Rewrite", "20 × 256 KB, same size", 54.3, 39.6),
]

# GitHub's light and dark palettes
THEMES = {
    "light": dict(text="#59636e", strong="#1f2328", grid="#d8dee4",
                  ref="#747d87", bar="#2272dc"),
    "dark": dict(text="#9198a1", strong="#f0f6fc", grid="#30363d",
                 ref="#848d97", bar="#4493f8"),
}


def draw(theme, path):
    c = THEMES[theme]
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10,
                         "svg.fonttype": "path"})
    fig, ax = plt.subplots(figsize=(7.6, 3.4))
    fig.patch.set_alpha(0)
    ax.patch.set_alpha(0)

    # one gap between groups
    ys, y, prev = [], 0.0, None
    for group, *_ in ROWS:
        if prev is not None and group != prev:
            y += 0.6
        ys.append(y)
        y += 1
        prev = group

    speedups = [s / o for _, _, s, o in ROWS]
    ax.barh(ys, speedups, height=0.6, color=c["bar"], zorder=2)
    for y, sp in zip(ys, speedups):
        ax.text(sp + 0.05, y, f"{sp:.1f}×", va="center", ha="left",
                color=c["bar"], fontweight="bold")

    ax.axvline(1, color=c["ref"], linestyle=(0, (4, 3)), linewidth=1, zorder=3)
    ax.text(1, -0.95, "stock SQLite", ha="center", va="bottom",
            color=c["text"], fontsize=9)

    ax.set_yticks(ys, [label for _, label, *_ in ROWS], color=c["text"])
    ax.invert_yaxis()
    # group names at the left edge of the figure, level with their first row
    left_edge = blended_transform_factory(fig.transFigure, ax.transData)
    seen = set()
    for (group, *_), y in zip(ROWS, ys):
        if group not in seen:
            seen.add(group)
            ax.text(0.01, y, group, transform=left_edge, ha="left",
                    va="center", color=c["strong"], fontweight="bold")

    ax.set_xlim(0, 3.75)
    ax.set_xticks([0, 1, 2, 3], ["0", "1×", "2×", "3×"], color=c["text"])
    ax.tick_params(length=0, pad=6)
    ax.grid(axis="x", color=c["grid"], zorder=0)
    for side in ax.spines.values():
        side.set_visible(False)
    fig.text(0.01, 0.95, "How many times faster than stock SQLite",
             ha="left", va="top", color=c["strong"], fontweight="bold",
             fontsize=11)

    fig.subplots_adjust(left=0.41, right=0.97, top=0.84, bottom=0.08)
    fig.savefig(path, format="svg", metadata={"Date": None})
    plt.close(fig)


if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    for theme in THEMES:
        draw(theme, os.path.join(here, f"speedup-{theme}.svg"))
