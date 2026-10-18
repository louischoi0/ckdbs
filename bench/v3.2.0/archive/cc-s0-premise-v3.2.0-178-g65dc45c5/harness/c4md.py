import json, sys
rows = json.load(open(sys.argv[1] + '.table'))
print("| Relation | height | leaves (walked) | live rows (COUNT) | rows per leaf | row size | leaf capacity | prior = leaves x cap x 0.75 | prior / true |")
print("|---|---|---|---|---|---|---|---|---|")
h1 = []
ratios = []
for name, h, lv, tr, rs, capA, capB in rows:
    if h == 1:
        h1.append((name, tr, lv, rs, capA)); continue
    prior = lv * capA * 0.75
    ratios.append(prior / tr)
    print(f"| {name} | {h} | {lv:,} | {tr:,} | {tr/lv:.1f} | {rs} B | {capA} | {prior:,.0f} | {prior/tr:.2f}x |")
print()
print("range", min(ratios), max(ratios), "n", len(ratios))
print("h1:")
for name, tr, lv, rs, cap in h1:
    print(f"  {name}: rows {tr}, formula-if-misapplied {lv*cap*0.75:.0f} ({lv*cap*0.75/tr:.2f}x)")
