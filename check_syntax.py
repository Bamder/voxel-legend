import sys

def check_file(filename):
    with open(filename, 'r', encoding='utf-8') as f:
        content = f.read()
    
    lines = content.split('\n')
    
    # Simple brace counter
    in_string = False
    in_char = False
    in_comment = False
    in_multiline = False
    
    for i, line in enumerate(lines, 1):
        j = 0
        while j < len(line):
            c = line[j]
            
            # Handle escape sequences
            if j > 0 and line[j-1] == '\\':
                j += 1
                continue
            
            # Handle strings
            if c == '"' and not in_comment and not in_char:
                in_string = not in_string
            
            j += 1
        
        # Check line-level issues
        stripped = line.strip()
        
        # Skip comment-only lines
        if stripped.startswith('//'):
            continue
        
        # Check for odd quotes (string not closed on line)
        dq = stripped.count('"')
        if dq % 2 != 0 and not in_string:
            print(f'Line {i}: Odd quotes - {stripped[:100]}')
    
    # Check overall brace balance
    braces = 0
    parens = 0
    brackets = 0
    in_string = False
    in_char = False
    in_comment = False
    multiline_start = -1
    
    for i, line in enumerate(lines, 1):
        j = 0
        lbrace_in_string = False
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
                multiline_start = i
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
                    if c == '{': braces += 1
                    elif c == '}': braces -= 1
                    elif c == '(': parens += 1
                    elif c == ')': parens -= 1
                    elif c == '[': brackets += 1
                    elif c == ']': brackets -= 1
            
            j += 1
    
    print(f'\nBrace balance: {{{braces}}}')
    print(f'Paren balance: ({parens})')
    print(f'Bracket balance: [{brackets}]')
    print(f'In string: {in_string}')
    print(f'In char: {in_char}')
    print(f'In comment: {in_comment}')
    
    if in_string:
        print('\nWARNING: String literal not closed at end of file!')
    if in_char:
        print('\nWARNING: Character literal not closed at end of file!')
    if in_comment:
        print(f'\nWARNING: Comment not closed! Started at line {multiline_start}')

if __name__ == '__main__':
    check_file('main.cpp')
