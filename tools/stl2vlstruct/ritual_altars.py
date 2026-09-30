# 古欧洲风格祭祀台生成脚本
# 参考：古罗马石祭坛、异教祭祀台、中世纪教堂祭坛设计
#
# 设计理念：
# 1. 使用方石(ASHLAR)、鹅卵石(COBBLE)、砖块(BRICK)建造石质结构
# 2. 四角放置火把(TORCH)，中央可放置提灯(LANTERN)
# 3. 台阶式基座，逐层上升
# 4. 边缘装饰和柱体元素
# 5. 保持中央区域空旷以便放置物品

import os
import struct
import random

# 方块类型
AIR = 0
ASHLAR = 61      # 方石 - 主要建筑材料
COBBLE = 17      # 鹅卵石 - 地面/基座
BRICK = 18       # 砖块 - 装饰/台阶
TIMBER = 57      # 木材 - 框架/柱体
PLANKS = 16      # 木板 - 辅助
TORCH = 63       # 火把 - 照明
LANTERN = 64     # 提灯 - 主光源

OUT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "assets", "structures"))


class Altar:
    def __init__(self, sx, sy, sz):
        self.sx = sx
        self.sy = sy
        self.sz = sz
        self.b = bytearray(sx * sy * sz)

    def _i(self, x, y, z):
        return (y * self.sz + z) * self.sx + x

    def inside(self, x, y, z):
        return 0 <= x < self.sx and 0 <= y < self.sy and 0 <= z < self.sz

    def get(self, x, y, z):
        if not self.inside(x, y, z):
            return None
        return self.b[self._i(x, y, z)]

    def set(self, x, y, z, v):
        if self.inside(x, y, z):
            self.b[self._i(x, y, z)] = v

    def used_sy(self):
        span = self.sx * self.sz
        for y in range(self.sy - 1, -1, -1):
            if any(self.b[y * span:(y + 1) * span]):
                return y + 1
        return 1

    def stone_variation(self, x, y, z):
        """生成石块纹理变化"""
        h = (x * 31 + y * 17 + z * 41) % 10
        if h < 7:
            return ASHLAR
        elif h < 9:
            return COBBLE
        else:
            return BRICK


def rect(altar, x0, x1, y, z0, z1, block):
    """填充矩形区域"""
    for z in range(z0, z1 + 1):
        for x in range(x0, x1 + 1):
            altar.set(x, y, z, block)


def platform(altar, y, x0, x1, z0, z1, block, hollow=False, wall=1):
    """创建平台/台阶，带墙壁厚度"""
    for z in range(z0, z1 + 1):
        for x in range(x0, x1 + 1):
            is_edge = x in (x0, x1) or z in (z0, z1)
            if hollow and not is_edge:
                if altar.get(x, y, z) == AIR:
                    altar.set(x, y, z, AIR)
            else:
                if is_edge or wall == 0:
                    altar.set(x, y, z, block)


def pillar(altar, x, z, y0, y1, block):
    """创建圆柱/柱子"""
    for y in range(y0, y1 + 1):
        altar.set(x, y, z, block)


def torch_at(altar, x, y, z):
    """放置火把"""
    altar.set(x, y, z, TORCH)


def lantern_at(altar, x, y, z):
    """放置提灯"""
    altar.set(x, y, z, LANTERN)


# ============ 6种古欧洲风格祭祀台 ============

def build_element_altar():
    """
    元素祭祀台 - 基础古罗马风格
    特点：方石基座、四角火把、中央祭坛石
    """
    sx, sz = 21, 21
    sy = 8
    a = Altar(sx, sy, sz)
    cx, cz = sx // 2, sz // 2

    # 第一层：宽阔石质基座
    rect(a, 0, sx-1, 0, 0, sz-1, COBBLE)
    rect(a, 1, sx-2, 1, 1, sz-2, ASHLAR)

    # 第二层：装饰边缘
    rect(a, 2, sx-3, 2, 2, sz-3, ASHLAR)

    # 第三层：台阶
    rect(a, 3, sx-4, 3, 3, sz-4, BRICK)

    # 第四层：主体基座
    rect(a, 4, sx-5, 4, 4, sz-5, ASHLAR)

    # 第五层：祭坛主体
    rect(a, 5, sx-6, 5, 5, sz-6, ASHLAR)
    rect(a, 6, sx-7, 5, 6, sz-7, ASHLAR)  # 中心平台

    # 第六层：中央祭坛石
    rect(a, 8, 12, 6, 8, 12, ASHLAR)

    # 四角火把
    torch_at(a, 5, 6, 5)
    torch_at(a, sx-6, 6, 5)
    torch_at(a, 5, 6, sz-6)
    torch_at(a, sx-6, 6, sz-6)

    # 边缘装饰火把
    torch_at(a, cx, 5, 2)
    torch_at(a, cx, 5, sz-3)
    torch_at(a, 2, 5, cz)
    torch_at(a, sx-3, 5, cz)

    # 角落小柱子
    for dx, dz in [(4,4), (sx-5,4), (4,sz-5), (sx-5,sz-5)]:
        pillar(a, dx, dz, 3, 6, TIMBER)

    return a


def build_god_altar():
    """
    神祇祭祀台 - 庄严肃穆的希腊神庙风格
    特点：多级台阶、柱廊入口、中央圣火
    """
    sx, sz = 23, 25
    sy = 10
    a = Altar(sx, sy, sz)
    cx, cz = sx // 2, sz // 2

    # 底部宽阔基座
    rect(a, 0, sx-1, 0, 0, sz-1, COBBLE)
    rect(a, 1, sx-2, 1, 1, sz-2, ASHLAR)

    # 第二层台阶
    rect(a, 2, sx-3, 2, 2, sz-3, BRICK)

    # 第三层台阶
    rect(a, 3, sx-4, 3, 3, sz-4, ASHLAR)

    # 第四层 - 主祭坛平台
    rect(a, 4, sx-5, 4, 4, sz-5, ASHLAR)

    # 祭坛主体 - 前方开放
    rect(a, 5, sx-6, 5, 5, 10, ASHLAR)  # 后墙
    rect(a, 5, sx-6, 5, sz-11, sz-6, ASHLAR)  # 前墙
    rect(a, 5, 6, 5, 10, sz-11, ASHLAR)  # 左墙
    rect(a, sx-7, sx-6, 5, 10, sz-11, ASHLAR)  # 右墙

    # 内部空心
    for z in range(10, sz-11):
        for x in range(6, sx-6):
            a.set(x, 5, z, AIR)

    # 中央祭坛石
    rect(a, 9, 13, 5, 9, 15, ASHLAR)

    # 台阶通往中央
    rect(a, 9, 13, 4, 10, 14, BRICK)

    # 四角柱
    for dx, dz in [(5,5), (sx-6,5), (5,sz-6), (sx-6,sz-6)]:
        for y in range(4, 9):
            pillar(a, dx, dz, y, y, TIMBER)

    # 前方两根装饰柱
    pillar(a, 8, 10, 4, 8, TIMBER)
    pillar(a, 14, 10, 4, 8, TIMBER)

    # 火把 - 入口两侧
    torch_at(a, 7, 6, 10)
    torch_at(a, 15, 6, 10)

    # 四角火把
    torch_at(a, 5, 7, 5)
    torch_at(a, sx-6, 7, 5)
    torch_at(a, 5, 7, sz-6)
    torch_at(a, sx-6, 7, sz-6)

    # 中央提灯
    lantern_at(a, 11, 7, 12)

    return a


def build_old_god_altar():
    """
    旧神祭祀台 - 古老德鲁伊石环风格
    特点：粗糙石块、环形排列、蜡烛阵列
    """
    sx, sz = 23, 23
    sy = 6
    a = Altar(sx, sy, sz)
    cx, cz = sx // 2, sz // 2

    # 地面
    rect(a, 0, sx-1, 0, 0, sz-1, COBBLE)

    # 中央圆形石台（近似）
    for r in range(6, 0, -1):
        for z in range(sz):
            for x in range(sx):
                dist = ((x-cx)**2 + (z-cz)**2) ** 0.5
                if abs(dist - r) < 1.5:
                    y = 1 if r >= 3 else 2
                    a.set(x, y, z, COBBLE if r % 2 == 0 else ASHLAR)

    # 外圈大石
    for angle in range(0, 360, 30):
        import math
        rad = math.radians(angle)
        x = int(cx + 9 * math.cos(rad))
        z = int(cz + 9 * math.sin(rad))
        a.set(x, 1, z, COBBLE)
        a.set(x, 2, z, COBBLE)

    # 中央祭坛
    rect(a, cx-1, cx+1, 3, cz-1, cz+1, ASHLAR)

    # 蜡烛/火把阵列
    for angle in range(0, 360, 45):
        import math
        rad = math.radians(angle)
        x = int(cx + 5 * math.cos(rad))
        z = int(cz + 5 * math.sin(rad))
        torch_at(a, x, 2, z)

    # 内圈火把
    for angle in range(22, 360, 45):
        import math
        rad = math.radians(angle)
        x = int(cx + 3 * math.cos(rad))
        z = int(cz + 3 * math.sin(rad))
        torch_at(a, x, 2, z)

    # 入口标记石
    rect(a, cx-2, cx-1, 1, cz-9, cz-8, COBBLE)
    rect(a, cx+1, cx+2, 1, cz-9, cz-8, COBBLE)

    return a


def build_outer_altar():
    """
    外围祭祀台 - 开放祭坛风格
    特点：低矮石墙、广泛火把阵、中央圣坛
    """
    sx, sz = 25, 25
    sy = 6
    a = Altar(sx, sy, sz)
    cx, cz = sx // 2, sz // 2

    # 基座
    rect(a, 0, sx-1, 0, 0, sz-1, COBBLE)
    rect(a, 1, sx-2, 1, 1, sz-2, ASHLAR)

    # 中央平台
    rect(a, 7, 17, 2, 7, 17, ASHLAR)

    # 外围低墙
    rect(a, 3, 3, 2, 3, sz-4, ASHLAR)  # 左墙
    rect(a, sx-4, sx-4, 2, 3, sz-4, ASHLAR)  # 右墙
    rect(a, 3, sx-4, 2, 3, 3, ASHLAR)  # 前墙
    rect(a, 3, sx-4, 2, sz-4, sz-4, ASHLAR)  # 后墙

    # 四角矮柱
    for dx, dz in [(3,3), (sx-4,3), (3,sz-4), (sx-4,sz-4)]:
        pillar(a, dx, dz, 2, 4, TIMBER)

    # 中央祭坛石
    rect(a, 10, 14, 3, 10, 14, ASHLAR)
    rect(a, 11, 13, 4, 11, 13, ASHLAR)

    # 火把阵 - 双圈
    # 内圈
    for dx, dz in [(cx, cz-4), (cx, cz+4), (cx-4, cz), (cx+4, cz)]:
        torch_at(a, dx, 3, dz)

    # 外圈
    for angle in range(0, 360, 45):
        import math
        rad = math.radians(angle)
        x = int(cx + 8 * math.cos(rad))
        z = int(cz + 8 * math.sin(rad))
        torch_at(a, x, 2, z)

    # 中央提灯
    lantern_at(a, cx, 5, cz)

    # 门道标记
    for x in range(10, 15):
        a.set(x, 2, 3, AIR)
        a.set(x, 3, 3, AIR)

    return a


def build_time_altar():
    """
    时间祭祀台 - 日晷风格
    特点：圆形基座、中央指针、四季火把
    """
    sx, sz = 23, 23
    sy = 7
    a = Altar(sx, sy, sz)
    cx, cz = sx // 2, sz // 2

    # 基座
    rect(a, 0, sx-1, 0, 0, sz-1, COBBLE)
    rect(a, 2, sx-3, 1, 2, sz-3, ASHLAR)

    # 圆形主平台
    for z in range(sz):
        for x in range(sx):
            dist = ((x-cx)**2 + (z-cz)**2) ** 0.5
            if dist < 9:
                a.set(x, 2, z, ASHLAR)

    # 内圈装饰
    for z in range(sz):
        for x in range(sx):
            dist = ((x-cx)**2 + (z-cz)**2) ** 0.5
            if 4 < dist < 5:
                a.set(x, 2, z, BRICK)

    # 中央柱基
    rect(a, 9, 13, 3, 9, 13, ASHLAR)

    # 中央时间柱
    for y in range(3, 6):
        a.set(cx, y, cz, TIMBER)

    # 顶部石球/日晷指针
    a.set(cx, 6, cz, ASHLAR)

    # 四方向火把（象征四季）
    torch_at(a, cx, 4, cz-7)  # 北
    torch_at(a, cx, 4, cz+7)  # 南
    torch_at(a, cx-7, 4, cz)  # 西
    torch_at(a, cx+7, 4, cz)  # 东

    # 角落小石柱
    for angle in range(45, 360, 90):
        import math
        rad = math.radians(angle)
        x = int(cx + 8 * math.cos(rad))
        z = int(cz + 8 * math.sin(rad))
        pillar(a, x, z, 2, 3, COBBLE)

    # 中间圈火把
    for angle in range(0, 360, 30):
        import math
        rad = math.radians(angle)
        x = int(cx + 5 * math.cos(rad))
        z = int(cz + 5 * math.sin(rad))
        torch_at(a, x, 3, z)

    return a


def build_worldtree_altar():
    """
    世界树祭祀台 - 北欧风格
    特点：中央支柱、环形提灯、树根装饰
    """
    sx, sz = 25, 25
    sy = 12
    a = Altar(sx, sy, sz)
    cx, cz = sx // 2, sz // 2

    # 基座
    rect(a, 0, sx-1, 0, 0, sz-1, COBBLE)
    rect(a, 1, sx-2, 1, 1, sz-2, ASHLAR)

    # 石质环形基座
    for z in range(sz):
        for x in range(sx):
            dist = ((x-cx)**2 + (z-cz)**2) ** 0.5
            if 5 < dist < 8:
                a.set(x, 2, z, ASHLAR)

    # 第二层
    rect(a, 6, 18, 3, 6, 18, ASHLAR)

    # 中央支柱基座
    rect(a, 10, 14, 4, 10, 14, BRICK)

    # 世界树主干
    for y in range(4, 11):
        a.set(cx, y, cz, TIMBER)
        # 树干厚度
        if y < 8:
            a.set(cx-1, y, cz, TIMBER)
            a.set(cx+1, y, cz, TIMBER)
            a.set(cx, y, cz-1, TIMBER)
            a.set(cx, y, cz+1, TIMBER)

    # 树冠平台
    rect(a, 9, 15, 10, 9, 15, ASHLAR)
    rect(a, 10, 14, 11, 10, 14, PLANKS)

    # 提灯环绕（生命之光）
    for angle in range(0, 360, 60):
        import math
        rad = math.radians(angle)
        x = int(cx + 7 * math.cos(rad))
        z = int(cz + 7 * math.sin(rad))
        lantern_at(a, x, 4, z)

    # 外圈火把
    for angle in range(30, 360, 45):
        import math
        rad = math.radians(angle)
        x = int(cx + 10 * math.cos(rad))
        z = int(cz + 10 * math.sin(rad))
        torch_at(a, x, 3, z)

    # 树根装饰（角落石块）
    for dx, dz in [(6,6), (sx-7,6), (6,sz-7), (sx-7,sz-7)]:
        pillar(a, dx, dz, 2, 3, COBBLE)
        torch_at(a, dx, 4, dz)

    # 顶部装饰
    a.set(cx, 11, cz, ASHLAR)

    return a


def save(path, a):
    """保存为VLSTRUCT格式"""
    sy = a.used_sy()
    payload = bytes(a.b[:a.sx * sy * a.sz])
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"VLSTRUCT")
        f.write(struct.pack("<Hiii", 1, a.sx, sy, a.sz))
        f.write(payload)


def main():
    altars = [
        ("ritual_element", build_element_altar, "元素祭祀台 - 古罗马基础风格"),
        ("ritual_god", build_god_altar, "神祇祭祀台 - 希腊神庙风格"),
        ("ritual_old_god", build_old_god_altar, "旧神祭祀台 - 德鲁伊石环风格"),
        ("ritual_outer", build_outer_altar, "外围祭祀台 - 开放祭坛风格"),
        ("ritual_time", build_time_altar, "时间祭祀台 - 日晷风格"),
        ("ritual_worldtree", build_worldtree_altar, "世界树祭祀台 - 北欧风格"),
    ]

    for name, fn, desc in altars:
        a = fn()
        sy = a.used_sy()
        path = os.path.join(OUT, name + ".vlstruct")
        save(path, a)

        # 统计方块
        counts = {}
        for v in a.b[:a.sx * sy * a.sz]:
            if v:
                counts[v] = counts.get(v, 0) + 1

        print(f"{name}: {a.sx}x{sy}x{a.sz}")
        print(f"  描述: {desc}")
        print(f"  方块统计: {dict((k, v) for k, v in counts.items() if v > 0)}")
        print()


if __name__ == "__main__":
    main()
