#!/usr/bin/env python3
"""
Generates tables/terrain_splines.h: the "factor" and "jaggedness" splines of
the 1.18+ overworld terrain, for the world types Default and Amplified.

usage: terrain_splines.py DATA_DIR [DATA_DIR ...] > ../tables/terrain_splines.h
       terrain_splines.py --dump NAME VARIANT DATA_DIR

DATA_DIR is the root of an unpacked vanilla data pack of one release, i.e. the
directory that holds data/minecraft/worldgen/ (for example a checkout of a
"<version>-data" tag of https://github.com/misode/mcmeta, or the "data" folder
from the server jar). 1.18.2 and later keep the splines in
worldgen/density_function/overworld{,_amplified}/{offset,factor,jaggedness}.json;
1.18 and 1.18.1 keep them in worldgen/noise_settings/{overworld,amplified}.json
under noise.terrain_shaper. Both layouts are read.

With several DATA_DIRs the script checks that every release has the same
splines (as float32) and fails otherwise, so the single table is known to be
valid for all of them. It also checks that Default and Amplified differ only
in the constant values (same shape, locations and derivatives), which is what
allows one table with two value columns.

--dump prints one spline (NAME: offset, factor or jaggedness; VARIANT:
overworld or amplified) as nested lists [location, derivative, value] with
float32 values, for comparisons with other implementations.

The offset spline is not part of the table: the engine constructs it in
initBiomeNoiseEx(). The script still reads it for the consistency checks.
"""
import json
import os
import struct
import sys

# order of the spline coordinates in biomenoise.c (SP_*)
COORDS = {
    'continents': 0, 'erosion': 1, 'ridges_folded': 2, 'ridges': 3,
}
# names in the terrain_shaper of 1.18 and 1.18.1
COORDS_118 = {
    'continents': 0, 'erosion': 1, 'ridges': 2, 'weirdness': 3,
}
NAMES = ('offset', 'factor', 'jaggedness')
VARIANTS = ('overworld', 'amplified')


def f32(x):
    return struct.unpack('f', struct.pack('f', x))[0]


def find_spline(x):
    if isinstance(x, dict):
        if 'coordinate' in x and 'points' in x:
            return x
        for v in x.values():
            r = find_spline(v)
            if r is not None:
                return r
    elif isinstance(x, list):
        for v in x:
            r = find_spline(v)
            if r is not None:
                return r
    return None


def load_spline(data_dir, variant, name):
    """Returns (json spline, coordinate name table)."""
    base = os.path.join(data_dir, 'data', 'minecraft', 'worldgen')
    sub = 'overworld' if variant == 'overworld' else 'overworld_amplified'
    path = os.path.join(base, 'density_function', sub, name + '.json')
    if os.path.isfile(path):
        with open(path) as f:
            s = find_spline(json.load(f))
        if s is not None:
            return s, COORDS
    with open(os.path.join(base, 'noise_settings', variant + '.json')) as f:
        ns = json.load(f)
    return ns['noise']['terrain_shaper'][name], COORDS_118


def normalize(s, coords):
    """Nested tuples (coordinate, ((location, derivative, value), ...)) with
    float32 numbers; a constant is a float."""
    if isinstance(s, (int, float)):
        return f32(s)
    c = s['coordinate']
    if not isinstance(c, str):
        raise ValueError('unexpected spline coordinate %r' % (c,))
    c = c.split(':')[-1].split('/')[-1]
    pts = tuple((f32(p['location']), f32(p['derivative']),
                 normalize(p['value'], coords)) for p in s['points'])
    return (coords[c], pts)


def same_shape(a, b):
    if isinstance(a, float) or isinstance(b, float):
        return isinstance(a, float) and isinstance(b, float)
    if a[0] != b[0] or len(a[1]) != len(b[1]):
        return False
    for pa, pb in zip(a[1], b[1]):
        if pa[0] != pb[0] or pa[1] != pb[1] or not same_shape(pa[2], pb[2]):
            return False
    return True


def as_lists(s):
    if isinstance(s, float):
        return s
    return [s[0], [[p[0], p[1], as_lists(p[2])] for p in s[1]]]


def flt(x):
    s = '%.9g' % x
    if 'e' not in s and '.' not in s and 'inf' not in s:
        s += '.0'
    return s + 'F'


class Table:
    """Flattens a pair of splines of the same shape, sharing equal subtrees."""

    def __init__(self):
        self.nodes = []     # (coordinate, first point, number of points)
        self.points = []    # (location, derivative, default, amplified, child)
        self.known = {}

    def add(self, d, a):
        key = (d, a)
        if key in self.known:
            return self.known[key]
        idx = len(self.nodes)
        self.known[key] = idx
        self.nodes.append(None)
        pts = []
        for pd, pa in zip(d[1], a[1]):
            if isinstance(pd[2], float):
                pts.append((pd[0], pd[1], pd[2], pa[2], -1))
            else:
                pts.append((pd[0], pd[1], 0.0, 0.0, self.add(pd[2], pa[2])))
        self.nodes[idx] = (d[0], len(self.points), len(pts))
        self.points.extend(pts)
        return idx


def main(argv):
    if len(argv) >= 2 and argv[1] == '--dump':
        s, coords = load_spline(argv[4], argv[3], argv[2])
        json.dump(as_lists(normalize(s, coords)), sys.stdout)
        print()
        return 0
    dirs = argv[1:]
    if not dirs:
        sys.stderr.write(__doc__)
        return 1

    ref = None
    for d in dirs:
        cur = {}
        for name in NAMES:
            for variant in VARIANTS:
                s, coords = load_spline(d, variant, name)
                cur[name, variant] = normalize(s, coords)
            if not same_shape(cur[name, 'overworld'], cur[name, 'amplified']):
                sys.stderr.write('%s: the %s splines of Default and Amplified '
                                 'differ in shape\n' % (d, name))
                return 1
        if ref is None:
            ref = cur
        elif cur != ref:
            diff = [k for k in ref if ref[k] != cur[k]]
            sys.stderr.write('%s: splines differ from %s: %r\n' % (d, dirs[0], diff))
            return 1

    out = []
    out.append('// Generated by docs/terrain_splines.py from the vanilla worldgen data, do not')
    out.append('// edit. The splines "factor" and "jaggedness" of the 1.18+ overworld terrain')
    out.append('// (density functions overworld/factor, overworld/jaggedness and their')
    out.append('// overworld_amplified counterparts). They are the same in every release from')
    out.append('// 1.18 to 26.3; the script checks this for the releases it is given.')
    out.append('//')
    out.append('// A spline is a list of nodes, node 0 is the root. A node interpolates over')
    out.append('// its points along one coordinate (typ: SP_* in biomenoise.c). The value of a')
    out.append('// point is another node (child >= 0) or a constant: val[0] in a default world,')
    out.append('// val[1] in an Amplified world. Locations and derivatives are shared.')
    out.append('')
    out.append('#ifndef TERRAIN_SPLINES_H_')
    out.append('#define TERRAIN_SPLINES_H_')
    out.append('')
    out.append('typedef struct { float loc, der, val[2]; short child; } TerrainSplinePoint;')
    out.append('typedef struct { unsigned char typ, len; unsigned short first; } TerrainSplineNode;')
    for name in ('factor', 'jaggedness'):
        t = Table()
        root = t.add(ref[name, 'overworld'], ref[name, 'amplified'])
        assert root == 0
        out.append('')
        out.append('static const TerrainSplinePoint g_%s_points[%d] =\n{' % (name, len(t.points)))
        for i, (loc, der, vd, va, child) in enumerate(t.points):
            out.append('    {%s, %s, {%s, %s}, %d}, // %d' % (
                flt(loc), flt(der), flt(vd), flt(va), child, i))
        out.append('};')
        out.append('')
        out.append('static const TerrainSplineNode g_%s_nodes[%d] =\n{' % (name, len(t.nodes)))
        for i, (typ, first, n) in enumerate(t.nodes):
            out.append('    {%d, %d, %d}, // %d' % (typ, n, first, i))
        out.append('};')
    out.append('')
    out.append('#endif // TERRAIN_SPLINES_H_')
    print('\n'.join(out))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
