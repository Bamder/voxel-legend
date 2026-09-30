import sys

def check_braces_from_main(filename):
    with open(filename, 'r', encoding='utf-8') as f:
        content = f.read()
    
    lines = content.split('\n')
    
    # Track balance from line 1299 (main function start)
    braces = 0
    parens = 0
    brackets = 0
    in_string = False
    in_char = False
    in_comment = False
    
    brace_changes = []
    paren_changes = []
    
    for i, line in enumerate(lines, 1):
        if i < 1299:
            continue
        
        j = 0
        line_start_braces = braces
        line_start_parens = parens
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
                        brace_changes.append((i, +1, line.strip()[:60]))
                    elif c == '}': 
                        braces -= 1
                        brace_changes.append((i, -1, line.strip()[:60]))
                    elif c == '(': 
                        parens += 1
                    elif c == ')': 
                        parens -= 1
                        paren_changes.append((i, -1, line.strip()[:60]))
                    elif c == '[': brackets += 1
                    elif c == ']': brackets -= 1
            
            j += 1
        
        # Print if anything changed
        if braces != line_start_braces or parens != line_start_parens:
            ctx = ""
            if braces < 0 or parens < 0:
                ctx = " <-- PROBLEM"
            print(f"Line {i}: braces={braces}, parens={parens} {ctx}: {line.strip()[:70]}")
    
    print(f'\nFinal balance from main():')
    print(f'  Braces: {braces}')
    print(f'  Parens: {parens}')
    print(f'  Brackets: {brackets}')
    print(f'  In string: {in_string}')
    print(f'  In char: {in_char}')
    print(f'  In comment: {in_comment}')
    
    # Find where parens went negative
    if parens < 0:
        print('\nParens went negative!')
        for pc, delta, ctx in paren_changes:
            print(f"  Line {pc}: delta={delta} - {ctx}")
    
    if braces < 0:
        print('\nBraces went negative!')
        for bc, delta, ctx in brace_changes:
            print(f"  Line {bc}: delta={delta} - {ctx}")

if __name__ == '__main__':
    check_braces_from_main('main.cpp')
