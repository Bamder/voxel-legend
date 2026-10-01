import sys
import re

def check_balanced(filename):
    with open(filename, 'r', encoding='utf-8') as f:
        content = f.read()
    
    lines = content.split('\n')
    
    # Track balance from beginning
    braces = 0
    parens = 0
    brackets = 0
    in_string = False
    in_char = False
    in_comment = False
    in_multiline = False
    
    brace_changes = []  # (line, delta, context)
    
    for i, line in enumerate(lines, 1):
        j = 0
        line_start_braces = braces
        while j < len(line):
            c = line[j]
            
            # Escape sequences
            if j > 0 and line[j-1] == '\\':
                j += 1
                continue
            
            # Single-line comment
            if c == '/' and j + 1 < len(line) and line[j+1] == '/' and not in_string and not in_char:
                break
            
            # Multi-line comment start
            if c == '/' and j + 1 < len(line) and line[j+1] == '*' and not in_string:
                in_comment = True
                j += 2
                continue
            
            # Multi-line comment end
            if c == '*' and j + 1 < len(line) and line[j+1] == '/' and in_comment:
                in_comment = False
                j += 2
                continue
            
            if not in_comment:
                # Character literal
                if c == "'" and not in_string:
                    in_char = not in_char
                
                # String literal
                if c == '"':
                    in_string = not in_string
                
                # Count braces only outside strings and comments
                if not in_string and not in_char:
                    if c == '{': 
                        braces += 1
                        brace_changes.append((i, +1, line.strip()[:50]))
                    elif c == '}': 
                        braces -= 1
                        brace_changes.append((i, -1, line.strip()[:50]))
                    elif c == '(': parens += 1
                    elif c == ')': parens -= 1
                    elif c == '[': brackets += 1
                    elif c == ']': brackets -= 1
            
            j += 1
        
        # Check if braces changed on this line
        if braces != line_start_braces:
            print(f"Line {i}: braces now {braces} - {line.strip()[:60]}")
    
    print(f'\nFinal balance:')
    print(f'  Braces: {braces}')
    print(f'  Parens: {parens}')
    print(f'  Brackets: {brackets}')
    print(f'  In string: {in_string}')
    print(f'  In char: {in_char}')
    print(f'  In comment: {in_comment}')
    
    # Find where braces went negative
    if braces < 0:
        print('\nBraces went negative!')
        for lc, delta, ctx in brace_changes:
            print(f"  Line {lc}: delta={delta} - {ctx}")

if __name__ == '__main__':
    check_balanced('main.cpp')
