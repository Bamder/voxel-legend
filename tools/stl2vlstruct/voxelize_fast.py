"""
快速 OBJ 体素化脚本
使用采样点方法快速体素化模型
"""
import struct
import math
import random
from pathlib import Path

# 方块 ID
BLOCK_AIR = 0
BLOCK_STONE = 3
BLOCK_COBBLE = 17
BLOCK_BRICK = 18
BLOCK_WOOD = 21
BLOCK_LOG = 6
BLOCK_METAL = 57
BLOCK_OBSIDIAN = 58


class OBJModel:
    def __init__(self):
        self.vertices = []
        self.normals = []
        self.faces = []

    def parse(self, filepath):
        """解析 OBJ 文件"""
        temp_vertices = []
        temp_normals = []

        with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith('#'):
                    continue

                parts = line.split()
                if not parts:
                    continue

                cmd = parts[0]

                if cmd == 'v':
                    try:
                        x, y, z = float(parts[1]), float(parts[2]), float(parts[3])
                        temp_vertices.append((x, y, z))
                    except:
                        pass

                elif cmd == 'vn':
                    try:
                        nx, ny, nz = float(parts[1]), float(parts[2]), float(parts[3])
                        temp_normals.append((nx, ny, nz))
                    except:
                        pass

                elif cmd == 'f':
                    face = []
                    for i in range(1, len(parts)):
                        indices = parts[i].split('/')
                        vi = int(indices[0]) - 1
                        face.append(vi)

                    if len(face) == 3:
                        self.faces.append(face)
                    elif len(face) > 3:
                        v0 = face[0]
                        for i in range(1, len(face) - 1):
                            self.faces.append([v0, face[i], face[i+1]])

        self.vertices = temp_vertices
        self.normals = temp_normals
        return len(self.faces) > 0

    def get_bounds(self):
        """获取模型边界"""
        if not self.vertices:
            return None

        xs = [v[0] for v in self.vertices]
        ys = [v[1] for v in self.vertices]
        zs = [v[2] for v in self.vertices]

        return {
            'min': (min(xs), min(ys), min(zs)),
            'max': (max(xs), max(ys), max(zs))
        }

    def point_in_triangle(self, p, v0, v1, v2):
        """判断点是否在三角形内（使用重心坐标）"""
        def sign(p1, p2, p3):
            return (p1[0] - p3[0]) * (p2[1] - p3[1]) - (p2[0] - p3[0]) * (p1[1] - p3[1])

        d1 = sign(p, v0, v1)
        d2 = sign(p, v1, v2)
        d3 = sign(p, v2, v0)

        has_neg = (d1 < 0) or (d2 < 0) or (d3 < 0)
        has_pos = (d1 > 0) or (d2 > 0) or (d3 > 0)

        return not (has_neg and has_pos)

    def triangle_bounding_box(self, v0, v1, v2):
        """获取三角形的轴对齐包围盒"""
        min_x = min(v0[0], v1[0], v2[0])
        max_x = max(v0[0], v1[0], v2[0])
        min_y = min(v0[1], v1[1], v2[1])
        max_y = max(v0[1], v1[1], v2[1])
        min_z = min(v0[2], v1[2], v2[2])
        max_z = max(v0[2], v1[2], v2[2])
        return (min_x, max_x, min_y, max_y, min_z, max_z)


def voxelize_model_fast(model, grid_size=32, flip_yz=True, samples_per_voxel=8):
    """
    快速体素化：使用采样点方法
    """
    bounds = model.get_bounds()
    if not bounds:
        return None

    min_x, min_y, min_z = bounds['min']
    max_x, max_y, max_z = bounds['max']

    size_x = max_x - min_x
    size_y = max_y - min_y
    size_z = max_z - min_z

    size_x = max(size_x, 0.001)
    size_y = max(size_y, 0.001)
    size_z = max(size_z, 0.001)

    # 体素网格尺寸
    sx = grid_size
    sy = int(grid_size * size_y / max(size_x, size_z)) + 1
    sz = grid_size

    # 创建体素网格
    grid = [[[False for _ in range(sz)] for _ in range(sy)] for _ in range(sx)]
    materials = [[[BLOCK_STONE for _ in range(sz)] for _ in range(sy)] for _ in range(sx)]

    # 对每个三角形
    for face in model.faces:
        if len(face) < 3:
            continue

        v0 = model.vertices[face[0]]
        v1 = model.vertices[face[1]]
        v2 = model.vertices[face[2]]

        # 如果需要交换 YZ
        if flip_yz:
            v0 = (v0[0], v0[2], v0[1])
            v1 = (v1[0], v1[2], v1[1])
            v2 = (v2[0], v2[2], v2[1])

        # 获取三角形包围盒
        bb = model.triangle_bounding_box(v0, v1, v2)

        # 计算需要检查的体素范围
        start_x = max(0, int((bb[0] - min_x) / size_x * sx))
        end_x = min(sx - 1, int((bb[1] - min_x) / size_x * sx) + 1)
        start_y = max(0, int((bb[2] - min_y) / size_y * sy))
        end_y = min(sy - 1, int((bb[3] - min_y) / size_y * sy) + 1)
        start_z = max(0, int((bb[4] - min_z) / size_z * sz))
        end_z = min(sz - 1, int((bb[5] - min_z) / size_z * sz) + 1)

        # 对每个体素
        for x in range(start_x, end_x + 1):
            for y in range(start_y, end_y + 1):
                for z in range(start_z, end_z + 1):
                    # 体素中心世界坐标
                    wx = min_x + (x + 0.5) / sx * size_x
                    wy = min_y + (y + 0.5) / sy * size_y
                    wz = min_z + (z + 0.5) / sz * size_z

                    # 检查点是否在三角形内
                    if model.point_in_triangle((wx, wy, wz), v0, v1, v2):
                        grid[x][y][z] = True

    return grid, materials, sx, sy, sz


def save_vlstruct(filepath, grid, materials, sx, sy, sz):
    """保存为 vlstruct 二进制格式"""
    with open(filepath, 'wb') as f:
        f.write(b'VLSTRUCT')
        f.write(struct.pack('<H', 1))
        f.write(struct.pack('<i', sx))
        f.write(struct.pack('<i', sy))
        f.write(struct.pack('<i', sz))

        for y in range(sy):
            for z in range(sz):
                for x in range(sx):
                    if grid[x][y][z]:
                        block_id = materials[x][y][z]
                    else:
                        block_id = BLOCK_AIR
                    f.write(struct.pack('B', block_id))


def main():
    # 转换配置
    CONVERSIONS = [
        # 房间
        ('Isometric room.obj', 'room_isometric.vlstruct', 40),
        ('japanese room.obj', 'room_japanese.vlstruct', 40),
        ('room.obj', 'room_basic.vlstruct', 32),
    ]

    base_dir = Path('C:/Users/ThinkBook/Desktop/voxel-legend-main/tools/raw_models')
    output_dir = Path('C:/Users/ThinkBook/Desktop/voxel-legend-main/assets/structures')

    print("=" * 60)
    print("快速 OBJ 体素化转换")
    print("=" * 60)

    for src_name, dst_name, size in CONVERSIONS:
        src_path = base_dir / src_name
        dst_path = output_dir / dst_name

        print(f"\n>>> 转换: {src_name} -> {dst_name}")

        if not src_path.exists():
            print(f"    [SKIP] 源文件不存在")
            continue

        # 解析 OBJ
        print(f"    解析 OBJ 文件...")
        model = OBJModel()
        if not model.parse(src_path):
            print(f"    [ERROR] OBJ 解析失败")
            continue

        bounds = model.get_bounds()
        print(f"    顶点数: {len(model.vertices)}, 面数: {len(model.faces)}")
        print(f"    边界: X[{bounds['min'][0]:.1f}~{bounds['max'][0]:.1f}], "
              f"Y[{bounds['min'][1]:.1f}~{bounds['max'][1]:.1f}], "
              f"Z[{bounds['min'][2]:.1f}~{bounds['max'][2]:.1f}]")

        # 体素化
        print(f"    体素化中 (尺寸={size})...")
        result = voxelize_model_fast(model, grid_size=size, flip_yz=True)

        if result is None:
            print(f"    [ERROR] 体素化失败")
            continue

        grid, materials, sx, sy, sz = result

        # 统计
        voxel_count = sum(1 for y in range(sy) for z in range(sz) for x in range(sx) if grid[x][y][z])
        print(f"    体素网格: {sx} x {sy} x {sz}, 填充体素: {voxel_count}")

        # 保存
        print(f"    保存中...")
        save_vlstruct(dst_path, grid, materials, sx, sy, sz)
        print(f"    [OK] 完成!")

    print("\n" + "=" * 60)
    print("转换完成!")
    print("=" * 60)


if __name__ == '__main__':
    main()
