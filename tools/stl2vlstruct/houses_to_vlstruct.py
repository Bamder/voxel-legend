# 转换脚本：将3个房子OBJ模型转换为vlstruct格式
# 使用方法：在项目根目录运行 python tools/stl2vlstruct/houses_to_vlstruct.py

import sys
import os

# 添加工具目录到路径
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from stl2vlstruct import OBJConverter, load_config

def convert_houses():
    """转换3个房子OBJ文件为vlstruct"""

    raw_models_dir = os.path.join(os.path.dirname(os.path.dirname(__file__)), 'raw_models')
    output_dir = os.path.join(os.path.dirname(os.path.dirname(__file__)), '..', 'assets', 'structures')

    # 定义要转换的文件（按实际文件命名）
    houses = [
        {
            'input': os.path.join(raw_models_dir, 'room.obj'),
            'output': os.path.join(output_dir, 'supply_room.vlstruct'),
            'name': '补给房',
            'description': '道具补给房间'
        },
        {
            'input': os.path.join(raw_models_dir, 'Isometric room.obj'),
            'output': os.path.join(output_dir, 'weapon_room.vlstruct'),
            'name': '武器房',
            'description': '武器装备房间'
        },
        {
            'input': os.path.join(raw_models_dir, 'japanese room.obj'),
            'output': os.path.join(output_dir, 'clue_room.vlstruct'),
            'name': '线索房',
            'description': '线索调查房间'
        }
    ]

    print("=" * 50)
    print("开始转换房屋模型...")
    print("=" * 50)

    for house in houses:
        print(f"\n>>> 正在转换: {house['name']} ({os.path.basename(house['input'])})")

        if not os.path.exists(house['input']):
            print(f"    [错误] 文件不存在: {house['input']}")
            continue

        try:
            converter = OBJConverter(house['input'], config_path=None)
            config = converter.config

            # 根据房屋类型设置不同的参数
            if 'weapon' in house['output']:
                # 武器房 - 较大尺寸，石头材质
                config['target_size'] = 20
                config['mode'] = 'auto'
            elif 'supply' in house['output']:
                # 补给房 - 中等尺寸
                config['target_size'] = 18
                config['mode'] = 'auto'
            elif 'clue' in house['output']:
                # 线索房 - 较小尺寸，日式风格
                config['target_size'] = 15
                config['mode'] = 'auto'

            converter.config = config

            # 解析OBJ文件
            vertices = converter.parse_obj()
            print(f"    解析完成: {len(vertices)} 个顶点")

            # 体素化
            grid = converter.voxelize(vertices)
            print(f"    体素化完成")

            # 转换为block IDs
            blocks = converter.grid_to_blocks(grid)
            print(f"    转换完成: {len(blocks)} 个方块")

            # 保存vlstruct
            converter.save_vlstruct(house['output'], blocks)
            print(f"    [成功] 保存到: {house['output']}")

        except Exception as e:
            print(f"    [错误] 转换失败: {e}")
            import traceback
            traceback.print_exc()

    print("\n" + "=" * 50)
    print("转换完成!")
    print("=" * 50)

if __name__ == '__main__':
    convert_houses()
