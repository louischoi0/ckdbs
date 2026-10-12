import json, sys
d = json.load(open(sys.argv[1]))
for c in d['clients']:
    print(c['role'], c['index'], c['core'], c['core_end'], c['ok'], c['errors'], c['refusals'], c['slow_dropped'], len(c['slow']), c['slow'][:2])
print(d['roles'], d['targets'], d['cores_seen'], d['cores_end'])
print({k: v for k, v in d.items() if k.startswith('meta_') and not isinstance(v, dict)})
print([k for k in d if isinstance(d[k], dict)])
