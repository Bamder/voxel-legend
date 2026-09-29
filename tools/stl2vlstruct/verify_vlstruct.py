"""验证 vlstruct 文件可读性"""
import struct
import os

def read_vlstruct(path):
    """读取并解析 vlstruct 文件"""
    with open(path, 'rb') as f:
        # 读取魔数
        magic = f.read(8)
        if magic != b'VLSTRUCT':
            return None, f"Invalid magic: {magic}"
        
        # 读取版本
        version = struct.unpack('<H', f.read(2))[0]
        
        # 读取尺寸
        sx = struct.unpack('<i', f.read(4))[0]
        sy = struct.unpack('<i', f.read(4))[0]
        sz = struct.unpack('<i', f.read(4))[0]
        
        # 读取方块数据
        blocks = []
        total = sx * sy * sz
        for i in range(total):
            block_id = struct.unpack('B', f.read(1))[0]
            blocks.append(block_id)
        
        return {
            'version': version,
            'size': (sx, sy, sz),
            'total_voxels': sum(1 for b in blocks if b != 0),
            'blocks': blocks
        }, None

# 测试所有 ritual 文件
files = [
    'ritual_element',
    'ritual_god', 
    'ritual_old_god',
    'ritual_outer',
    'ritual_time',
    'ritual_worldtree'
]

base = 'assets/structures'
print("验证 vlstruct 文件:")
print("=" * 60)

for name in files:
    path = os.path.join(base, f'{name}.vlstruct')
    if os.path.exists(path):
        result, err = read_vlstruct(path)
        if err:
            print(f"  {name}: ERROR - {err}")
        else:
            print(f"  {name}: OK")
            print(f"    版本: {result['version']}")
            print(f"    尺寸: {result['size'][0]} x {result['size'][1]} x {result['size'][2]}")
            print(f"    体素数: {result['total_voxels']}")
    else:
        print(f"  {name}: NOT FOUND")
    print()

print("=" * 60)
print("所有文件验证完成!")
