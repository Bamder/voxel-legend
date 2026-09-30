import sys

def check_main_function(filename):
    with open(filename, 'r', encoding='utf-8', errors='replace') as f:
        content = f.read()
    
    lines = content.split('\n')
    
    # Find main function start (line 1299, but let's search)
    main_line = None
    for i, line in enumerate(lines, 1):
        if 'int main(int argc, char** argv)' in line:
            main_line = i
            break
    
    if main_line is None:
        print("Could not find main function")
        return
    
    print(f"main function starts at line {main_line}")
    
    # Track balance from main start
    braces = 0
    parens = 0
    brackets = 0
    in_string = False
    in_char = False
    in_comment = False
    in_multiline = False
    
    problems = []
    
    for i, line in enumerate(lines[main_line-1:], main_line):
        # Simple line-level analysis
        j = 0
        line_braces = 0
        line_parens = 0
        
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
                in_multiline = True
                j += 2
                continue
            
            # Multi-line comment end
            if c == '*' and j + 1 < len(line) and line[j+1] == '/' and in_multiline:
                in_multiline = False
                j += 2
                continue
            
            if not in_multiline:
                # Character literal
                if c == "'" and not in_string:
                    in_char = not in_char
                
                # String literal
                if c == '"' and not in_char:
                    in_string = not in_string
                
                # Count braces/parens only outside strings/comments
                if not in_string and not in_char:
                    if c == '{': 
                        braces += 1
                        line_braces += 1
                    elif c == '}': 
                        braces -= 1
                        line_braces -= 1
                    elif c == '(': 
                        parens += 1
                        line_parens += 1
                    elif c == ')': 
                        parens -= 1
                        line_parens -= 1
                    elif c == '[': brackets += 1
                    elif c == ']': brackets -= 1
            
            j += 1
        
        # Report if balance went negative
        if braces < 0:
            problems.append((i, f"Braces went negative: {braces}"))
        if parens < 0:
            problems.append((i, f"Parens went negative: {parens}"))
    
    # Report all problems
    print(f"\nFinal balance:")
    print(f"  Braces: {braces}")
    print(f"  Parens: {parens}")
    print(f"  Brackets: {brackets}")
    print(f"  In string: {in_string}")
    print(f"  In char: {in_char}")
    print(f"  In multiline comment: {in_multiline}")
    
    if problems:
        print(f"\n{len(problems)} problems found:")
        for line, msg in problems:
            print(f"  Line {line}: {msg}")
    
    # Check final state
    if braces != 0 or parens != 0 or in_string or in_char or in_multiline:
        print("\nSyntax issues detected:")
        if braces != 0:
            print(f"  - Unmatched braces: {braces}")
        if parens != 0:
            print(f"  - Unmatched parens: {parens}")
        if in_string:
            print("  - Unclosed string literal")
        if in_char:
            print("  - Unclosed char literal")
        if in_multiline:
            print("  - Unclosed multiline comment")

if __name__ == '__main__':
    check_main_function('main.cpp')
