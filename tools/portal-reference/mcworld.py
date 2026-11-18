"""Minimal, dependency-free reader for Minecraft Anvil region files (1.18+).

Only what the portal reference harness needs: NBT decoding, chunk lookup in
.mca files (including oversized .mcc chunks), chunk status, structure starts
and block-state lookup by palette name.
"""

import gzip
import os
import struct
import zlib

TAG_END, TAG_BYTE, TAG_SHORT, TAG_INT, TAG_LONG, TAG_FLOAT, TAG_DOUBLE = range(7)
TAG_BYTE_ARRAY, TAG_STRING, TAG_LIST, TAG_COMPOUND, TAG_INT_ARRAY, TAG_LONG_ARRAY = range(7, 13)


class _Reader:
    def __init__(self, data):
        self.d = data
        self.p = 0

    def take(self, n):
        b = self.d[self.p:self.p + n]
        self.p += n
        return b

    def unpack(self, fmt):
        s = struct.calcsize(fmt)
        v = struct.unpack_from(fmt, self.d, self.p)
        self.p += s
        return v[0] if len(v) == 1 else v

    def string(self):
        n = self.unpack(">H")
        # Java "modified UTF-8"; plain utf-8 is sufficient for vanilla ids
        return self.take(n).decode("utf-8", errors="surrogatepass")


def _payload(r, t):
    if t == TAG_BYTE:
        return r.unpack(">b")
    if t == TAG_SHORT:
        return r.unpack(">h")
    if t == TAG_INT:
        return r.unpack(">i")
    if t == TAG_LONG:
        return r.unpack(">q")
    if t == TAG_FLOAT:
        return r.unpack(">f")
    if t == TAG_DOUBLE:
        return r.unpack(">d")
    if t == TAG_BYTE_ARRAY:
        n = r.unpack(">i")
        return r.take(n)
    if t == TAG_STRING:
        return r.string()
    if t == TAG_LIST:
        et = r.unpack(">b")
        n = r.unpack(">i")
        return [_payload(r, et) for _ in range(n)]
    if t == TAG_COMPOUND:
        out = {}
        while True:
            ct = r.unpack(">b")
            if ct == TAG_END:
                return out
            name = r.string()
            out[name] = _payload(r, ct)
    if t == TAG_INT_ARRAY:
        n = r.unpack(">i")
        return list(struct.unpack_from(">%di" % n, r.take(4 * n)))
    if t == TAG_LONG_ARRAY:
        n = r.unpack(">i")
        return list(struct.unpack_from(">%dq" % n, r.take(8 * n)))
    raise ValueError("unknown NBT tag type %d" % t)


def parse_nbt(data):
    """Parse an uncompressed NBT blob with a named root compound."""
    r = _Reader(data)
    t = r.unpack(">b")
    if t != TAG_COMPOUND:
        raise ValueError("root is not a compound")
    r.string()
    return _payload(r, t)


def _decompress(kind, raw):
    if kind == 1:
        return gzip.decompress(raw)
    if kind == 2:
        return zlib.decompress(raw)
    if kind == 3:
        return raw
    raise ValueError("unsupported chunk compression %d (use deflate)" % kind)


class Region:
    """One .mca file; chunks are decoded lazily."""

    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.data = f.read()

    def chunk(self, cx, cz):
        if len(self.data) < 8192:
            return None
        i = (cx & 31) + (cz & 31) * 32
        loc = struct.unpack_from(">I", self.data, 4 * i)[0]
        off, cnt = loc >> 8, loc & 0xFF
        if off == 0 or cnt == 0:
            return None
        p = off * 4096
        length, kind = struct.unpack_from(">IB", self.data, p)
        if kind & 0x80:
            mcc = os.path.join(os.path.dirname(self.path), "c.%d.%d.mcc" % (cx, cz))
            with open(mcc, "rb") as f:
                raw = f.read()
            kind &= 0x7F
        else:
            raw = self.data[p + 5:p + 4 + length]
        return parse_nbt(_decompress(kind, raw))


class World:
    """Chunk access for one dimension's region directory."""

    def __init__(self, region_dir):
        self.dir = region_dir
        self._regions = {}

    def chunk(self, cx, cz):
        key = (cx >> 5, cz >> 5)
        if key not in self._regions:
            path = os.path.join(self.dir, "r.%d.%d.mca" % key)
            self._regions[key] = Region(path) if os.path.exists(path) else None
        reg = self._regions[key]
        return reg.chunk(cx, cz) if reg else None


def read_nbt_file(path):
    """Parse a gzip-compressed NBT file such as level.dat."""
    with open(path, "rb") as f:
        return parse_nbt(gzip.decompress(f.read()))


# World preset name -> id of the overworld's noise settings.
NOISE_SETTINGS = {
    "normal": "minecraft:overworld",
    "amplified": "minecraft:amplified",
    "large_biomes": "minecraft:large_biomes",
}


def overworld_noise_settings(world_dir):
    """Id of the noise settings of the overworld generator of a saved world.

    This is what tells the world types apart: 'minecraft:overworld' (Default),
    'minecraft:amplified', 'minecraft:large_biomes'.
    """
    return dimension_noise_settings(world_dir, "minecraft:overworld")


def dimension_noise_settings(world_dir, dimension):
    """The noise settings of a dimension's generator in a saved world: an id,
    or the settings themselves (a dict) if the world stores them inline.

    26.1+ keeps the dimensions of the world preset in their own file; a data
    pack's dimension file replaces them only when the world is loaded.
    """
    path = os.path.join(world_dir, "data", "minecraft", "world_gen_settings.dat")
    if os.path.exists(path):
        wgs = read_nbt_file(path)["data"]
    else:
        wgs = read_nbt_file(os.path.join(world_dir, "level.dat"))["Data"]["WorldGenSettings"]
    return wgs["dimensions"][dimension]["generator"]["settings"]


def chunk_status(chunk):
    if chunk is None:
        return None
    return chunk.get("Status")


def structure_starts(chunk):
    if chunk is None:
        return {}
    s = chunk.get("structures", {})
    return s.get("starts", {})


def palette_entry(e, defaults=None):
    """Normalise a block palette entry to (name, properties).

    Pre-26.1: {"Name": id, "Properties": {...}}. 26.1+: {"id": id,
    "properties": {...}}, or just the id (a plain string, or a string wrapped
    as {"": id} inside a heterogeneous list) for a block in its default state.
    For default-state entries 'defaults' (id -> properties) fills in the
    properties if known, otherwise an empty dict is returned.
    """
    if isinstance(e, str):
        name, props = e, None
    elif "" in e and len(e) == 1:
        name, props = e[""], None
    elif "Name" in e:
        name, props = e["Name"], e.get("Properties", {})
    else:
        name, props = e["id"], e.get("properties", {})
    if props is None:
        props = dict((defaults or {}).get(name, {}))
    return name, props


def find_blocks(chunk, name, defaults=None):
    """Yield (x, y, z, properties) for every block of the given id in a chunk."""
    cx, cz = chunk["xPos"], chunk["zPos"]
    for sec in chunk.get("sections", []):
        bs = sec.get("block_states")
        if not bs:
            continue
        pal = [palette_entry(e, defaults) for e in bs["palette"]]
        idxs = [i for i, e in enumerate(pal) if e[0] == name]
        if not idxs:
            continue
        sy = sec["Y"]
        data = bs.get("data")
        if data is None:
            # single-entry palette: whole section is this block
            for n in range(4096):
                yield (cx * 16 + (n & 15), sy * 16 + (n >> 8), cz * 16 + ((n >> 4) & 15),
                       pal[0][1])
            continue
        bits = max(4, (len(pal) - 1).bit_length())
        per = 64 // bits
        mask = (1 << bits) - 1
        want = set(idxs)
        for n in range(4096):
            word = data[n // per] & 0xFFFFFFFFFFFFFFFF
            v = (word >> ((n % per) * bits)) & mask
            if v in want:
                yield (cx * 16 + (n & 15), sy * 16 + (n >> 8), cz * 16 + ((n >> 4) & 15),
                       pal[v][1])


# ------------------------------------------------- heightmaps, biomes, blocks

AIR = ("minecraft:air", "minecraft:cave_air", "minecraft:void_air")


def unpack_longs(data, bits, count):
    """Entries of a vanilla SimpleBitStorage long array.

    Entries are 'bits' wide, the first entry sits in the lowest bits of the
    first long, and no entry spans two longs (format since 1.16).
    """
    per = 64 // bits
    if len(data) != (count + per - 1) // per:
        raise ValueError("%d longs for %d entries of %d bits" % (len(data), count, bits))
    mask = (1 << bits) - 1
    out = []
    for word in data:
        word &= 0xFFFFFFFFFFFFFFFF
        for _ in range(per):
            out.append(word & mask)
            word >>= bits
    return out[:count]


def heightmap(chunk, name, height=384):
    """A stored chunk heightmap as 256 absolute y values, index (z & 15) * 16 + (x & 15).

    Each value is the y of the highest block the heightmap counts, i.e.
    vanilla's Heightmap.getFirstAvailable() - 1; min_y - 1 for an empty column.
    Stored entries are relative to min_y and ceil(log2(height + 1)) bits wide.
    """
    min_y = chunk["yPos"] * 16
    bits = height.bit_length()
    return [min_y + v - 1 for v in unpack_longs(chunk["Heightmaps"][name], bits, 256)]


class ChunkView:
    """Block and biome lookup by block coordinates in one chunk (sections decoded lazily)."""

    def __init__(self, chunk, defaults=None):
        self.chunk = chunk
        self.min_y = chunk["yPos"] * 16
        self.defaults = defaults
        self.sections = {sec["Y"]: sec for sec in chunk.get("sections", [])}
        self._blocks = {}
        self._biomes = {}

    def _section_blocks(self, sy):
        if sy not in self._blocks:
            bs = self.sections.get(sy, {}).get("block_states")
            if not bs:
                self._blocks[sy] = None
            else:
                pal = [palette_entry(e, self.defaults)[0] for e in bs["palette"]]
                data = bs.get("data")
                if data is None:
                    if len(pal) != 1:
                        raise ValueError("block palette of %d entries without data" % len(pal))
                    self._blocks[sy] = (pal, None)
                else:
                    bits = max(4, (len(pal) - 1).bit_length())
                    self._blocks[sy] = (pal, unpack_longs(data, bits, 4096))
        return self._blocks[sy]

    def block(self, x, y, z):
        """Block id at a block position; None where the chunk stores no section."""
        sec = self._section_blocks(y >> 4)
        if sec is None:
            return None
        pal, idx = sec
        return pal[0] if idx is None else pal[idx[((y & 15) << 8) | ((z & 15) << 4) | (x & 15)]]

    def top_block(self, x, z, skip=AIR):
        """(y, id) of the highest block whose id is not in skip, or (min_y - 1, None)."""
        for sy in sorted(self.sections, reverse=True):
            sec = self._section_blocks(sy)
            if sec is None:
                continue
            pal, idx = sec
            if all(n in skip for n in pal):
                continue
            for y in range(sy * 16 + 15, sy * 16 - 1, -1):
                name = pal[0] if idx is None else pal[idx[((y & 15) << 8) | ((z & 15) << 4) | (x & 15)]]
                if name not in skip:
                    return y, name
        return self.min_y - 1, None

    def _section_biomes(self, sy):
        if sy not in self._biomes:
            b = self.sections[sy]["biomes"]
            pal = [e if isinstance(e, str) else e[""] for e in b["palette"]]
            data = b.get("data")
            if data is None:
                if len(pal) != 1:
                    raise ValueError("biome palette of %d entries without data" % len(pal))
                self._biomes[sy] = pal * 64
            else:
                # unlike block states there is no 4-bit minimum for biomes
                bits = max(1, (len(pal) - 1).bit_length())
                self._biomes[sy] = [pal[i] for i in unpack_longs(data, bits, 64)]
        return self._biomes[sy]

    def biome(self, x, y, z):
        """Biome id of the 4x4x4 cell containing a block position."""
        cells = self._section_biomes(y >> 4)
        return cells[(((y >> 2) & 3) << 4) | (((z >> 2) & 3) << 2) | ((x >> 2) & 3)]
