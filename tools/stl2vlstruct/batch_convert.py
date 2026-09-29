"""
批量转换脚本 - 将原始模型转换为 ritual 结构文件
"""
import sys
sys.path.insert(0, str(__file__).replace('batch_convert.py', ''))
from stl2vlstruct import *

# 定义转换映射
CONVERSIONS = [
    # (源文件, 目标文件, 目标尺寸, 模式)
    ('altar.obj', 'ritual_element.vlstruct', 24, 'auto'),
    ('altar1.obj', 'ritual_god.vlstruct', 24, 'auto'),
    ('altarOBJ.obj', 'ritual_old_god.vlstruct', 20, 'auto'),
    ('oltarz_low1.OBJ', 'ritual_outer.vlstruct', 24, 'auto'),
    ('Postament.obj', 'ritual_time.vlstruct', 20, 'auto'),
    ('Scaniverse.obj', 'ritual_worldtree.vlstruct', 32, 'auto'),
]

def main():
    base_dir = Path(__file__).parent.parent.parent / 'tools' / 'raw_models'
    output_dir = Path(__file__).parent.parent.parent / 'assets' / 'structures'
    
    print("=" * 60)
    print("批量转换原始模型到 vlstruct 格式")
    print("=" * 60)
    
    for src_name, dst_name, size, mode in CONVERSIONS:
        src_path = base_dir / src_name
        dst_path = output_dir / dst_name
        
        print(f"\n>>> 转换: {src_name} -> {dst_name}")
        print(f"    源文件: {src_path}")
        print(f"    目标: {dst_path}")
        print(f"    尺寸: {size}, 模式: {mode}")
        
        if not src_path.exists():
            print(f"    ❌ 源文件不存在: {src_path}")
            continue
        
        # 解析 OBJ
        print(f"    读取文件...")
        vertices, normals, vertex_colors = parse_obj_with_colors(src_path)
        print(f"    解析到 {len(vertices)} 个顶点")
        
        if not vertices:
            print(f"    ❌ 未找到顶点数据")
            continue
        
        # 体素化
        print(f"    体素化中...")
        grid, materials, sx, sy, sz = smart_voxelize(
            vertices, vertex_colors, size, {'mode': mode}
        )
        
        if grid is None:
            print(f"    ❌ 体素化失败")
            continue
        
        print(f"    体素网格: {sx} x {sy} x {sz}")
        
        # 保存
        print(f"    保存中...")
        save_vlstruct(dst_path, grid, materials, sx, sy, sz)
        
        # 统计
        material_counts = {}
        for y in range(sy):
            for z in range(sz):
                for x in range(sx):
                    if grid[x][y][z]:
                        mid = materials[x][y][z]
                        material_counts[mid] = material_counts.get(mid, 0) + 1
        
        id_to_name = {v: k for k, v in BUILD_BLOCKS.items()}
        for mid, count in sorted(material_counts.items()):
            name = id_to_name.get(mid, f"ID:{mid}")
            print(f"      {name}: {count}")
        
        print(f"    [OK] Done!")
    
    print("\n" + "=" * 60)
    print("所有转换完成!")
    print("=" * 60)

if __name__ == '__main__':
    main()
