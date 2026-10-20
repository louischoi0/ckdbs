import re, sys
L = "/home/cdkbs/bench-runs/v3.3.0-33-g9682b725/logs/"
tags = ["sat-b16-s2-c8-g", "sat-b16-s2-c8-s", "sat-b32-s2-c8-g", "sat-b32-s2-c8-s"]


def stats(t):
    b = []
    s = []
    for l in open(L + t + ".cpu"):
        m = re.search(r"busy=(\d+) steal=(\d+)", l)
        if m:
            b.append(int(m[1]))
            s.append(int(m[2]))
    return len(b), sum(b) / len(b), max(b), sum(s) / len(s)


if __name__ == "__main__":
    print("| cell | 1-s samples | mean busy | peak busy | mean steal |")
    print("|---|---|---|---|---|")
    for t in tags:
        n, m, x, st = stats(t)
        print(f"| `{t}` | {n} | {m:.0f} % | {x} % | {st:.1f} % |")
