# 英国巨石阵风格建筑生成脚本

import os
import struct

# 方块类型
AIR = 0
COBBLE = 17      # 鹅卵石 - 主要材料
ASHLAR = 61      # 方石 - 装饰

OUT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "assets", "structures"))

class Structure:
    def __init__(self, sx, sy, sz):
        self.sx = sx
        self.sy = sy
        self.sz = sz
        self.b = bytearray(sx * sy * sz)

    def _i(self, x, y, z):
        return (y * self.sz + z) * self.sx + x

    def inside(self, x, y, z):
        return 0 <= x < self.sx and 0 <= y < self.sy and 0 <= z < self.sz

    def set(self, x, y, z, v):
        if self.inside(x, y, z):
            self.b[self._i(x, y, z)] = v

    def get(self, x, y, z):
        if not self.inside(x, y, z):
            return None
        return self.b[self._i(x, y, z)]

    def used_sy(self):
        span = self.sx * self.sz
        for y in range(self.sy - 1, -1, -1):
            if any(self.b[y * span:(y + 1) * span]):
                return y + 1
        return 1


def rect(s, x0, x1, y, z0, z1, block):
    """填充矩形区域"""
    for z in range(z0, z1 + 1):
        for x in range(x0, x1 + 1):
            s.set(x, y, z, block)


def pillar(s, x, z, y0, y1, block, width=1):
    """创建石柱"""
    for y in range(y0, y1 + 1):
        for dx in range(width):
            for dz in range(width):
                s.set(x + dx, y, z + dz, block)


def stone_at(s, x, y, z, height, block, width=1):
    """放置一块巨石"""
    for h in range(height):
        for dx in range(width):
            for dz in range(width):
                s.set(x + dx, y + h, z + dz, block)


def build_stonehenge():
    """
    英国巨石阵风格建筑
    - 外圈：30块直立巨石围成环
    - 内圈：20块稍矮的巨石
    - 中央：5块立石（呈马蹄形）
    - 部分横梁连接
    """
    import math
    
    sx, sz = 41, 41
    sy = 14
    s = Structure(sx, sy, sz)
    cx, cz = sx // 2, sz // 2

    # 地面平台
    rect(s, 0, sx-1, 0, 0, sz-1, COBBLE)

    # ========== 外圈巨石（约30块）==========
    outer_radius = 16
    outer_count = 30
    outer_height = 8
    for i in range(outer_count):
        angle = 2 * math.pi * i / outer_count
        # 调整位置让入口朝南（-z方向）
        x = int(cx + outer_radius * math.sin(angle))
        z = int(cz + outer_radius * math.cos(angle))
        stone_at(s, x, 1, z, outer_height, ASHLAR, width=1)

    # ========== 内圈巨石（约20块）==========
    inner_radius = 11
    inner_count = 20
    inner_height = 6
    for i in range(inner_count):
        angle = 2 * math.pi * i / inner_count
        x = int(cx + inner_radius * math.sin(angle))
        z = int(cz + inner_radius * math.cos(angle))
        stone_at(s, x, 1, z, inner_height, ASHLAR, width=1)

    # ========== 马蹄形中央石群（5块）==========
    # 中间3块立石
    stone_at(s, cx, 1, cz - 4, 10, ASHLAR)  # 中心主石
    stone_at(s, cx - 3, 1, cz - 3, 9, ASHLAR)  # 左石
    stone_at(s, cx + 3, 1, cz - 3, 9, ASHLAR)  # 右石
    
    # 两块更矮的石（马蹄形两端）
    stone_at(s, cx - 5, 1, cz - 1, 6, ASHLAR)
    stone_at(s, cx + 5, 1, cz - 1, 6, ASHLAR)

    # ========== 横梁连接外圈部分石柱 ==========
    # 顶部横梁（连接外圈相邻石柱）
    for i in range(0, outer_count, 5):  # 每隔5块加一个横梁
        angle1 = 2 * math.pi * i / outer_count
        angle2 = 2 * math.pi * (i + 1) / outer_count
        
        x1 = int(cx + outer_radius * math.sin(angle1))
        z1 = int(cz + outer_radius * math.cos(angle1))
        x2 = int(cx + outer_radius * math.sin(angle2))
        z2 = int(cz + outer_radius * math.cos(angle2))
        
        # 在顶部放置横梁
        top_y = outer_height + 1
        
        # 简化：只在南边（入口方向）加横梁
        if i >= 12 and i <= 18:  # 南边区域
            mid_x = (x1 + x2) // 2
            mid_z = (z1 + z2) // 2
            stone_at(s, mid_x, top_y, mid_z, 1, ASHLAR)

    # ========== 入口标记石 ==========
    # 入口两侧的立石
    stone_at(s, cx - 3, 1, cz + 17, 7, ASHLAR)
    stone_at(s, cx + 3, 1, cz + 17, 7, ASHLAR)
    
    # 入口横梁
    rect(s, cx - 3, cx + 3, 8, cz + 17, cz + 17, ASHLAR)

    return s


def save(path, s):
    """保存为VLSTRUCT格式"""
    sy = s.used_sy()
    payload = bytes(s.b[:s.sx * sy * s.sz])
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"VLSTRUCT")
        f.write(struct.pack("<Hiii", 1, s.sx, sy, s.sz))
        f.write(payload)


def main():
    stonehenge = build_stonehenge()
    sy = stonehenge.used_sy()
    
    path = os.path.join(OUT, "stonehenge.vlstruct")
    save(path, stonehenge)
    
    # 统计方块
    counts = {}
    for v in stonehenge.b[:stonehenge.sx * sy * stonehenge.sz]:
        if v:
            counts[v] = counts.get(v, 0) + 1
    
    print(f"stonehenge: {stonehenge.sx}x{sy}x{stonehenge.sz}")
    print(f"  描述: 英国巨石阵风格建筑")
    print(f"  方块统计: {dict((k, v) for k, v in counts.items() if v > 0)}")
    print(f"  已保存到: {path}")


if __name__ == "__main__":
    main()
