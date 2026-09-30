# 三间资源房：中世纪英格兰房屋。
# 道具房 room_basic     都铎木构小屋（石基、密肋木架、出挑楼层、茅草双坡）
# 武器房 room_isometric 威尔登厅屋（两端出挑、中央凹进、陶瓦陡坡、大烟囱）
# 线索房 room_japanese  曲木茅屋（石勒脚、宽灰板、山墙曲木、侧烟囱）
#
# 参照：Geograph 上的 cruck / half-timbered 实拍，Wealden hall 与 jetty 的做法，
# 以及方块沙盒里「先石基、再深色木架、白灰填心、上檐出挑、陡坡加烟囱」的教程。
# 本引擎只有整格方块，坡屋顶用逐层退台，斜撑和曲木用折线。
#
# 方块编号必须与 src/world/blocks.hpp 末尾追加的顺序一致。
# 玩家高 1.80 世界单位，方块边长 0.5，约 3.6 格。门洞留 5 格（2.5 世界单位）。
# 战利品在房子中心附近生成（见 room_server：X 偏移 -2..+6，Z 偏移 -4..+4），
# 这片区域以及门外通道在 1..4 层必须是空气。

import os
import struct

AIR, GLASS, COAL = 0, 10, 12
PLANKS, COBBLE, BRICK = 16, 17, 18
TIMBER, PLASTER, THATCH, CLAY, ASHLAR = 57, 58, 59, 60, 61

OUT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "assets", "structures"))


def mix(x, y, z):
    return (x * 73856093 ^ y * 19349663 ^ z * 83492791) & 0xFFFFFFFF


def line(a0, b0, a1, b1):
    dx, dy = abs(a1 - a0), abs(b1 - b0)
    sx, sy = (1 if a0 < a1 else -1), (1 if b0 < b1 else -1)
    err = dx - dy
    a, b = a0, b0
    while True:
        yield a, b
        if a == a1 and b == b1:
            break
        e2 = 2 * err
        if e2 > -dy:
            err -= dy
            a += sx
        if e2 < dx:
            err += dx
            b += sy


class House:
    def __init__(self, sx, sz, sy=48):
        self.sx, self.sy, self.sz = sx, sy, sz
        self.b = bytearray(sx * sy * sz)
        self.ban = set()
        self.chimney = set()

    def _i(self, x, y, z):
        return (y * self.sz + z) * self.sx + x

    def inside(self, x, y, z):
        return 0 <= x < self.sx and 0 <= y < self.sy and 0 <= z < self.sz

    def get(self, x, y, z):
        if not self.inside(x, y, z):
            return None
        return self.b[self._i(x, y, z)]

    def set(self, x, y, z, v):
        if not self.inside(x, y, z):
            return
        self.b[self._i(x, y, z)] = v

    def force(self, x, y, z, v):
        if self.inside(x, y, z):
            self.b[self._i(x, y, z)] = v

    def stone(self, x, y, z):
        return COBBLE if mix(x, y, z) % 11 == 0 else ASHLAR

    def used_sy(self):
        span = self.sx * self.sz
        last = 0
        for y in range(self.sy):
            if any(self.b[y * span:(y + 1) * span]):
                last = y
        return last + 1


def foundation(h, wood_rect):
    x0, x1, z0, z1 = wood_rect
    for z in range(h.sz):
        for x in range(h.sx):
            if x0 < x < x1 and z0 < z < z1:
                h.set(x, 0, z, PLANKS)
            else:
                h.set(x, 0, z, h.stone(x, 0, z))


def frame_x(h, x, z0, z1, y0, y1, stud, braces=True):
    for z in range(z0, z1 + 1):
        for y in range(y0, y1 + 1):
            edge = z in (z0, z1) or y in (y0, y1)
            studded = (z - z0) % stud == 0
            rail = y == (y0 + y1) // 2
            h.set(x, y, z, TIMBER if (edge or studded or rail) else PLASTER)
    if not braces or stud < 2:
        return
    bay = 0
    z = z0
    while z + stud <= z1:
        if bay % 2 == 0:
            for a, b in line(z + 1, y0 + 1, z + stud - 1, y1 - 1):
                if z0 < a < z1 and y0 < b < y1 and h.get(x, b, a) == PLASTER:
                    h.set(x, b, a, TIMBER)
        bay += 1
        z += stud


def frame_z(h, z, x0, x1, y0, y1, stud, braces=True):
    for x in range(x0, x1 + 1):
        for y in range(y0, y1 + 1):
            edge = x in (x0, x1) or y in (y0, y1)
            studded = (x - x0) % stud == 0
            rail = y == (y0 + y1) // 2
            h.set(x, y, z, TIMBER if (edge or studded or rail) else PLASTER)
    if not braces or stud < 2:
        return
    bay = 0
    x = x0
    while x + stud <= x1:
        if bay % 2 == 0:
            for a, b in line(x + 1, y0 + 1, x + stud - 1, y1 - 1):
                if x0 < a < x1 and y0 < b < y1 and h.get(a, b, z) == PLASTER:
                    h.set(a, b, z, TIMBER)
        bay += 1
        x += stud


def stone_x(h, x, z0, z1, y0, y1):
    for z in range(z0, z1 + 1):
        for y in range(y0, y1 + 1):
            post = z in (z0, z1) or y == y1
            h.set(x, y, z, TIMBER if post else h.stone(x, y, z))


def stone_z(h, z, x0, x1, y0, y1):
    for x in range(x0, x1 + 1):
        for y in range(y0, y1 + 1):
            post = x in (x0, x1) or y == y1
            h.set(x, y, z, TIMBER if post else h.stone(x, y, z))


def window_x(h, x, z, y, w, ht):
    if h.get(x, y, z) in (None, AIR):
        return
    for dz in range(w):
        for dy in range(ht):
            h.force(x, y + dy, z + dz, GLASS)
    for dz in range(-1, w + 1):
        for dy in range(-1, ht + 1):
            if 0 <= dz < w and 0 <= dy < ht:
                continue
            if h.get(x, y + dy, z + dz) not in (None, AIR):
                h.set(x, y + dy, z + dz, TIMBER)


def window_z(h, z, x, y, w, ht):
    if h.get(x, y, z) in (None, AIR):
        return
    for dx in range(w):
        for dy in range(ht):
            h.force(x + dx, y + dy, z, GLASS)
    for dx in range(-1, w + 1):
        for dy in range(-1, ht + 1):
            if 0 <= dx < w and 0 <= dy < ht:
                continue
            if h.get(x + dx, y + dy, z) not in (None, AIR):
                h.set(x + dx, y + dy, z, TIMBER)


def door_x(h, x, z, y, w, ht):
    for dz in range(w):
        for dy in range(ht):
            h.force(x, y + dy, z + dz, AIR)
    for dz in range(-1, w + 1):
        for dy in range(ht + 1):
            on_leaf = 0 <= dz < w and dy < ht
            if on_leaf:
                continue
            if h.get(x, y + dy, z + dz) not in (None, AIR):
                h.set(x, y + dy, z + dz, TIMBER)


def door_z(h, z, x, y, w, ht):
    for dx in range(w):
        for dy in range(ht):
            h.force(x + dx, y + dy, z, AIR)
    for dx in range(-1, w + 1):
        for dy in range(ht + 1):
            if 0 <= dx < w and dy < ht:
                continue
            if h.get(x + dx, y + dy, z) not in (None, AIR):
                h.set(x + dx, y + dy, z, TIMBER)


def jetty_slab(h, x0, x1, z0, z1, y, gx0, gx1, gz0, gz1):
    for z in range(z0, z1 + 1):
        for x in range(x0, x1 + 1):
            overhang = x < gx0 or x > gx1 or z < gz0 or z > gz1
            rim = x in (x0, x1) or z in (z0, z1)
            h.set(x, y, z, TIMBER if (overhang or rim) else PLANKS)


def corbels_x(h, x, z0, z1, y, skip_z):
    z = z0
    while z <= z1:
        if z not in skip_z:
            h.set(x, y, z, TIMBER)
        z += 3


def roof_slope_x(h, y_eave, x0, x1, z0, z1, mat):
    """Ridge runs along Z. Eaves at x0 and x1. One-block overhang."""
    peak = (x1 - x0) // 2
    top = y_eave
    for k in range(peak + 1):
        y = y_eave + k
        top = y
        x_lo, x_hi = x0 + k, x1 - k
        if x_lo > x_hi:
            break
        xs = (x_lo,) if x_lo == x_hi else (x_lo, x_hi)
        for z in range(z0 - 1, z1 + 2):
            for x in xs:
                if (x, z) in h.chimney:
                    continue
                rafter = z0 < z < z1 and (z - z0) % 5 == 0
                h.set(x, y, z, TIMBER if rafter else mat)
            if z in (z0, z1) and x_lo < x_hi:
                for x in range(x_lo + 1, x_hi):
                    if (x, z) in h.chimney:
                        continue
                    if h.get(x, y, z) == AIR:
                        h.set(x, y, z, PLASTER)
    for z in range(z0 - 1, z1 + 2):
        for x in (x0 - 1, x1 + 1):
            if (x, z) not in h.chimney:
                h.set(x, y_eave, z, mat)
    return top


def roof_slope_z(h, y_eave, x0, x1, z0, z1, mat):
    """Ridge runs along X. Eaves at z0 and z1."""
    peak = (z1 - z0) // 2
    top = y_eave
    for k in range(peak + 1):
        y = y_eave + k
        top = y
        z_lo, z_hi = z0 + k, z1 - k
        if z_lo > z_hi:
            break
        zs = (z_lo,) if z_lo == z_hi else (z_lo, z_hi)
        for x in range(x0 - 1, x1 + 2):
            for z in zs:
                if (x, z) in h.chimney:
                    continue
                rafter = x0 < x < x1 and (x - x0) % 5 == 0
                h.set(x, y, z, TIMBER if rafter else mat)
            if x in (x0, x1) and z_lo < z_hi:
                for z in range(z_lo + 1, z_hi):
                    if (x, z) in h.chimney:
                        continue
                    if h.get(x, y, z) == AIR:
                        h.set(x, y, z, PLASTER)
    for x in range(x0 - 1, x1 + 2):
        for z in (z0 - 1, z1 + 1):
            if (x, z) not in h.chimney:
                h.set(x, y_eave, z, mat)
    return top


def king_gable_z(h, z, x0, x1, y_eave, y_peak):
    mid = (x0 + x1) // 2
    for y in range(y_eave, y_peak + 1):
        if h.get(mid, y, z) not in (None, AIR):
            h.set(mid, y, z, TIMBER)
    collar = y_eave + max(2, (y_peak - y_eave) // 3)
    half = max(2, (x1 - x0) // 4)
    for x in range(mid - half, mid + half + 1):
        if h.get(x, collar, z) not in (None, AIR):
            h.set(x, collar, z, TIMBER)


def cruck_gable_z(h, z, x0, x1, y_foot, y_peak):
    mid = (x0 + x1) // 2

    def blade(foot, apex, bow_x):
        n = max(8, (y_peak - y_foot) * 2)
        for i in range(n + 1):
            t = i / n
            u = 1.0 - t
            x = u * u * foot + 2 * u * t * bow_x + t * t * apex
            y = u * u * y_foot + 2 * u * t * (y_foot + (y_peak - y_foot) * 0.42) + t * t * y_peak
            xi, yi = int(round(x)), int(round(y))
            if h.get(xi, yi, z) not in (None, AIR):
                h.set(xi, yi, z, TIMBER)
            if h.get(xi, yi, z + (1 if z < h.sz // 2 else -1)) not in (None, AIR):
                h.set(xi, yi, z + (1 if z < h.sz // 2 else -1), TIMBER)

    blade(x0, mid, x0)
    blade(x1, mid, x1)
    collar = y_foot + (y_peak - y_foot) // 2
    for x in range(x0 + 2, x1 - 1):
        if h.get(x, collar, z) not in (None, AIR):
            h.set(x, collar, z, TIMBER)


def raise_chimney(h, x0, x1, z0, z1, y_top):
    for z in range(z0, z1 + 1):
        for x in range(x0, x1 + 1):
            h.chimney.add((x, z))
    # Filled after the roof punches through. Cap courses are brick.
    h.chimney_box = (x0, x1, z0, z1, y_top)


def stamp_chimney(h, y_peak):
    if not hasattr(h, "chimney_box"):
        return
    x0, x1, z0, z1, _ = h.chimney_box
    cap = y_peak + 3
    for y in range(1, cap + 1):
        for z in range(z0, z1 + 1):
            for x in range(x0, x1 + 1):
                mat = BRICK if y >= cap - 1 or mix(x, y, z) % 4 == 0 else ASHLAR
                h.set(x, y, z, mat)
    # Hearth coal on the floor, just outside the stack, if the path allows it.
    cx, cz = (x0 + x1) // 2, (z0 + z1) // 2
    for x, z in ((cx, z0 - 1), (cx, z1 + 1), (x0 - 1, cz), (x1 + 1, cz)):
        if (x, z) in h.ban or not h.inside(x, 1, z):
            continue
        if h.get(x, 1, z) in (AIR, None):
            h.set(x, 1, z, COAL)
            break


def ban_rect(h, x0, x1, z0, z1):
    for z in range(z0, z1 + 1):
        for x in range(x0, x1 + 1):
            h.ban.add((x, z))


def loot_box(sx, sz):
    cx, cz = sx // 2, sz // 2
    return cx - 2, cx + 6, cz - 4, cz + 4


def build_tudor():
    """Jetty cottage. Door faces west, toward the team spawn."""
    sx, sz = 29, 31
    h = House(sx, sz)
    lx0, lx1, lz0, lz1 = loot_box(sx, sz)
    ban_rect(h, lx0, lx1, lz0, lz1)
    # Ground wall, then the jettied upper wall two blocks outside it.
    gx0, gx1, gz0, gz1 = 6, 24, 6, 26
    ox0, ox1, oz0, oz1 = 4, 26, 4, 28
    ban_rect(h, 0, lx0 - 1, 14, 16)  # west porch, door, and the aisle up to the loot box
    foundation(h, (gx0, gx1, gz0, gz1))
    stone_x(h, gx0, gz0, gz1, 1, 2)
    stone_x(h, gx1, gz0, gz1, 1, 2)
    stone_z(h, gz0, gx0, gx1, 1, 2)
    stone_z(h, gz1, gx0, gx1, 1, 2)
    frame_x(h, gx0, gz0, gz1, 3, 5, stud=2)
    frame_x(h, gx1, gz0, gz1, 3, 5, stud=2)
    frame_z(h, gz0, gx0, gx1, 3, 5, stud=2)
    frame_z(h, gz1, gx0, gx1, 3, 5, stud=2)
    jetty_slab(h, ox0, ox1, oz0, oz1, 6, gx0, gx1, gz0, gz1)
    corbels_x(h, ox0, oz0, oz1, 5, set(range(13, 18)))
    frame_x(h, ox0, oz0, oz1, 7, 11, stud=2)
    frame_x(h, ox1, oz0, oz1, 7, 11, stud=2)
    frame_z(h, oz0, ox0, ox1, 7, 11, stud=2)
    frame_z(h, oz1, ox0, ox1, 7, 11, stud=2)
    door_x(h, gx0, 14, 1, 3, 5)
    window_x(h, gx0, 8, 3, 2, 2)
    window_x(h, gx0, 20, 3, 2, 2)
    window_x(h, ox0, 10, 8, 3, 3)
    window_x(h, ox0, 18, 8, 3, 3)
    window_z(h, oz0, 12, 8, 2, 3)
    window_z(h, oz1, 12, 8, 2, 3)
    raise_chimney(h, 21, 23, 21, 23, 0)
    top = roof_slope_x(h, 12, ox0, ox1, oz0, oz1, THATCH)
    stamp_chimney(h, top)
    king_gable_z(h, oz0, ox0, ox1, 12, top)
    king_gable_z(h, oz1, ox0, ox1, 12, top)
    # Sideboard against the back wall, clear of the loot box and the door.
    for x in range(8, 11):
        h.set(x, 1, 24, PLANKS)
    return h


def build_wealden():
    """Hall house. Door faces north, toward the team spawn. Clay-tile roof."""
    sx, sz = 37, 27
    h = House(sx, sz)
    lx0, lx1, lz0, lz1 = loot_box(sx, sz)
    ban_rect(h, lx0, lx1, lz0, lz1)
    ox0, ox1 = 2, 34
    z_jet, z_gnd, z_back = 2, 5, 22
    left = (2, 12)
    center = (13, 28)
    right = (29, 34)
    ban_rect(h, 20, 22, 0, lz0 - 1)  # north porch, door, and the aisle up to the loot box
    foundation(h, (ox0, ox1, z_gnd, z_back))
    # Straight stone ground floor. Upper end bays jetty forward of this line.
    stone_z(h, z_gnd, ox0, ox1, 1, 5)
    stone_z(h, z_back, ox0, ox1, 1, 5)
    stone_x(h, ox0, z_jet, z_back, 1, 5)
    stone_x(h, ox1, z_jet, z_back, 1, 5)
    for x in range(ox0, ox1 + 1):
        h.set(x, 6, z_gnd, TIMBER)
        h.set(x, 6, z_back, TIMBER)
    for z in range(z_jet, z_back + 1):
        h.set(ox0, 6, z, TIMBER)
        h.set(ox1, 6, z, TIMBER)
    for x0, x1 in (left, right):
        jetty_slab(h, x0, x1, z_jet, z_back, 6, x0, x1, z_gnd, z_back)
    for x0, x1 in (left, right):
        x = x0
        while x <= x1:
            h.set(x, 5, z_jet, TIMBER)
            x += 3
    frame_z(h, z_jet, left[0], left[1], 7, 11, stud=3)
    frame_z(h, z_jet, right[0], right[1], 7, 11, stud=3)
    frame_z(h, z_back, ox0, ox1, 7, 11, stud=3)
    frame_x(h, ox0, z_jet, z_back, 7, 11, stud=3)
    frame_x(h, ox1, z_jet, z_back, 7, 11, stud=3)
    # Cheeks closing the sides of each projecting bay.
    frame_x(h, left[1], z_jet, z_gnd, 7, 11, stud=3, braces=False)
    frame_x(h, right[0], z_jet, z_gnd, 7, 11, stud=3, braces=False)
    # Recessed center wall climbs into the slope. Eave is z_jet, so at z_gnd
    # the roof has already risen (z_gnd - z_jet) courses.
    k = z_gnd - z_jet
    frame_z(h, z_gnd, center[0], center[1], 7, 11 + k, stud=3)
    door_z(h, z_gnd, 20, 1, 3, 5)
    window_z(h, z_gnd, 8, 3, 2, 2)
    window_z(h, z_gnd, 30, 3, 2, 2)
    window_z(h, z_jet, 5, 8, 3, 3)
    window_z(h, z_jet, 30, 8, 3, 3)
    window_z(h, z_gnd, 18, 8, 4, 3)
    # Posts under the inner corners of the lofts, outside the loot box.
    for x, z in ((left[1], 7), (left[1], 20), (right[0], 7), (right[0], 20)):
        for y in range(1, 6):
            h.set(x, y, z, TIMBER)
    raise_chimney(h, 13, 15, 18, 20, 0)
    top = roof_slope_z(h, 12, ox0, ox1, z_jet, z_back, CLAY)
    stamp_chimney(h, top)
    # A plank chest in the service bay, away from the hall center.
    h.set(31, 1, 20, PLANKS)
    h.set(32, 1, 20, PLANKS)
    return h


def build_cruck():
    """Single-storey cruck cottage. Door faces east, toward the team spawn."""
    sx, sz = 25, 29
    h = House(sx, sz)
    lx0, lx1, lz0, lz1 = loot_box(sx, sz)
    ban_rect(h, lx0, lx1, lz0, lz1)
    x0, x1, z0, z1 = 3, 21, 3, 25
    ban_rect(h, lx1 + 1, sx - 1, 13, 15)
    foundation(h, (x0, x1, z0, z1))
    stone_x(h, x0, z0, z1, 1, 2)
    stone_x(h, x1, z0, z1, 1, 2)
    stone_z(h, z0, x0, x1, 1, 2)
    stone_z(h, z1, x0, x1, 1, 2)
    frame_x(h, x0, z0, z1, 3, 6, stud=4)
    frame_x(h, x1, z0, z1, 3, 6, stud=4)
    frame_z(h, z0, x0, x1, 3, 6, stud=4, braces=False)
    frame_z(h, z1, x0, x1, 3, 6, stud=4, braces=False)
    door_x(h, x1, 13, 1, 3, 5)
    window_x(h, x1, 7, 3, 2, 2)
    window_x(h, x1, 17, 3, 2, 2)
    window_z(h, z0, 10, 3, 2, 2)
    window_z(h, z1, 10, 3, 2, 2)
    raise_chimney(h, 5, 7, 21, 23, 0)
    top = roof_slope_x(h, 7, x0, x1, z0, z1, THATCH)
    stamp_chimney(h, top)
    cruck_gable_z(h, z0, x0, x1, 2, top)
    cruck_gable_z(h, z1, x0, x1, 2, top)
    h.set(6, 1, 6, PLANKS)
    h.set(7, 1, 6, PLANKS)
    return h


def check(h, name):
    sx, sz = h.sx, h.sz
    lx0, lx1, lz0, lz1 = loot_box(sx, sz)
    sy = h.used_sy()
    assert 1 <= sy <= 64 and sx <= 64 and sz <= 64, name
    for z in range(sz):
        for x in range(sx):
            assert h.get(x, 0, z) != AIR, f"{name} hole in foundation {(x, z)}"
    for x, z in h.ban:
        for y in range(1, 5):
            assert h.get(x, y, z) == AIR, f"{name} blocked reserved {(x, y, z)}={h.get(x, y, z)}"
    # The loot box has to sit inside the walls, not out on the apron.
    assert h.get(lx0, 2, lz0) == AIR and h.get(lx1, 2, lz1) == AIR, name
    counts = {}
    for v in h.b[:sx * sy * sz]:
        if v:
            counts[v] = counts.get(v, 0) + 1
    assert counts.get(TIMBER, 0) > 80, name
    assert counts.get(PLASTER, 0) > 40, name
    roof = counts.get(THATCH, 0) + counts.get(CLAY, 0)
    assert roof > 80, name
    assert any(y > 12 and h.get(sx // 2, y, sz // 2) != AIR for y in range(sy)), name
    return sy, counts


GLYPH = {
    AIR: ".", GLASS: "G", COAL: "#", PLANKS: "p", COBBLE: "c", BRICK: "B",
    TIMBER: "T", PLASTER: "P", THATCH: "H", CLAY: "C", ASHLAR: "A",
}


def elev_from(h, axis, reverse=False):
    sy = h.used_sy()
    rows = []
    if axis == "x":
        xs = range(h.sx - 1, -1, -1) if reverse else range(h.sx)
        for y in range(sy - 1, -1, -1):
            row = []
            for z in range(h.sz):
                ch = "."
                for x in xs:
                    v = h.get(x, y, z)
                    if v:
                        ch = GLYPH.get(v, "?")
                        break
                row.append(ch)
            rows.append("".join(row))
    else:
        zs = range(h.sz - 1, -1, -1) if reverse else range(h.sz)
        for y in range(sy - 1, -1, -1):
            row = []
            for x in range(h.sx):
                ch = "."
                for z in zs:
                    v = h.get(x, y, z)
                    if v:
                        ch = GLYPH.get(v, "?")
                        break
                row.append(ch)
            rows.append("".join(row))
    return "\n".join(rows)


def save(path, h):
    sy = h.used_sy()
    payload = bytes(h.b[:h.sx * sy * h.sz])
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"VLSTRUCT")
        f.write(struct.pack("<Hiii", 1, h.sx, sy, h.sz))
        f.write(payload)


def main():
    houses = (
        ("room_basic", build_tudor, "x", False),
        ("room_isometric", build_wealden, "z", False),
        ("room_japanese", build_cruck, "x", True),
    )
    for name, fn, axis, rev in houses:
        h = fn()
        sy, counts = check(h, name)
        path = os.path.join(OUT, name + ".vlstruct")
        save(path, h)
        print(f"{name}: {h.sx}x{sy}x{h.sz} blocks={sum(counts.values())}")
        print(elev_from(h, axis, rev))
        print()


if __name__ == "__main__":
    main()
