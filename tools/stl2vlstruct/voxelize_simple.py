"""
简化的 OBJ 体素化脚本
直接对三角形面进行采样
"""
import struct
import random
from pathlib import Path

# 方块 ID
BLOCK_AIR = 0
BLOCK_STONE = 3
BLOCK_COBBLE = 17
BLOCK_BRICK = 18
BLOCK_WOOD = 21
BLOCK_LOG = 6

class OBJModel:
    def __init__(self):
        self.vertices = []
        self.faces = []
    
    def parse(self, filepath):
        """解析 OBJ 文件"""
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
                        self.vertices.append((x, y, z))
                    except:
                        pass
                
                elif cmd == 'f':
                    # 解析面
                    face = []
                    for i in range(1, len(parts)):
                        indices = parts[i].split('/')
                        vi = int(indices[0]) - 1
                        if 0 <= vi < len(self.vertices):
                            face.append(vi)
                    
                    # 三角化
                    if len(face) == 3:
                        self.faces.append(face)
                    elif len(face) > 3:
                        v0 = face[0]
                        for i in range(1, len(face) - 1):
                            self.faces.append([v0, face[i], face[i+1]])
        
        return len(self.faces) > 0
    
    def sample_surface(self, num_samples=5000):
        """对表面进行随机采样"""
        samples = []
        
        # 计算总表面积
        total_area = 0
        triangles = []
        for face in self.faces:
            if len(face) < 3:
                continue
            v0, v1, v2 = self.vertices[face[0]], self.vertices[face[1]], self.vertices[face[2]]
            
            # 计算三角形面积
            ax, ay, az = v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2]
            bx, by, bz = v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2]
            
            cx, cy, cz = ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx
            area = (cx*cx + cy*cy + cz*cz) ** 0.5 * 0.5
            total_area += area
            triangles.append((v0, v1, v2, area))
        
        if total_area == 0:
            return samples
        
        # 按面积比例采样
        for _ in range(num_samples):
            r = random.random() * total_area
            acc = 0
            for v0, v1, v2, area in triangles:
                acc += area
                if acc >= r:
                    # 在三角形内随机采样
                    u = random.random()
                    v = random.random()
                    if u + v > 1:
                        u, v = 1 - u, 1 - v
                    
                    wx = v0[0] + u * (v1[0] - v0[0]) + v * (v2[0] - v0[0])
                    wy = v0[1] + u * (v1[1] - v0[1]) + v * (v2[1] - v0[1])
                    wz = v0[2] + u * (v1[2] - v0[2]) + v * (v2[2] - v0[2])
                    samples.append((wx, wy, wz))
                    break
        
        return samples
    
    def get_bounds(self):
        if not self.vertices:
            return None
        xs = [v[0] for v in self.vertices]
        ys = [v[1] for v in self.vertices]
        zs = [v[2] for v in self.vertices]
        return {
            'min': (min(xs), min(ys), min(zs)),
            'max': (max(xs), max(ys), max(zs))
        }


def voxelize_samples(samples, bounds, sx, sy, sz, flip_yz=True):
    """
    将采样点转换为体素
    flip_yz: OBJ 的 Y 轴是高度，vlstruct 的 Y 轴也是高度，Z 轴可能需要交换
    """
    if not samples:
        return None, None
    
    min_x, min_y, min_z = bounds['min']
    max_x, max_y, max_z = bounds['max']
    
    size_x = max_x - min_x
    size_y = max_y - min_y
    size_z = max_z - min_z
    
    # 创建体素网格
    grid = [[[False for _ in range(sz)] for _ in range(sy)] for _ in range(sx)]
    materials = [[[BLOCK_STONE for _ in range(sz)] for _ in range(sy)] for _ in range(sx)]
    
    # 将采样点映射到体素
    for wx, wy, wz in samples:
        # 计算体素坐标
        vx = int((wx - min_x) / size_x * (sx - 1))
        vy = int((wy - min_y) / size_y * (sy - 1))
        vz = int((wz - min_z) / size_z * (sz - 1))
        
        # 边界检查
        vx = max(0, min(sx - 1, vx))
        vy = max(0, min(sy - 1, vy))
        vz = max(0, min(sz - 1, vz))
        
        # 填充体素
        grid[vx][vy][vz] = True
    
    # 填充内部（简单的洪水填充）
    fill_inside(grid, sx, sy, sz)
    
    return grid, materials


def fill_inside(grid, sx, sy, sz):
    """简单的内部填充：沿 Y 轴扫描"""
    for x in range(sx):
        for z in range(sz):
            # 找到第一个填充的体素（从上往下）
            top = -1
            bottom = -1
            for y in range(sy):
                if grid[x][y][z]:
                    if top == -1:
                        top = y
                    bottom = y
            
            # 填充中间部分
            if top != -1 and bottom > top:
                for y in range(top, bottom + 1):
                    grid[x][y][z] = True


def save_vlstruct(filepath, grid, materials, sx, sy, sz):
    """保存为 vlstruct 二进制格式"""
    with open(filepath, 'wb') as f:
        f.write(b'VLSTRUCT')
        f.write(struct.pack('<H', 1))  # 版本必须是 1
        f.write(struct.pack('<i', sx))
        f.write(struct.pack('<i', sy))
        f.write(struct.pack('<i', sz))
        
        for y in range(sy):
            for z in range(sz):
                for x in range(sx):
                    block_id = materials[x][y][z] if grid[x][y][z] else BLOCK_AIR
                    f.write(struct.pack('B', block_id))


def main():
    random.seed(42)  # 固定随机种子以便复现
    
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
    print("OBJ 体素化转换 (采样法)")
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
        
        # 计算合适的体素尺寸
        size_x = bounds['max'][0] - bounds['min'][0]
        size_y = bounds['max'][1] - bounds['min'][1]
        size_z = bounds['max'][2] - bounds['min'][2]
        max_dim = max(size_x, size_y, size_z)
        
        # 根据模型比例计算体素网格
        scale = (size - 2) / max_dim
        sx = max(1, min(64, int(size_x * scale) + 2))
        sy = max(1, min(64, int(size_y * scale) + 2))
        sz = max(1, min(64, int(size_z * scale) + 2))
        
        # 采样
        num_samples = min(20000, len(model.faces) * 10)
        print(f"    采样 {num_samples} 个点...")
        samples = model.sample_surface(num_samples)
        print(f"    采样完成，得到 {len(samples)} 个点")
        
        # 体素化
        print(f"    体素化中 (网格: {sx}x{sy}x{sz})...")
        grid, materials = voxelize_samples(samples, bounds, sx, sy, sz, flip_yz=True)
        
        if grid is None:
            print(f"    [ERROR] 体素化失败")
            continue
        
        # 统计
        voxel_count = sum(1 for y in range(sy) for z in range(sz) for x in range(sx) if grid[x][y][z])
        print(f"    填充体素: {voxel_count}")
        
        # 保存
        print(f"    保存中...")
        save_vlstruct(dst_path, grid, materials, sx, sy, sz)
        print(f"    [OK] 完成!")
    
    print("\n" + "=" * 60)
    print("转换完成!")
    print("=" * 60)


if __name__ == '__main__':
    main()
