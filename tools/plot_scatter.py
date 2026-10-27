#!/usr/bin/env python3
"""
Сравнение алгоритмов по времени: одна панель на алгоритм, все панели в одну строку
с общей осью Y. По X — графы, цвет/форма — source_size (1, 10, 100).

Обработка данных:
  1. time_s суммируется по (algo_name, cnf, graph, source_size, chunk) — это защита от
     дублей строк; если строка на чанк одна, значение не меняется;
  2. ТОЧКА НА ГРАФИКЕ = ОДИН ЧАНК. Так виден разброс и скопления значений;
  3. грамматика (cnf) не показывается; если их несколько, точки разных грамматик
     попадают в одно облако;
  4. для каждой группы (algo, graph, source_size) считаются среднее (цветная толстая
     черта) и медиана (тонкая чёрная черта поверх неё).
  Флаг --sum-chunks вместо этого суммирует все чанки (одна точка на группу).

Примеры:
  python plot_scatter_v2.py results.csv
  python plot_scatter_v2.py results.csv -o diagrams/scatter --algos CFL_CFPQ_RSM CFL_CFPQ_RSM_blocked
  python plot_scatter_v2.py results.csv --sort graph --cols 2
"""
import argparse
import os

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from matplotlib.lines import Line2D

# Цвета из вашего старого скрипта; формы маркеров дублируют цвет (читается в ч/б и при дальтонизме)
STYLE = {
    1:   dict(color="#FF6B7A", marker="o"),
    10:  dict(color="#8ED345", marker="s"),
    100: dict(color="#4DA8DA", marker="^"),
}
FALLBACK = ["#E69F00", "#9467bd", "#8c564b"]


def parse_args():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("csv")
    p.add_argument("-o", "--output", default="scatter_plot", help="префикс выходных файлов (.svg и .png)")
    p.add_argument("--algos", nargs="+", help="какие алгоритмы показывать и в каком порядке")
    p.add_argument("--graphs", nargs="+", help="какие графы показывать и в каком порядке (без расширения)")
    p.add_argument("--sort", choices=["time", "name", "none"], default="time",
                   help="порядок графов по X: time — по медианному времени (по умолчанию), "
                        "name — по алфавиту, none — как в CSV")
    p.add_argument("--cols", type=int, help="число панелей в строке (по умолчанию все в одну строку)")
    p.add_argument("--no-sharey", action="store_true", help="отдельная ось Y у каждой панели")
    p.add_argument("--sum-chunks", action="store_true",
                   help="суммировать чанки в одну точку (разброс при этом пропадёт)")
    p.add_argument("--font", type=float, default=14)
    return p.parse_args()


def load(path, sum_chunks=False):
    df = pd.read_csv(path)
    df["graph"] = df["graph"].map(lambda x: os.path.splitext(os.path.basename(str(x)))[0])
    keys = ["algo_name", "cnf", "graph", "source_size"]
    if not sum_chunks:
        keys.append("chunk")   # точка = чанк
    return df.groupby(keys, as_index=False)["time_s"].sum()


def main():
    a = parse_args()
    df = load(a.csv, a.sum_chunks)

    algos = a.algos or list(dict.fromkeys(df["algo_name"]))
    df = df[df["algo_name"].isin(algos)]
    if a.graphs:
        df = df[df["graph"].isin(a.graphs)]
    if df.empty:
        raise SystemExit("Нет данных после фильтрации")

    if a.graphs:
        graphs = a.graphs
    elif a.sort == "time":
        graphs = list(df.groupby("graph")["time_s"].median().sort_values().index)
    elif a.sort == "name":
        graphs = sorted(df["graph"].unique())
    else:
        graphs = list(dict.fromkeys(df["graph"]))

    sizes = sorted(df["source_size"].unique())
    style = {}
    for i, s in enumerate(sizes):
        style[s] = STYLE.get(int(s)) or dict(color=FALLBACK[i % len(FALLBACK)], marker="D")

    # Сетка панелей
    n = len(algos)
    ncols = min(a.cols or n, n)
    nrows = int(np.ceil(n / ncols))
    panel_w = max(5.0, 1.0 * len(graphs) + 1.5)
    plt.rcParams.update({"font.size": a.font, "axes.titlesize": a.font + 2, "svg.fonttype": "none"})
    fig, axes = plt.subplots(nrows, ncols, figsize=(panel_w * ncols, 5.2 * nrows),
                             sharey=not a.no_sharey, squeeze=False)

    rng = np.random.default_rng(0)
    group_w = 0.84
    step = group_w / len(sizes)

    for k, (ax, algo) in enumerate(zip(axes.flat, algos)):
        sub = df[df["algo_name"] == algo]
        for gi, g in enumerate(graphs):
            for si, s in enumerate(sizes):
                y = sub[(sub["graph"] == g) & (sub["source_size"] == s)]["time_s"].to_numpy()
                if len(y) == 0:
                    continue
                st = style[s]
                x0 = gi + (si - (len(sizes) - 1) / 2) * step
                x = x0 + rng.uniform(-step * 0.40, step * 0.40, len(y))
                ax.scatter(x, y, s=22, color=st["color"], marker=st["marker"],
                           alpha=0.5, linewidths=0, zorder=2)
                ax.hlines(y.mean(), x0 - step * 0.42, x0 + step * 0.42,
                          color=st["color"], linewidth=4, zorder=4)
                ax.hlines(np.median(y), x0 - step * 0.25, x0 + step * 0.25,
                          color="black", linewidth=1.6, zorder=5)
        ax.set_yscale("log")
        ax.set_title(algo, fontweight="bold")
        ax.set_xticks(range(len(graphs)))
        ax.set_xticklabels(graphs, rotation=40, ha="right")
        ax.set_xlim(-0.55, len(graphs) - 0.45)
        for xv in np.arange(0.5, len(graphs) - 1, 1):
            ax.axvline(xv, color="#ccc", linewidth=0.6, zorder=0)
        ax.grid(axis="y", which="major", linestyle="--", alpha=0.45)
        ax.grid(axis="y", which="minor", linestyle=":", alpha=0.15)
        for sp in ("top", "right"):
            ax.spines[sp].set_visible(False)
        if k % ncols == 0:
            ax.set_ylabel("Время (сек)")
    for ax in list(axes.flat)[n:]:
        ax.set_visible(False)

    handles = [Line2D([], [], linestyle="", markersize=9, label=f"|S| = {s}", alpha=0.85, **style[s])
               for s in sizes]
    handles += [Line2D([], [], color="gray", linewidth=4, label="среднее (цветная черта)"),
                Line2D([], [], color="black", linewidth=1.6, label="медиана (чёрная черта)")]
    fig.legend(handles=handles, loc="upper center", ncol=len(handles), frameon=False,
               bbox_to_anchor=(0.5, 1.0))
    fig.tight_layout(rect=(0, 0, 1, 0.92))

    d = os.path.dirname(a.output)
    if d:
        os.makedirs(d, exist_ok=True)
    for ext in ("svg", "png"):
        fig.savefig(f"{a.output}.{ext}", dpi=200, bbox_inches="tight")
    print(f"Сохранено: {a.output}.svg, {a.output}.png")


if __name__ == "__main__":
    main()