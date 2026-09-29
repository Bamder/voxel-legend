"""
改进的 OBJ 体素化脚本
使用射线投射法正确处理 OBJ 面，生成体素结构
"""
import struct
import math
from pathlib import Path
from collections import defaultdict

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
        self.faces = []  # (v1, v2, v3, vn1, vn2, vn3)
    
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
                    # 解析面: f v1/vt1/vn1 v2/vt2/vn2 v3/vt3/vn3 ...
                    face = []
                    face_normals = []
                    for i in range(1, len(parts)):
                        indices = parts[i].split('/')
                        vi = int(indices[0]) - 1  # OBJ 索引从 1 开始
                        face.append(vi)
                        
                        # 法向量索引（如果有）
                        if len(indices) >= 3 and indices[2]:
                            ni = int(indices[2]) - 1
                            if 0 <= ni < len(temp_normals):
                                face_normals.append(temp_normals[ni])
                    
                    # 三角化（如果面超过 3 个顶点）
                    if len(face) == 3:
                        self.faces.append(face + face_normals[:3] if face_normals else face + [(0,1,0)]*3)
                    elif len(face) > 3:
                        # 扇形三角化
                        v0 = face[0]
                        n0 = face_normals[0] if face_normals else (0, 1, 0)
                        for i in range(1, len(face) - 1):
                            self.faces.append([v0, face[i], face[i+1]] + [n0, face_normals[i] if face_normals else (0,1,0), face_normals[i+1] if face_normals else (0,1,0)])
        
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
    
    def ray_triangle_intersect(self, origin, direction, v0, v1, v2):
        """射线-三角形相交检测"""
        epsilon = 1e-6
        
        edge1 = (v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2])
        edge2 = (v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2])
        
        hx = direction[1] * edge2[2] - direction[2] * edge2[1]
        hy = direction[2] * edge2[0] - direction[0] * edge2[2]
        hz = direction[0] * edge2[1] - direction[1] * edge2[0]
        
        a = edge1[0] * hx + edge1[1] * hy + edge1[2] * hz
        
        if abs(a) < epsilon:
            return None
        
        f = 1.0 / a
        s = (origin[0] - v0[0], origin[1] - v0[1], origin[2] - v0[2])
        u = f * (s[0] * hx + s[1] * hy + s[2] * hz)
        
        if u < 0.0 or u > 1.0:
            return None
        
        qx = s[1] * edge1[2] - s[2] * edge1[1]
        qy = s[2] * edge1[0] - s[0] * edge1[2]
        qz = s[0] * edge1[1] - s[1] * edge1[0]
        v = f * (direction[0] * qx + direction[1] * qy + direction[2] * qz)
        
        if v < 0.0 or u + v > 1.0:
            return None
        
        t = f * (edge2[0] * qx + edge2[1] * qy + edge2[2] * qz)
        
        if t > epsilon:
            return t
        
        return None


def voxelize_model(model, target_size=32, flip_yz=True):
    """
    使用射线投射法体素化模型
    
    Args:
        model: OBJModel 实例
        target_size: 目标体素网格大小
        flip_yz: 是否交换 Y 和 Z 轴
    """
    bounds = model.get_bounds()
    if not bounds:
        return None
    
    # 获取边界
    min_x, min_y, min_z = bounds['min']
    max_x, max_y, max_z = bounds['max']
    
    # 模型尺寸
    size_x = max_x - min_x
    size_y = max_y - min_y
    size_z = max_z - min_z
    
    # 避免除零
    size_x = max(size_x, 0.001)
    size_y = max(size_y, 0.001)
    size_z = max(size_z, 0.001)
    
    # 计算缩放比例
    max_dim = max(size_x, size_y, size_z)
    scale = (target_size - 2) / max_dim
    
    # 体素网格尺寸
    sx = max(1, min(64, int(size_x * scale) + 2))
    sy = max(1, min(64, int(size_y * scale) + 2))
    sz = max(1, min(64, int(size_z * scale) + 2))
    
    # 创建体素网格
    grid = [[[False for _ in range(sz)] for _ in range(sy)] for _ in range(sx)]
    materials = [[[BLOCK_STONE for _ in range(sz)] for _ in range(sy)] for _ in range(sx)]
    
    # 射线投射：从六个方向投射射线
    directions = [
        (1, 0, 0), (-1, 0, 0),
        (0, 1, 0), (0, -1, 0),
        (0, 0, 1), (0, 0, -1)
    ]
    
    def voxel_to_world(vx, vy, vz):
        """体素坐标转世界坐标"""
        wx = min_x + (vx / (sx - 1)) * size_x if flip_yz else min_x + (vx / (sx - 1)) * size_x
        wy = min_y + (vy / (sy - 1)) * size_y
        wz = min_z + (vz / (sz - 1)) * size_z if flip_yz else min_z + (vz / (sz - 1)) * size_z
        return (wx, wy, wz)
    
    def world_to_voxel(wx, wy, wz):
        """世界坐标转体素坐标"""
        vx = int((wx - min_x) / size_x * (sx - 1) + 0.5)
        vy = int((wy - min_y) / size_y * (sy - 1) + 0.5)
        vz = int((wz - min_z) / size_z * (sz - 1) + 0.5)
        
        if flip_yz:
            # 交换 Y 和 Z
            return (vx, vz, vy)
        return (vx, vy, vz)
    
    # 对每个体素进行射线检测
    for y in range(sy):
        for z in range(sz):
            for x in range(sx):
                # 计算体素中心的世界坐标
                if flip_yz:
                    wx = min_x + (x / (sx - 1)) * size_x
                    wy = min_z + (z / (sz - 1)) * size_z
                    wz = min_y + (y / (sy - 1)) * size_y
                else:
                    wx = min_x + (x / (sx - 1)) * size_x
                    wy = min_y + (y / (sy - 1)) * size_y
                    wz = min_z + (z / (sz - 1)) * size_z
                
                # 从体素中心向六个方向发射射线
                hit_count = 0
                for dx, dy, dz in directions:
                    origin = (wx + dx * 0.001, wy + dy * 0.001, wz + dz * 0.001)
                    direction = (dx, dy, dz)
                    
                    # 检测与所有面的相交
                    for face in model.faces:
                        if len(face) < 3:
                            continue
                        
                        v0 = model.vertices[face[0]]
                        v1 = model.vertices[face[1]]
                        v2 = model.vertices[face[2]]
                        
                        # 如果交换了 YZ，需要调整顶点坐标
                        if flip_yz:
                            v0 = (v0[0], v0[2], v0[1])
                            v1 = (v1[0], v1[2], v1[1])
                            v2 = (v2[0], v2[2], v2[1])
                        
                        t = model.ray_triangle_intersect(origin, direction, v0, v1, v2)
                        if t is not None and t < target_size:
                            hit_count += 1
                            break
                
                # 如果射线从偶数个方向命中（偶数=内部，奇数=表面）
                # 这里用 hit_count > 0 表示表面
                if hit_count > 0:
                    grid[x][y if not flip_yz else z][z if not flip_yz else y] = True
    
    return grid, materials, sx, sy, sz


def save_vlstruct(filepath, grid, materials, sx, sy, sz):
    """保存为 vlstruct 二进制格式"""
    with open(filepath, 'wb') as f:
        # 魔数
        f.write(b'VLSTRUCT')
        # 版本
        f.write(struct.pack('<H', 2))
        # 尺寸
        f.write(struct.pack('<i', sx))
        f.write(struct.pack('<i', sy))
        f.write(struct.pack('<i', sz))
        
        # 方块数据
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
        ('altar.obj', 'ritual_element.vlstruct', 40),
        ('altar1.obj', 'ritual_god.vlstruct', 40),
        ('altarOBJ.obj', 'ritual_old_god.vlstruct', 32),
        ('oltarz_low1.OBJ', 'ritual_outer.vlstruct', 40),
        ('Postament.obj', 'ritual_time.vlstruct', 32),
        ('Scaniverse.obj', 'ritual_worldtree.vlstruct', 48),
    ]
    
    base_dir = Path('C:/Users/ThinkBook/Desktop/voxel-legend-main/tools/raw_models')
    output_dir = Path('C:/Users/ThinkBook/Desktop/voxel-legend-main/assets/structures')
    
    print("=" * 60)
    print("改进的 OBJ 体素化转换")
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
        result = voxelize_model(model, target_size=size, flip_yz=True)
        
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
