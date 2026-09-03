#!/usr/bin/env python3
"""Generate the LOD meshes Grass Optimizations looks for.

`GrassMeshLibrary::LoadLODMesh` asks BSModelDB for
``LOD\\Grass\\<source-mesh-stem>_LOD0.nif`` (the middle tier) and ``_LOD1.nif``
(the far tier), where the stem is the source grass .nif's filename without its
directory or extension. A missing file just means that tier keeps the full mesh,
and a missing far mesh falls back to the middle one.
Usage:
    python tools/make_grass_lod.py --src <dir of grass .nif> --out <dir>
    python tools/make_grass_lod.py --src ... --out ... --mid-ratio 0.6 --far-ratio 0.3
"""

import argparse
import math
import os
import struct
import sys

NIF_VERSION = 0x14020007
GEOMETRY_TYPES = (
    "BSTriShape",
    "BSLODTriShape",
    "BSMeshLODTriShape",
    "BSSubIndexTriShape",
    "BSDynamicTriShape",
)

# Byte offsets inside one 32-byte SSE grass vertex. Only the position is rewritten;
# every other field is copied verbatim so the LOD samples the same atlas.
POS_OFFSET = 0

# BSTriShape stores NiBound, then the skin/shader/alpha refs, immediately before the
# vertex descriptor that ScanGeometry locates.
BOUND_BACK_OFFSET = 28


class Reader:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def raw(self, count):
        chunk = self.data[self.pos:self.pos + count]
        if len(chunk) != count:
            raise ValueError("unexpected end of file")
        self.pos += count
        return chunk

    def u8(self):
        return self.raw(1)[0]

    def u16(self):
        return struct.unpack_from("<H", self.raw(2))[0]

    def u32(self):
        return struct.unpack_from("<I", self.raw(4))[0]

    def i32(self):
        return struct.unpack_from("<i", self.raw(4))[0]

    def line(self):
        end = self.data.index(b"\n", self.pos)
        out = self.data[self.pos:end]
        self.pos = end + 1
        return out

    def short_string(self):
        return self.raw(self.u8())

    def sized_string(self):
        return self.raw(self.u32())


class Nif:
    """Header-only NIF model: blocks stay opaque bytes so a rewrite is byte-exact."""

    def __init__(self, data):
        r = Reader(data)
        self.header_string = r.line()
        self.version = r.u32()
        if self.version != NIF_VERSION:
            raise ValueError("unsupported NIF version 0x%08X" % self.version)
        self.endian = r.u8()
        self.user_version = r.u32()
        block_count = r.u32()
        self.bs_version = r.u32()
        self.export_info = [r.short_string() for _ in range(3)]
        self.max_filepath = r.short_string() if self.bs_version >= 130 else None
        self.block_types = [r.sized_string() for _ in range(r.u16())]
        self.block_type_index = [r.u16() for _ in range(block_count)]
        block_sizes = [r.u32() for _ in range(block_count)]
        string_count = r.u32()
        r.u32()  # max string length, recomputed on write
        self.strings = [r.sized_string() for _ in range(string_count)]
        self.groups = [r.u32() for _ in range(r.u32())]
        self.blocks = [bytearray(r.raw(size)) for size in block_sizes]
        self.roots = [r.i32() for _ in range(r.u32())]
        if r.pos != len(data):
            raise ValueError("%d trailing bytes" % (len(data) - r.pos))

    def type_of(self, index):
        return self.block_types[self.block_type_index[index]].decode("latin-1")

    def to_bytes(self):
        out = bytearray()
        out += self.header_string + b"\n"
        out += struct.pack("<I", self.version)
        out += bytes([self.endian])
        out += struct.pack("<II", self.user_version, len(self.blocks))
        out += struct.pack("<I", self.bs_version)
        for entry in self.export_info:
            out += bytes([len(entry)]) + entry
        if self.max_filepath is not None:
            out += bytes([len(self.max_filepath)]) + self.max_filepath
        out += struct.pack("<H", len(self.block_types))
        for block_type in self.block_types:
            out += struct.pack("<I", len(block_type)) + block_type
        for index in self.block_type_index:
            out += struct.pack("<H", index)
        for block in self.blocks:
            out += struct.pack("<I", len(block))
        out += struct.pack("<I", len(self.strings))
        out += struct.pack("<I", max((len(s) for s in self.strings), default=0))
        for text in self.strings:
            out += struct.pack("<I", len(text)) + text
        out += struct.pack("<I", len(self.groups))
        for group in self.groups:
            out += struct.pack("<I", group)
        for block in self.blocks:
            out += block
        out += struct.pack("<I", len(self.roots))
        for root in self.roots:
            out += struct.pack("<i", root)
        return bytes(out)


class Geometry:
    """The vertex/index payload at the tail of a BSTriShape block."""

    def __init__(self, offset, desc, stride, vertices, triangles):
        self.offset = offset
        self.desc = desc
        self.stride = stride
        self.vertices = vertices  # list of `stride`-byte records
        self.triangles = triangles  # list of (a, b, c)


def vertex_stride(desc):
    """Mirrors VertexStrideFromDesc in GrassMeshLibrary.h."""
    return (desc & 0xF) * 4


def scan_geometry(block):
    """Locates the vertex descriptor by anchoring on the block's known tail size."""
    limit = len(block) - 16
    for offset in range(max(0, limit)):
        desc = struct.unpack_from("<Q", block, offset)[0]
        stride = vertex_stride(desc)
        if stride == 0:
            continue
        tri_count, vert_count = struct.unpack_from("<HH", block, offset + 8)
        if not tri_count or not vert_count:
            continue
        data_size = struct.unpack_from("<I", block, offset + 12)[0]
        if data_size != vert_count * stride + tri_count * 6:
            continue
        # ... followed only by the 4-byte particle data size, which grass never uses.
        if offset + 16 + data_size + 4 != len(block):
            continue
        base = offset + 16
        vertices = [bytes(block[base + i * stride:base + (i + 1) * stride]) for i in range(vert_count)]
        tri_base = base + vert_count * stride
        triangles = [struct.unpack_from("<3H", block, tri_base + i * 6) for i in range(tri_count)]
        return Geometry(offset, desc, stride, vertices, triangles)
    return None


def read_position(record):
    return struct.unpack_from("<3f", record, POS_OFFSET)


def write_position(record, position):
    out = bytearray(record)
    struct.pack_into("<3f", out, POS_OFFSET, *position)
    return bytes(out)


def connected_components(geom):
    """Groups triangles into cards by shared vertex index."""
    parent = list(range(len(geom.vertices)))

    def find(node):
        while parent[node] != node:
            parent[node] = parent[parent[node]]
            node = parent[node]
        return node

    for a, b, c in geom.triangles:
        for other in (b, c):
            root_a, root_b = find(a), find(other)
            if root_a != root_b:
                parent[root_a] = root_b

    groups = {}
    for index, tri in enumerate(geom.triangles):
        groups.setdefault(find(tri[0]), []).append(index)
    return list(groups.values())


class Card:
    def __init__(self, geom, tri_indices):
        self.tri_indices = tri_indices
        self.vertex_indices = sorted({v for i in tri_indices for v in geom.triangles[i]})
        positions = [read_position(geom.vertices[v]) for v in self.vertex_indices]
        self.top = max(p[2] for p in positions)
        self.centroid = (
            sum(p[0] for p in positions) / len(positions),
            sum(p[1] for p in positions) / len(positions),
        )
        self.area = 0.0
        for i in tri_indices:
            a, b, c = (read_position(geom.vertices[v]) for v in geom.triangles[i])
            u = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
            v = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
            cross = (
                u[1] * v[2] - u[2] * v[1],
                u[2] * v[0] - u[0] * v[2],
                u[0] * v[1] - u[1] * v[0],
            )
            self.area += 0.5 * math.sqrt(sum(component * component for component in cross))


def select_cards(cards, target_triangles):
    """Area-weighted farthest-point sampling, so survivors stay spread across the clump.

    Seeded with the tallest card because the clump's top edge is the one silhouette
    cue still readable at the pixel sizes these tiers swap in at. Area carries more
    weight than spacing: a triangle budget spent on the cards that cover the most
    leaves less silhouette for the widening pass to make up.
    """
    if not cards:
        return []

    mean_area = sum(card.area for card in cards) / len(cards) or 1.0
    # Spacing is measured against the clump's own footprint so the two terms stay
    # comparable whether the mesh is a few units across or fifty.
    span = max(
        max(card.centroid[axis] for card in cards) - min(card.centroid[axis] for card in cards)
        for axis in (0, 1)
    )
    span = max(span, 1e-4)

    remaining = list(range(len(cards)))
    seed = max(remaining, key=lambda i: (cards[i].top, cards[i].area))
    chosen = [seed]
    remaining.remove(seed)
    kept_triangles = len(cards[seed].tri_indices)

    def spacing(i, j):
        dx = cards[i].centroid[0] - cards[j].centroid[0]
        dy = cards[i].centroid[1] - cards[j].centroid[1]
        return math.hypot(dx, dy) / span

    nearest = {i: spacing(i, seed) for i in remaining}

    while remaining and kept_triangles < target_triangles:
        best = max(remaining, key=lambda i: math.sqrt(max(nearest[i], 1e-6)) * (cards[i].area / mean_area))
        chosen.append(best)
        remaining.remove(best)
        kept_triangles += len(cards[best].tri_indices)
        for i in remaining:
            nearest[i] = min(nearest[i], spacing(i, best))

    return sorted(chosen)


GRID = 32
COVERAGE_SAMPLES = 3000
WIDEN_STEPS = 7


def van_der_corput(index, base):
    value, denominator = 0.0, 1.0
    while index:
        index, digit = divmod(index, base)
        denominator *= base
        value += digit / denominator
    return value


def card_samples(geom, card, budget, total_area):
    """Points spread over the card's triangles, for the occupancy estimate."""
    points = []
    for i in card.tri_indices:
        a, b, c = (read_position(geom.vertices[v]) for v in geom.triangles[i])
        area = 0.0
        u = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        v = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
        cross = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
        area = 0.5 * math.sqrt(sum(component * component for component in cross))
        count = max(1, int(round(budget * area / total_area)))
        for n in range(count):
            s = van_der_corput(n + 1, 2)
            t = van_der_corput(n + 1, 3)
            if s + t > 1.0:
                s, t = 1.0 - s, 1.0 - t
            points.append(tuple(a[axis] + u[axis] * s + v[axis] * t for axis in range(3)))
    return points


# Grass is read at a shallow angle at LOD distance, so the two side silhouettes matter
# most, but ground cover only shows up from above; average all three.
PROJECTIONS = ((0, 2), (1, 2), (0, 1))


def occupancy(samples_by_card, centroids, indices, widen, bounds):
    """Fraction of grid cells the given cards cover, averaged over the three projections."""
    lo, hi = bounds
    scale = [GRID / max(hi[axis] - lo[axis], 1e-4) for axis in range(3)]
    grids = [set() for _ in PROJECTIONS]
    for i in indices:
        cx, cy = centroids[i]
        for x, y, z in samples_by_card[i]:
            if widen != 1.0:
                x = cx + (x - cx) * widen
                y = cy + (y - cy) * widen
            cell = [
                min(GRID - 1, max(0, int((x - lo[0]) * scale[0]))),
                min(GRID - 1, max(0, int((y - lo[1]) * scale[1]))),
                min(GRID - 1, max(0, int((z - lo[2]) * scale[2]))),
            ]
            for grid, (u, v) in zip(grids, PROJECTIONS):
                grid.add((cell[u], cell[v]))
    return sum(len(grid) for grid in grids) / float(len(grids) * GRID * GRID)


def solve_widen(geom, cards, chosen, max_widen):
    """Smallest XY scale that puts the survivors' silhouette back at the full mesh's.

    Measuring occupancy rather than summed area matters: a flower head built from five
    coincident billboards loses almost no silhouette when four of them go, and scaling
    by the area it "lost" would visibly fatten the remaining petal card.
    """
    if max_widen <= 1.0 or len(chosen) == len(cards):
        return 1.0

    total_area = sum(card.area for card in cards) or 1.0
    samples = [card_samples(geom, card, COVERAGE_SAMPLES, total_area) for card in cards]
    centroids = [card.centroid for card in cards]

    corners = []
    for i, card in enumerate(cards):
        cx, cy = card.centroid
        for x, y, z in samples[i]:
            corners.append((cx + (x - cx) * max_widen, cy + (y - cy) * max_widen, z))
    bounds = (
        tuple(min(p[axis] for p in corners) for axis in range(3)),
        tuple(max(p[axis] for p in corners) for axis in range(3)),
    )

    target = occupancy(samples, centroids, range(len(cards)), 1.0, bounds)
    if occupancy(samples, centroids, chosen, 1.0, bounds) >= target:
        return 1.0
    if occupancy(samples, centroids, chosen, max_widen, bounds) < target:
        return max_widen

    low, high = 1.0, max_widen
    for _ in range(WIDEN_STEPS):
        mid = 0.5 * (low + high)
        if occupancy(samples, centroids, chosen, mid, bounds) >= target:
            high = mid
        else:
            low = mid
    return high


def build_reduced(geom, keep_ratio, max_widen, min_triangles):
    cards = [Card(geom, tri_indices) for tri_indices in connected_components(geom)]

    target = max(min_triangles, int(round(len(geom.triangles) * keep_ratio)))
    if target >= len(geom.triangles):
        chosen = list(range(len(cards)))
    else:
        chosen = select_cards(cards, target)

    widen = solve_widen(geom, cards, chosen, max_widen)

    vertex_map = {}
    vertices = []
    triangles = []
    for card_index in chosen:
        card = cards[card_index]
        cx, cy = card.centroid
        for v in card.vertex_indices:
            record = geom.vertices[v]
            if widen > 1.0:
                x, y, z = read_position(record)
                record = write_position(record, (cx + (x - cx) * widen, cy + (y - cy) * widen, z))
            vertex_map[v] = len(vertices)
            vertices.append(record)
        for i in card.tri_indices:
            a, b, c = geom.triangles[i]
            triangles.append((vertex_map[a], vertex_map[b], vertex_map[c]))

    return vertices, triangles, widen


def bounding_sphere(vertices):
    positions = [read_position(record) for record in vertices]
    center = tuple(
        (min(p[axis] for p in positions) + max(p[axis] for p in positions)) * 0.5
        for axis in range(3)
    )
    radius = max(
        math.sqrt(sum((p[axis] - center[axis]) ** 2 for axis in range(3))) for p in positions
    )
    return center, radius


def rewrite_block(block, geom, vertices, triangles):
    if len(vertices) > 0xFFFF or len(triangles) > 0xFFFF:
        raise ValueError("reduced mesh exceeds the 16-bit vertex/triangle limits")

    head = bytes(block[:geom.offset])
    data = bytearray()
    for record in vertices:
        data += record
    for tri in triangles:
        data += struct.pack("<3H", *tri)

    out = bytearray(head)
    out += struct.pack("<Q", geom.desc)
    out += struct.pack("<HHI", len(triangles), len(vertices), len(data))
    out += data
    out += struct.pack("<I", 0)  # particle data size

    center, radius = bounding_sphere(vertices)
    struct.pack_into("<4f", out, geom.offset - BOUND_BACK_OFFSET, center[0], center[1], center[2], radius)
    return out


def make_lod(source_bytes, keep_ratio, max_widen, min_triangles):
    nif = Nif(source_bytes)
    stats = []
    for index in range(len(nif.blocks)):
        if nif.type_of(index) not in GEOMETRY_TYPES:
            continue
        geom = scan_geometry(nif.blocks[index])
        if geom is None:
            raise ValueError("block %d (%s) has no readable geometry" % (index, nif.type_of(index)))
        vertices, triangles, widen = build_reduced(geom, keep_ratio, max_widen, min_triangles)
        nif.blocks[index] = rewrite_block(nif.blocks[index], geom, vertices, triangles)
        stats.append((len(geom.triangles), len(triangles), widen))
    if not stats:
        raise ValueError("no geometry blocks found")
    return nif.to_bytes(), stats


def verify(data, expect_triangles):
    """Re-parses a written file the way GrassMeshLibrary will see it."""
    nif = Nif(data)
    found = []
    for index in range(len(nif.blocks)):
        if nif.type_of(index) not in GEOMETRY_TYPES:
            continue
        geom = scan_geometry(nif.blocks[index])
        if geom is None:
            raise ValueError("block %d lost its geometry" % index)
        if vertex_stride(geom.desc) == 0:
            raise ValueError("block %d decoded to a zero stride" % index)
        for tri in geom.triangles:
            if max(tri) >= len(geom.vertices):
                raise ValueError("block %d has an out-of-range index" % index)
        found.append(len(geom.triangles))
    if found != expect_triangles:
        raise ValueError("triangle counts %s do not match %s" % (found, expect_triangles))
    if nif.to_bytes() != data:
        raise ValueError("file does not round-trip")


TIERS = (("_LOD0.nif", "mid"), ("_LOD1.nif", "far"))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--src", required=True, help="directory holding the source grass .nif files")
    parser.add_argument("--out", required=True, help="directory to write <stem>_LOD0.nif / _LOD1.nif into")
    parser.add_argument("--mid-ratio", type=float, default=0.6, help="triangle fraction kept for the middle tier")
    parser.add_argument("--far-ratio", type=float, default=0.3, help="triangle fraction kept for the far tier")
    parser.add_argument("--max-widen", type=float, default=2.0, help="cap on the XY scale applied to surviving cards")
    parser.add_argument("--min-triangles", type=int, default=4, help="floor so tiny meshes keep a readable shape")
    parser.add_argument("--dry-run", action="store_true", help="report what would be written without writing")
    args = parser.parse_args(argv)

    for ratio in (args.mid_ratio, args.far_ratio):
        if not 0.0 < ratio <= 1.0:
            parser.error("ratios must be in (0, 1]")

    sources = sorted(
        os.path.join(args.src, name)
        for name in os.listdir(args.src)
        if name.lower().endswith(".nif")
    )
    if not sources:
        parser.error("no .nif files under %s" % args.src)

    if not args.dry_run:
        os.makedirs(args.out, exist_ok=True)

    ratios = (args.mid_ratio, args.far_ratio)
    totals = [0, 0, 0]
    failures = []

    for path in sources:
        stem = os.path.splitext(os.path.basename(path))[0]
        source_bytes = open(path, "rb").read()
        line = ["%-28s" % os.path.basename(path)]
        try:
            for (suffix, label), ratio in zip(TIERS, ratios):
                data, stats = make_lod(source_bytes, ratio, args.max_widen, args.min_triangles)
                verify(data, [reduced for _, reduced, _ in stats])
                source_tris = sum(original for original, _, _ in stats)
                lod_tris = sum(reduced for _, reduced, _ in stats)
                widen = max(widen for _, _, widen in stats)
                if label == "mid":
                    totals[0] += source_tris
                    totals[1] += lod_tris
                else:
                    totals[2] += lod_tris
                line.append("%s %4d->%-4d (%3d%%, x%.2f)" % (label, source_tris, lod_tris,
                                                             round(100.0 * lod_tris / max(source_tris, 1)), widen))
                if not args.dry_run:
                    with open(os.path.join(args.out, stem + suffix), "wb") as handle:
                        handle.write(data)
        except Exception as error:  # noqa: BLE001 - reported per file, the run continues
            failures.append((os.path.basename(path), error))
            line.append("FAILED: %s" % error)
        print("  ".join(line))

    print()
    print("source %d tris | mid %d (%d%%) | far %d (%d%%)" % (
        totals[0], totals[1], round(100.0 * totals[1] / max(totals[0], 1)),
        totals[2], round(100.0 * totals[2] / max(totals[0], 1))))
    if failures:
        print("%d file(s) failed:" % len(failures))
        for name, error in failures:
            print("  %s: %s" % (name, error))
        return 1
    print("%d file(s) -> %d LOD meshes in %s" % (len(sources), 2 * len(sources), args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
