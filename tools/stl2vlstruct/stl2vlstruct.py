"""
Voxel Legend .vlstruct 转换工具 - 智能多材质版本
将 OBJ/STL 3D 模型转换为 .vlstruct 格式，支持智能多材质分配

使用方法:
    python stl2vlstruct.py --input model.obj --output result.vlstruct
    python stl2vlstruct.py --input model.obj --config ritual_config.json
    python stl2vlstruct.py --list-blocks
"""

import argparse
import json
import math
import struct
import sys
from pathlib import Path

# 项目方块 ID 映射 (与 blocks.hpp 中的 enum Block 一致)
BLOCK_IDS = {
    'AIR': 0, 'GRASS': 1, 'DIRT': 2, 'STONE': 3, 'SAND': 4, 'WATER': 5,
    'LOG': 6, 'LEAVES': 7, 'GRAVEL': 8, 'SNOW': 9, 'GLASS': 10,
    'BEDROCK': 11, 'COAL_ORE': 12, 'IRON_ORE': 13, 'GOLD_ORE': 14, 'DIAMOND_ORE': 15,
    'PLANKS': 16, 'COBBLE': 17, 'BRICK': 18, 'SANDSTONE': 19,
    'WOOD': 24, 'BARK': 25,
    # 新增装饰方块
    'TORCH': 49,        # 火把
    'CANDLE': 50,       # 蜡烛
    'METAL_BLOCK': 51,  # 金属方块
    'OBSIDIAN': 52,     # 黑曜石
}

# 可用于建筑的方块
BUILD_BLOCKS = {
    'STONE': 3, 'COBBLE': 18, 'BRICK': 19, 'SANDSTONE': 20,
    'PLANKS': 17, 'WOOD': 21, 'LOG': 6,
    'DIRT': 2, 'SAND': 4, 'GRAVEL': 8, 'GLASS': 10,
    'TORCH': 55, 'CANDLE': 56, 'METAL_BLOCK': 57, 'OBSIDIAN': 58,
}


def parse_obj_with_colors(filepath: Path):
    """解析 OBJ 文件，返回顶点列表和颜色信息"""
    vertices = []
    normals = []
    vertex_colors = []  # 存储每个顶点的颜色 (r, g, b)
    current_color = (1.0, 1.0, 1.0)  # 默认白色

    # OBJ 颜色库 (mtl 文件中的颜色)
    materials = {}

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
                # 顶点: v x y z [w]
                try:
                    x, y, z = float(parts[1]), float(parts[2]), float(parts[3])
                    vertices.append((x, y, z))
                    vertex_colors.append(current_color)
                except (ValueError, IndexError):
                    pass
            elif cmd == 'vn':
                # 法向量: vn x y z
                try:
                    nx, ny, nz = float(parts[1]), float(parts[2]), float(parts[3])
                    normals.append((nx, ny, nz))
                except (ValueError, IndexError):
                    pass
            elif cmd == 'usemtl':
                # 使用材质
                mtl_name = parts[1] if len(parts) > 1 else None
                if mtl_name in materials:
                    current_color = materials[mtl_name]
            elif cmd == 'mtllib':
                # 尝试加载材质库
                mtl_path = filepath.parent / parts[1]
                if mtl_path.exists():
                    materials.update(parse_mtl(mtl_path))

    return vertices, normals, vertex_colors


def parse_mtl(filepath: Path):
    """解析 MTL 文件，返回材质颜色字典"""
    materials = {}
    current_mtl = None

    with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue

            parts = line.split()
            if not parts:
                continue

            cmd = parts[0]
            if cmd == 'newmtl':
                current_mtl = parts[1] if len(parts) > 1 else None
                materials[current_mtl] = (1.0, 1.0, 1.0)
            elif cmd == 'Kd' and current_mtl:
                # 漫反射颜色
                try:
                    r, g, b = float(parts[1]), float(parts[2]), float(parts[3])
                    materials[current_mtl] = (r, g, b)
                except (ValueError, IndexError):
                    pass

    return materials


def parse_stl(filepath: Path):
    """解析 STL 文件（二进制格式），返回顶点列表"""
    vertices = []

    with open(filepath, 'rb') as f:
        # 跳过80字节头部
        f.read(80)
        # 读取三角形数量
        tri_count = struct.unpack('<I', f.read(4))[0]

        for _ in range(tri_count):
            # 读取法向量
            f.read(12)
            # 读取3个顶点
            for _ in range(3):
                x, y, z = struct.unpack('<fff', f.read(12))
                vertices.append((x, y, z))

    return vertices, [], []


def color_to_block_type(r: float, g: float, b: float):
    """根据颜色判断应该使用的方块类型"""
    # 归一化颜色值
    max_c = max(r, g, b)
    if max_c > 0:
        r, g, b = r / max_c, g / max_c, b / max_c

    # 黑色系 -> 黑曜石
    if r < 0.2 and g < 0.2 and b < 0.3:
        return BLOCK_IDS['OBSIDIAN']

    # 灰色系 -> 石砖
    if abs(r - g) < 0.15 and abs(g - b) < 0.15 and r > 0.2:
        return BLOCK_IDS['STONE']

    # 金色/黄色系 -> 金属
    if r > 0.7 and g > 0.6 and b < 0.3:
        return BLOCK_IDS['METAL_BLOCK']

    # 红色/橙色系 -> 砖块
    if r > 0.5 and g < 0.4 and b < 0.3:
        return BLOCK_IDS['BRICK']

    # 棕色系 -> 木头
    if r > 0.4 and g > 0.2 and g < 0.5 and b < 0.2:
        return BLOCK_IDS['WOOD']

    # 白色/浅色 -> 圆石
    if r > 0.6 and g > 0.6 and b > 0.6:
        return BLOCK_IDS['COBBLE']

    # 默认返回石头
    return BLOCK_IDS['STONE']


def smart_voxelize(vertices, vertex_colors, target_size: int = 20, config: dict = None):
    """
    智能体素化，支持多材质分配
    config: 可选配置字典，包含材质规则
    """
    if not vertices:
        return None, None, 0, 0, 0

    config = config or {}

    # 计算包围盒
    xs = [v[0] for v in vertices]
    ys = [v[1] for v in vertices]
    zs = [v[2] for v in vertices]

    min_x, max_x = min(xs), max(xs)
    min_y, max_y = min(ys), max(ys)
    min_z, max_z = min(zs), max(zs)

    # 避免零尺寸
    size_x = max(max_x - min_x, 0.001)
    size_y = max(max_y - min_y, 0.001)
    size_z = max(max_z - min_z, 0.001)

    # 调整目标尺寸以适应原始比例
    max_dim = max(size_x, size_y, size_z)
    scale = (target_size - 2) / max_dim

    sx = max(1, min(64, int(size_x * scale) + 2))
    sy = max(1, min(64, int(size_y * scale) + 2))
    sz = max(1, min(64, int(size_z * scale) + 2))

    # 创建体素网格
    grid = [[[False for _ in range(sz)] for _ in range(sy)] for _ in range(sx)]
    materials = [[[0 for _ in range(sz)] for _ in range(sy)] for _ in range(sx)]

    # 顶点到体素的映射
    vertex_voxels = []
    for vertex in vertices:
        vx = int((vertex[0] - min_x) / size_x * (sx - 1) + 0.5)
        vy = int((vertex[1] - min_y) / size_y * (sy - 1) + 0.5)
        vz = int((vertex[2] - min_z) / size_z * (sz - 1) + 0.5)

        vx = max(0, min(sx - 1, vx))
        vy = max(0, min(sy - 1, vy))
        vz = max(0, min(sz - 1, vz))

        grid[vx][vy][vz] = True
        vertex_voxels.append((vx, vy, vz))

    # 基于高度分配材质
    height_ratio = sy / target_size  # 高度比例

    for idx, (vx, vy, vz) in enumerate(vertex_voxels):
        # 计算相对高度 (0.0 = 底部, 1.0 = 顶部)
        rel_height = vy / max(sy - 1, 1)

        # 尝试从颜色获取材质
        if vertex_colors and idx < len(vertex_colors):
            color = vertex_colors[idx]
            mat = color_to_block_type(*color)
        else:
            mat = None

        # 基于高度和配置的材质分配
        if config.get('edge_only'):
            # 边缘模式：只在边缘放置方块
            if is_edge_voxel(vx, vy, vz, grid, sx, sy, sz):
                if mat:
                    materials[vx][vy][vz] = mat
                elif rel_height > 0.85:
                    materials[vx][vy][vz] = BLOCK_IDS['BRICK']
                elif rel_height < 0.15:
                    materials[vx][vy][vz] = BLOCK_IDS['COBBLE']
                else:
                    materials[vx][vy][vz] = BLOCK_IDS['STONE']
        elif config.get('layered'):
            # 分层模式：不同高度使用不同材质
            if rel_height > 0.9:
                materials[vx][vy][vz] = BLOCK_IDS.get(config.get('top_block', 'BRICK'), BLOCK_IDS['BRICK'])
            elif rel_height > 0.7:
                materials[vx][vy][vz] = BLOCK_IDS.get(config.get('mid_block', 'STONE'), BLOCK_IDS['STONE'])
            elif rel_height > 0.3:
                materials[vx][vy][vz] = BLOCK_IDS.get(config.get('body_block', 'COBBLE'), BLOCK_IDS['COBBLE'])
            elif rel_height > 0.1:
                materials[vx][vy][vz] = BLOCK_IDS.get(config.get('base_block', 'STONE'), BLOCK_IDS['STONE'])
            else:
                materials[vx][vy][vz] = BLOCK_IDS.get(config.get('foundation', 'COBBLE'), BLOCK_IDS['COBBLE'])
        elif config.get('candle_towers') and rel_height > 0.8:
            # 蜡烛塔模式：顶部添加蜡烛
            if is_tower_top(vx, vy, vz, grid, sx, sy, sz):
                materials[vx][vy][vz] = BLOCK_IDS['CANDLE']
            elif mat:
                materials[vx][vy][vz] = mat
            else:
                materials[vx][vy][vz] = BLOCK_IDS['STONE']
        elif config.get('torch_positions'):
            # 火把模式：在指定位置放置火把
            pos_key = f"{vx},{vy},{vz}"
            if pos_key in config['torch_positions']:
                materials[vx][vy][vz] = BLOCK_IDS['TORCH']
            elif mat:
                materials[vx][vy][vz] = mat
            else:
                materials[vx][vy][vz] = BLOCK_IDS['STONE']
        else:
            # 默认：基于高度智能分配
            if mat:
                materials[vx][vy][vz] = mat
            elif rel_height > 0.85:
                # 顶部边缘 - 使用砖块
                if is_edge_voxel(vx, vy, vz, grid, sx, sy, sz):
                    materials[vx][vy][vz] = BLOCK_IDS['BRICK']
                else:
                    materials[vx][vy][vz] = BLOCK_IDS['STONE']
            elif rel_height > 0.6:
                # 中上部 - 使用石头
                materials[vx][vy][vz] = BLOCK_IDS['STONE']
            elif rel_height > 0.2:
                # 中下部 - 使用圆石
                materials[vx][vy][vz] = BLOCK_IDS['COBBLE']
            else:
                # 底部 - 使用更坚固的石头
                materials[vx][vy][vz] = BLOCK_IDS['COBBLE']

    # 填充内部
    for z in range(sz):
        for x in range(sx):
            hits = [y for y in range(sy) if grid[x][y][z]]
            if len(hits) >= 2:
                hit_start, hit_end = hits[0], hits[-1]
                for y in range(hit_start, min(hit_end + 1, sy)):
                    if materials[x][y][z] == 0:
                        # 内部填充使用默认材质
                        rel_h = y / max(sy - 1, 1)
                        if rel_h > 0.7:
                            materials[x][y][z] = BLOCK_IDS['STONE']
                        elif rel_h > 0.3:
                            materials[x][y][z] = BLOCK_IDS['COBBLE']
                        else:
                            materials[x][y][z] = BLOCK_IDS['STONE']

    return grid, materials, sx, sy, sz


def is_edge_voxel(x, y, z, grid, sx, sy, sz):
    """判断体素是否在表面"""
    if x <= 0 or x >= sx - 1 or y <= 0 or y >= sy - 1 or z <= 0 or z >= sz - 1:
        return True
    neighbors = [
        (x - 1, y, z), (x + 1, y, z),
        (x, y - 1, z), (x, y + 1, z),
        (x, y, z - 1), (x, y, z + 1)
    ]
    for nx, ny, nz in neighbors:
        if not grid[nx][ny][nz]:
            return True
    return False


def is_tower_top(x, y, z, grid, sx, sy, sz):
    """判断是否为塔顶（上方无方块）"""
    if y >= sy - 1:
        return True
    return not grid[x, y + 1, z] if y + 1 < sy else True


def save_vlstruct(filepath: Path, grid, materials, sx: int, sy: int, sz: int):
    """保存为 .vlstruct 二进制格式"""
    with open(filepath, 'wb') as f:
        # 魔数 "VLSTRUCT"
        f.write(b'VLSTRUCT')
        # 版本 2 (支持多材质)
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
                        block_id = materials[x][y][z] if materials[x][y][z] > 0 else BLOCK_IDS['STONE']
                    else:
                        block_id = 0
                    f.write(struct.pack('B', block_id))


def list_blocks():
    """列出可用的建筑方块"""
    print("可用建筑方块:")
    print("-" * 40)
    print("基础建筑:")
    for name, bid in sorted([(k, v) for k, v in BUILD_BLOCKS.items() if v < 50],
                           key=lambda x: x[1]):
        print(f"  {name:12} (ID: {bid})")
    print("\n装饰方块:")
    for name, bid in sorted([(k, v) for k, v in BUILD_BLOCKS.items() if v >= 50],
                           key=lambda x: x[1]):
        print(f"  {name:12} (ID: {bid})")


def main():
    parser = argparse.ArgumentParser(
        description='将 OBJ/STL 3D 模型转换为 Voxel Legend .vlstruct 格式（智能多材质版）'
    )
    parser.add_argument('--input', '-i', type=Path, default=None,
                        help='输入的 OBJ 或 STL 文件')
    parser.add_argument('--output', '-o', type=Path, default=None,
                        help='输出的 .vlstruct 文件')
    parser.add_argument('--size', '-s', type=int, default=20,
                        help='目标尺寸（最大边长），默认 20，最大 64')
    parser.add_argument('--config', '-c', type=Path, default=None,
                        help='材质配置文件 (JSON)')
    parser.add_argument('--mode', '-m', default='auto',
                        choices=['auto', 'layered', 'edge', 'candle', 'torch'],
                        help='材质分配模式: auto=智能, layered=分层, edge=边缘, candle=蜡烛塔, torch=火把')
    parser.add_argument('--primary', '-p', default='STONE',
                        help='主材质方块')
    parser.add_argument('--secondary', default='COBBLE',
                        help='次要材质方块')
    parser.add_argument('--accent', default='BRICK',
                        help='装饰材质方块')
    parser.add_argument('--list-blocks', '-l', action='store_true',
                        help='列出所有可用的建筑方块')

    args = parser.parse_args()

    if args.list_blocks:
        list_blocks()
        return

    if not args.input or not args.output:
        print("错误: 必须指定 --input 和 --output 参数", file=sys.stderr)
        sys.exit(1)

    # 加载配置
    config = {}
    if args.config and args.config.exists():
        with open(args.config, 'r', encoding='utf-8') as f:
            config = json.load(f)
    else:
        # 从命令行参数构建配置
        config['mode'] = args.mode
        if args.primary.upper() in BUILD_BLOCKS:
            config['body_block'] = args.primary.upper()
        if args.secondary.upper() in BUILD_BLOCKS:
            config['base_block'] = args.secondary.upper()
        if args.accent.upper() in BUILD_BLOCKS:
            config['top_block'] = args.accent.upper()

    print(f"读取文件: {args.input}")
    ext = args.input.suffix.lower()

    if ext == '.obj':
        vertices, normals, vertex_colors = parse_obj_with_colors(args.input)
    elif ext == '.stl':
        vertices = parse_stl(args.input)[0]
        vertex_colors = []
    else:
        print(f"错误: 不支持的文件格式 '{ext}'（仅支持 .obj 和 .stl）", file=sys.stderr)
        sys.exit(1)

    print(f"解析到 {len(vertices)} 个顶点")

    if not vertices:
        print("错误: 文件中未找到顶点数据", file=sys.stderr)
        sys.exit(1)

    target_size = max(1, min(64, args.size))
    print(f"体素化（目标尺寸: {target_size}, 模式: {config.get('mode', 'auto')}）...")

    grid, materials, sx, sy, sz = smart_voxelize(
        vertices, vertex_colors, target_size, config
    )

    if grid is None:
        print("错误: 体素化失败", file=sys.stderr)
        sys.exit(1)

    print(f"体素网格尺寸: {sx} x {sy} x {sz}")

    # 统计材质分布
    material_counts = {}
    for y in range(sy):
        for z in range(sz):
            for x in range(sx):
                if grid[x][y][z]:
                    mid = materials[x][y][z]
                    material_counts[mid] = material_counts.get(mid, 0) + 1

    print("材质分布:")
    id_to_name = {v: k for k, v in BUILD_BLOCKS.items()}
    for mid, count in sorted(material_counts.items()):
        name = id_to_name.get(mid, f"ID:{mid}")
        print(f"  {name}: {count}")

    print(f"保存到: {args.output}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    save_vlstruct(args.output, grid, materials, sx, sy, sz)

    print("完成!")


if __name__ == '__main__':
    main()
