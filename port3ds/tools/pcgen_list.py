#!/usr/bin/env python3
"""List the generated sources (hostgen, ovdata, ...) of the PC game targets
from the probe's File API reply, one line per target: "<target> <file>...",
paths relative to the probe build directory. pcprobe.sh makes them as goals
of each target's build.make, which runs only their generators."""
import glob
import json
import os
import sys

TARGETS = ('port_slice_shared', 'walk_window')


def main(probe):
    reply = glob.glob(os.path.join(probe, '.cmake/api/v1/reply/codemodel-v2-*.json'))[0]
    cm = json.load(open(reply))
    d = os.path.dirname(reply)
    tg = {t['name']: t['jsonFile'] for t in cm['configurations'][0]['targets']}
    for name in TARGETS:
        t = json.load(open(os.path.join(d, tg[name])))
        gen = []
        for s in t['sources']:
            p = s['path'].replace('\\', '/')
            if not s.get('isGenerated') or p.endswith(('.obj', '.o', '.rule')):
                continue
            gen.append(p.split('build-pcprobe/')[-1])
        print('%s %s' % (name, ' '.join(gen)))


if __name__ == '__main__':
    main(sys.argv[1])
