#!/usr/bin/env python3
"""
优化祭祀台 vlstruct 文件，使其更贴近真实
- 减小尺寸，避免像大楼
- 添加石头底座
- 添加火把/蜡烛装饰
"""

import struct
import os
import math
import random

# 方块ID
AIR = 0
STONE = 3
PLANKS = 16
COBBLESTONE = 17
BRICKS = 18
WOOD = 24
STICK = 23
DIAMOND = 15
GOLD = 14
COAL = 12

def read_vlstruct(filepath):
    """Read vlstruct file"""
    with open(filepath, 'rb') as f:
        magic = f.read(8)
        if magic != b'VLSTRUCT':
            return None
        ver = struct.unpack('<H', f.read(2))[0]
        if ver != 1:
            return None
        sx, sy, sz = struct.unpack('<iii', f.read(12))
        blocks = f.read(sx * sy * sz)
        return sx, sy, sz, blocks

def save_vlstruct(filepath, sx, sy, sz, blocks):
    """Save vlstruct file"""
    with open(filepath, 'wb') as f:
        f.write(b'VLSTRUCT')
        f.write(struct.pack('<H', 1))
        f.write(struct.pack('<iii', sx, sy, sz))
        f.write(blocks)

def create_grid(sx, sy, sz):
    """Create empty grid"""
    return bytearray(sx * sy * sz)

def idx(x, y, z, sx, sy, sz):
    """Calculate 3D to 1D index"""
    return x + y * sx + z * sx * sy

def set_block(grid, x, y, z, sx, sy, sz, block_id):
    """Set block"""
    if 0 <= x < sx and 0 <= y < sy and 0 <= z < sz:
        grid[idx(x, y, z, sx, sy, sz)] = block_id

def get_block(grid, x, y, z, sx, sy, sz):
    """Get block"""
    if 0 <= x < sx and 0 <= y < sy and 0 <= z < sz:
        return grid[idx(x, y, z, sx, sy, sz)]
    return 0

def find_model_bounds(grid, sx, sy, sz):
    """Find model bounds"""
    min_x, max_x = sx, -1
    min_y, max_y = sy, -1
    min_z, max_z = sz, -1
    
    for y in range(sy):
        for z in range(sz):
            for x in range(sx):
                if grid[idx(x, y, z, sx, sy, sz)] != AIR:
                    if x < min_x: min_x = x
                    if x > max_x: max_x = x
                    if y < min_y: min_y = y
                    if y > max_y: max_y = y
                    if z < min_z: min_z = z
                    if z > max_z: max_z = z
    
    return min_x, max_x + 1, min_y, max_y + 1, min_z, max_z + 1

def optimize_altar(src_path, dst_path, altar_type):
    """
    Optimize altar
    altar_type: 0-5 for different types
    """
    result = read_vlstruct(src_path)
    if not result:
        print(f"  [ERROR] Cannot read {src_path}")
        return False
    
    orig_sx, orig_sy, orig_sz, orig_blocks = result
    
    # Create new grid - larger for decorations
    new_sx, new_sy, new_sz = 32, 16, 32
    grid = create_grid(new_sx, new_sy, new_sz)
    
    # Find original model bounds
    min_x, max_x, min_y, max_y, min_z, max_z = find_model_bounds(orig_blocks, orig_sx, orig_sy, orig_sz)
    
    # Calculate scale and offset
    model_w = max_x - min_x
    model_h = max_y - min_y
    model_d = max_z - min_z
    
    # Scale to appropriate size (max 8x8x8)
    max_dim = max(model_w, model_h, model_d)
    scale = min(8.0 / max_dim, 1.0)
    
    new_w = int(model_w * scale)
    new_h = int(model_h * scale)
    new_d = int(model_d * scale)
    
    # Center placement
    offset_x = (new_sx - new_w) // 2
    offset_y = 3  # Leave 3 for base
    offset_z = (new_sz - new_d) // 2
    
    # Base - different base per type
    base_block = [COBBLESTONE, STONE, BRICKS, STONE, COBBLESTONE, BRICKS][altar_type]
    for x in range(8):
        for z in range(8):
            set_block(grid, offset_x - 2 + x, 0, offset_z - 2 + z, new_sx, new_sy, new_sz, base_block)
            set_block(grid, offset_x - 2 + x, 1, offset_z - 2 + z, new_sx, new_sy, new_sz, base_block)
            # Second layer slightly inset
            if x > 0 and x < 7 and z > 0 and z < 7:
                set_block(grid, offset_x - 2 + x, 2, offset_z - 2 + z, new_sx, new_sy, new_sz, base_block)
    
    # Scale and place original model
    for y in range(min_y, max_y):
        for z in range(min_z, max_z):
            for x in range(min_x, max_x):
                block_id = orig_blocks[idx(x, y, z, orig_sx, orig_sy, orig_sz)]
                if block_id != AIR:
                    # Scale coordinates
                    nx = int((x - min_x) * scale)
                    ny = int((y - min_y) * scale)
                    nz = int((z - min_z) * scale)
                    if nx >= new_w: nx = new_w - 1
                    if ny >= new_h: ny = new_h - 1
                    if nz >= new_d: nz = new_d - 1
                    set_block(grid, offset_x + nx, offset_y + ny, offset_z + nz, new_sx, new_sy, new_sz, block_id)
    
    # Add decorative elements
    # Torch positions (four corners)
    torch_positions = [
        (offset_x - 1, offset_y + new_h, offset_z - 1),
        (offset_x + new_w, offset_y + new_h, offset_z - 1),
        (offset_x - 1, offset_y + new_h, offset_z + new_d),
        (offset_x + new_w, offset_y + new_h, offset_z + new_d),
    ]
    
    # Candle positions (edges)
    candle_positions = [
        (offset_x - 2, offset_y + new_h + 1, offset_z - 2),
        (offset_x + new_w + 1, offset_y + new_h + 1, offset_z - 2),
        (offset_x - 2, offset_y + new_h + 1, offset_z + new_d + 1),
        (offset_x + new_w + 1, offset_y + new_h + 1, offset_z + new_d + 1),
        (offset_x + new_w // 2, offset_y + new_h + 2, offset_z - 2),
        (offset_x + new_w // 2, offset_y + new_h + 2, offset_z + new_d + 1),
    ]
    
    # Sacred core (top center) - different block per type
    core_blocks = [DIAMOND, GOLD, COAL, STONE, PLANKS, WOOD][altar_type]
    center_x = offset_x + new_w // 2
    center_z = offset_z + new_d // 2
    top_y = offset_y + new_h
    
    # Add torches
    for tx, ty, tz in torch_positions:
        if 0 <= tx < new_sx and 0 <= ty < new_sy and 0 <= tz < new_sz:
            set_block(grid, tx, ty, tz, new_sx, new_sy, new_sz, STICK)
            set_block(grid, tx, ty + 1, tz, new_sx, new_sy, new_sz, COAL)  # Flame
    
    # Add candles
    for cx, cy, cz in candle_positions:
        if 0 <= cx < new_sx and 0 <= cy < new_sy and 0 <= cz < new_sz:
            set_block(grid, cx, cy, cz, new_sx, new_sy, new_sz, PLANKS)  # Candle base
            set_block(grid, cx, cy + 1, cz, new_sx, new_sy, new_sz, GOLD)  # Candle wick/flame
    
    # Add sacred core
    set_block(grid, center_x, top_y + 1, center_z, new_sx, new_sy, new_sz, core_blocks)
    set_block(grid, center_x, top_y + 2, center_z, new_sx, new_sy, new_sz, GOLD)  # Top decoration
    
    # Add steps
    for x in range(3):
        set_block(grid, center_x - 1 + x, offset_y, center_z + new_d + 1, new_sx, new_sy, new_sz, base_block)
        set_block(grid, center_x - 1 + x, offset_y, center_z - 2, new_sx, new_sy, new_sz, base_block)
    
    # Save
    save_vlstruct(dst_path, new_sx, new_sy, new_sz, bytes(grid))
    
    # Stats
    non_air = sum(1 for b in grid if b != AIR)
    print(f"  [OK] {os.path.basename(dst_path)}: {new_sx}x{new_sy}x{new_sz}, {non_air} blocks")
    return True

def main():
    import sys
    script_dir = os.path.dirname(os.path.abspath(sys.argv[0]))
    # Go up: stl2vlstruct -> tools -> project root
    project_dir = os.path.dirname(os.path.dirname(script_dir))
    base_dir = os.path.join(project_dir, "assets", "structures")
    
    print("=" * 60)
    print("Altar Optimization Script")
    print("=" * 60)
    
    # Altar configuration
    altars = [
        ("ritual_element.vlstruct", "altar.obj", "Element Altar"),
        ("ritual_god.vlstruct", "altar1.obj", "God Altar"),
        ("ritual_old_god.vlstruct", "altarOBJ.obj", "Old God Altar"),
        ("ritual_outer.vlstruct", "oltarz_low1.OBJ", "Outer God Altar"),
        ("ritual_time.vlstruct", "Postament.obj", "Time Altar"),
        ("ritual_worldtree.vlstruct", "Scaniverse.obj", "World Tree Altar"),
    ]
    
    for i, (vlstruct_name, obj_name, desc) in enumerate(altars):
        src_path = os.path.join(base_dir, vlstruct_name)
        print(f"\n>>> Processing: {desc} ({vlstruct_name})")
        
        if not os.path.exists(src_path):
            print(f"  [SKIP] File not found: {src_path}")
            continue
        
        if optimize_altar(src_path, src_path, i):
            print(f"  SUCCESS: {desc} optimized!")
    
    print("\n" + "=" * 60)
    print("All altars optimized!")
    print("=" * 60)

if __name__ == "__main__":
    main()
