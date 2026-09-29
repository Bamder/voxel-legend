#!/usr/bin/env python3
"""
Add torches and candles to existing ritual altar structures
Keeps original structure, only adds decorations
"""

import struct
import os

# Block IDs
AIR = 0
STONE = 3
COBBLESTONE = 17
BRICKS = 18
PLANKS = 16
STICK = 23
COAL = 12
GOLD = 14
WOOD = 24

def read_vlstruct(filepath):
    """Read vlstruct file"""
    with open(filepath, 'rb') as f:
        magic = f.read(8)
        if magic != b'VLSTRUCT':
            return None
        ver = struct.unpack('<H', f.read(2))[0]
        if ver not in [1, 2]:
            return None
        sx, sy, sz = struct.unpack('<iii', f.read(12))
        blocks = f.read(sx * sy * sz)
        return sx, sy, sz, blocks, ver

def save_vlstruct(filepath, sx, sy, sz, blocks):
    """Save vlstruct file (always use version 1 for compatibility)"""
    with open(filepath, 'wb') as f:
        f.write(b'VLSTRUCT')
        f.write(struct.pack('<H', 1))  # Use version 1
        f.write(struct.pack('<iii', sx, sy, sz))
        f.write(blocks)

def create_grid(sx, sy, sz):
    """Create grid from flat array"""
    return bytearray(sx * sy * sz)

def idx(x, y, z, sx, sy, sz):
    """3D to 1D index"""
    return x + y * sx + z * sx * sy

def set_block(grid, x, y, z, sx, sy, sz, block_id):
    """Set block if in bounds"""
    if 0 <= x < sx and 0 <= y < sy and 0 <= z < sz:
        grid[idx(x, y, z, sx, sy, sz)] = block_id

def get_block(grid, x, y, z, sx, sy, sz):
    """Get block if in bounds"""
    if 0 <= x < sx and 0 <= y < sy and 0 <= z < sz:
        return grid[idx(x, y, z, sx, sy, sz)]
    return 0

def find_bounds(grid, sx, sy, sz):
    """Find bounding box of non-air blocks"""
    min_x, max_x = sx, -1
    min_y, max_y = sy, -1
    min_z, max_z = sz, -1
    
    for y in range(sy):
        for z in range(sz):
            for x in range(sx):
                if grid[idx(x, y, z, sx, sy, sz)] != AIR:
                    min_x = min(min_x, x)
                    max_x = max(max_x, x)
                    min_y = min(min_y, y)
                    max_y = max(max_y, y)
                    min_z = min(min_z, z)
                    max_z = max(max_z, z)
    
    return min_x, max_x + 1, min_y, max_y + 1, min_z, max_z + 1

def add_decorations(grid, sx, sy, sz, min_x, max_x, min_y, max_y, min_z, max_z, altar_type):
    """Add torches and candles around the altar"""
    
    # Calculate altar center and dimensions
    center_x = (min_x + max_x) // 2
    center_z = (min_z + max_z) // 2
    width = max_x - min_x
    depth = max_z - min_z
    top_y = max_y
    
    # Choose decorative blocks based on altar type
    base_blocks = [COBBLESTONE, STONE, BRICKS, STONE, COBBLESTONE, BRICKS]
    candle_blocks = [GOLD, PLANKS, BRICKS, COAL, GOLD, PLANKS]
    
    base = base_blocks[altar_type % len(base_blocks)]
    candle = candle_blocks[altar_type % len(candle_blocks)]
    
    # Add stone base platform (2 blocks below)
    if min_y >= 2:
        for x in range(min_x - 1, max_x + 1):
            for z in range(min_z - 1, max_z + 1):
                if get_block(grid, x, min_y - 1, z, sx, sy, sz) == AIR:
                    set_block(grid, x, min_y - 1, z, sx, sy, sz, base)
                if get_block(grid, x, min_y - 2, z, sx, sy, sz) == AIR:
                    set_block(grid, x, min_y - 2, z, sx, sy, sz, base)
    
    # Add torches at corners
    torch_positions = [
        (min_x - 1, top_y + 1, min_z - 1),
        (max_x, top_y + 1, min_z - 1),
        (min_x - 1, top_y + 1, max_z),
        (max_x, top_y + 1, max_z),
    ]
    
    for tx, ty, tz in torch_positions:
        if 0 <= tx < sx and 0 <= ty < sy and 0 <= tz < sz:
            set_block(grid, tx, ty, tz, sx, sy, sz, STICK)
            set_block(grid, tx, ty + 1, tz, sx, sy, sz, COAL)
    
    # Add candles along edges
    candle_count = 0
    max_candles = 6 + altar_type * 2
    
    # Front edge candles
    for z in range(min_z, max_z, 3):
        if candle_count >= max_candles:
            break
        if get_block(grid, min_x - 1, top_y, z, sx, sy, sz) == AIR:
            set_block(grid, min_x - 1, top_y, z, sx, sy, sz, candle)
            set_block(grid, min_x - 1, top_y + 1, z, sx, sy, sz, COAL)
            candle_count += 1
    
    # Back edge candles
    for z in range(min_z, max_z, 3):
        if candle_count >= max_candles:
            break
        if get_block(grid, max_x, top_y, z, sx, sy, sz) == AIR:
            set_block(grid, max_x, top_y, z, sx, sy, sz, candle)
            set_block(grid, max_x, top_y + 1, z, sx, sy, sz, COAL)
            candle_count += 1
    
    # Side edge candles
    for x in range(min_x, max_x, 3):
        if candle_count >= max_candles:
            break
        if get_block(grid, x, top_y, min_z - 1, sx, sy, sz) == AIR:
            set_block(grid, x, top_y, min_z - 1, sx, sy, sz, candle)
            set_block(grid, x, top_y + 1, min_z - 1, sx, sy, sz, COAL)
            candle_count += 1
        
        if candle_count >= max_candles:
            break
        if get_block(grid, x, top_y, max_z, sx, sy, sz) == AIR:
            set_block(grid, x, top_y, max_z, sx, sy, sz, candle)
            set_block(grid, x, top_y + 1, max_z, sx, sy, sz, COAL)
            candle_count += 1
    
    # Add altar core (sacred element) on top center
    core_blocks = [GOLD, PLANKS, BRICKS, WOOD, STONE, PLANKS]
    core = core_blocks[altar_type % len(core_blocks)]
    
    if get_block(grid, center_x, top_y + 1, center_z, sx, sy, sz) == AIR:
        set_block(grid, center_x, top_y + 1, center_z, sx, sy, sz, core)
        set_block(grid, center_x, top_y + 2, center_z, sx, sy, sz, GOLD)
    
    # Add steps in front (where z is min)
    for x in range(center_x - 1, center_x + 2):
        if get_block(grid, x, min_y - 2, min_z - 2, sx, sy, sz) == AIR:
            set_block(grid, x, min_y - 2, min_z - 2, sx, sy, sz, base)
        if get_block(grid, x, min_y - 2, min_z - 1, sx, sy, sz) == AIR:
            set_block(grid, x, min_y - 2, min_z - 1, sx, sy, sz, base)
    
    return candle_count

def add_altar(src_path, altar_type):
    """Add decorations to altar file"""
    result = read_vlstruct(src_path)
    if not result:
        print(f"  [ERROR] Cannot read {src_path}")
        return False
    
    sx, sy, sz, blocks, ver = result
    
    # Copy blocks to grid
    grid = create_grid(sx, sy, sz)
    for i in range(len(blocks)):
        grid[i] = blocks[i]
    
    # Find original bounds
    min_x, max_x, min_y, max_y, min_z, max_z = find_bounds(grid, sx, sy, sz)
    
    print(f"  Original: {sx}x{sy}x{sz}, bounds ({min_x},{min_y},{min_z}) to ({max_x},{max_y},{max_z})")
    
    # Add decorations
    candle_count = add_decorations(grid, sx, sy, sz, min_x, max_x, min_y, max_y, min_z, max_z, altar_type)
    
    # Save
    save_vlstruct(src_path, sx, sy, sz, bytes(grid))
    
    # Stats
    non_air = sum(1 for b in grid if b != AIR)
    print(f"  Updated: {sx}x{sy}x{sz}, {non_air} blocks (+{candle_count} candles)")
    return True

def main():
    import sys
    script_dir = os.path.dirname(os.path.abspath(sys.argv[0]))
    project_dir = os.path.dirname(os.path.dirname(script_dir))
    base_dir = os.path.join(project_dir, "assets", "structures")
    
    print("=" * 60)
    print("Add Torches and Candles to Altars")
    print("=" * 60)
    
    altars = [
        ("ritual_element.vlstruct", "Element Altar"),
        ("ritual_god.vlstruct", "God Altar"),
        ("ritual_old_god.vlstruct", "Old God Altar"),
        ("ritual_outer.vlstruct", "Outer God Altar"),
        ("ritual_time.vlstruct", "Time Altar"),
        ("ritual_worldtree.vlstruct", "World Tree Altar"),
    ]
    
    for i, (filename, desc) in enumerate(altars):
        src_path = os.path.join(base_dir, filename)
        print(f"\n>>> Processing: {desc} ({filename})")
        
        if not os.path.exists(src_path):
            print(f"  [SKIP] File not found")
            continue
        
        if add_altar(src_path, i):
            print(f"  SUCCESS: {desc} decorated!")
    
    print("\n" + "=" * 60)
    print("All altars decorated!")
    print("=" * 60)

if __name__ == "__main__":
    main()
