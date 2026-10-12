import json, sys
d = json.load(open(sys.argv[1]))
def show(o, p='', dep=0):
    if isinstance(o, dict):
        for k, v in o.items():
            if isinstance(v, (dict, list)):
                print(p + k, type(v).__name__, len(v))
                if dep < 2:
                    show(v, p + '  ', dep + 1)
            else:
                print(p + k, '=', str(v)[:100])
    elif isinstance(o, list) and o:
        show(o[0], p + '  [0] ', dep + 1)
show(d)
