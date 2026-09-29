# STL/OBJ to .vlstruct 转换工具

将 OBJ/STL 3D 模型转换为 Voxel Legend 的结构文件格式，支持智能多材质分配。

## 依赖

```powershell
pip install numpy
```

## 快速开始

### 转换单个模型

```powershell
cd tools/stl2vlstruct

# 自动检测材质
python stl2vlstruct.py --input ..\raw_models\model.obj --output result.vlstruct --size 20

# 分层材质模式（推荐用于祭坛）
python stl2vlstruct.py --input ..\raw_models\model.obj --output result.vlstruct --mode layered --primary STONE --secondary COBBLE --accent BRICK
```

### 转换所有祭祀台

```powershell
cd tools/stl2vlstruct
python convert_rituals.py
```

### 查看可用方块

```powershell
python stl2vlstruct.py --list-blocks
```

## 可用方块

### 基础建筑方块

| 方块 | ID | 用途 |
|------|-----|------|
| STONE | 3 | 石头 - 主要建筑材料 |
| COBBLE | 18 | 圆石 - 基础装饰 |
| BRICK | 19 | 砖块 - 顶部/边缘装饰 |
| SANDSTONE | 20 | 砂岩 - 沙漠/古老风格 |
| PLANKS | 17 | 木板 |
| WOOD | 21 | 木头 |
| LOG | 6 | 原木 |
| DIRT | 2 | 泥土 |
| SAND | 4 | 沙子 |
| GRAVEL | 8 | 沙砾 |
| GLASS | 10 | 玻璃 |

### 装饰方块（新增）

| 方块 | ID | 用途 |
|------|-----|------|
| TORCH | 55 | 火把 - 照明效果 |
| CANDLE | 56 | 蜡烛 - 祭祀氛围 |
| METAL_BLOCK | 57 | 金属方块 - 装饰边框 |
| OBSIDIAN | 58 | 黑曜石 - 古老神秘材质 |

## 材质分配模式

| 模式 | 说明 | 适用场景 |
|------|------|----------|
| `auto` | 智能检测颜色和高度分配材质 | 通用 |
| `layered` | 基于高度分层（底座-主体-顶部） | 祭坛、建筑 |
| `edge` | 仅边缘填充，内部留空 | 装饰性结构 |
| `candle` | 顶部添加蜡烛效果 | 祭祀仪式 |
| `torch` | 在指定位置放置火把 | 照明需求 |

## 参数说明

| 参数 | 说明 |
|------|------|
| `--input, -i` | 输入的 OBJ/STL 文件 |
| `--output, -o` | 输出的 .vlstruct 文件 |
| `--size, -s` | 目标尺寸（最大边长），默认 20，最大 64 |
| `--mode, -m` | 材质分配模式 |
| `--primary, -p` | 主材质方块 |
| `--secondary` | 次要材质方块（底部） |
| `--accent` | 装饰材质方块（顶部/边缘） |

## 祭祀台配置说明

每个祭祀台的材质配置：

| 祭祀台 | 文件 | 尺寸 | 主材质 | 次要 | 装饰 |
|--------|------|------|--------|------|------|
| 元素之源 | ritual_element.vlstruct | 20 | STONE | COBBLE | BRICK |
| 世界树 | ritual_worldtree.vlstruct | 18 | WOOD | LOG | LEAVES |
| 世界主神 | ritual_god.vlstruct | 20 | STONE | COBBLE | SANDSTONE |
| 溯时之蛇 | ritual_time.vlstruct | 16 | SANDSTONE | COBBLE | BRICK |
| 旧神残魂 | ritual_old_god.vlstruct | 18 | OBSIDIAN | STONE | COBBLE |
| 外神 | ritual_outer.vlstruct | 22 | STONE | BRICK | METAL_BLOCK |

## 格式说明

.vlstruct 文件是二进制格式（版本 2）：

| 偏移 | 大小 | 内容 |
|------|------|------|
| 0 | 8 | 魔数: `VLSTRUCT` |
| 8 | 2 | 版本: `2`（支持多材质） |
| 10 | 4 | sx (X 尺寸) |
| 14 | 4 | sy (Y 尺寸) |
| 18 | 4 | sz (Z 尺寸) |
| 22+ | sx×sy×sz | 方块 ID 数组 |

## 在游戏中使用

转换后的 `.vlstruct` 文件放入 `assets/structures/` 目录后，可以：

1. 使用 `editor.exe` 的结构编辑器加载
2. 或在游戏中通过命令使用
