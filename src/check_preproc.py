import sys

def main():
    content = sys.stdin.read()
    
    # Find main function
    main_start = content.find('int main(int argc, char** argv) {')
    if main_start == -1:
        main_start = content.rfind('int main(')
    
    print(f'main starts at: {main_start}')
    
    # Count braces from main start to end
    braces = 0
    paren = 0
    string = False
    char_lit = False
    comment = False
    
    for i, c in enumerate(content[main_start:], main_start):
        if i > main_start and c == "'" and not string and not comment:
            char_lit = not char_lit
            continue
        if c == '"' and not char_lit and not comment:
            string = not string
            continue
        if c == '/' and i + 1 < len(content) and content[i+1] == '/' and not string and not char_lit:
            comment = True
            continue
        if c == '\n':
            comment = False
            continue
        if not string and not char_lit and not comment:
            if c == '{': braces += 1
            elif c == '}': braces -= 1
            elif c == '(': paren += 1
            elif c == ')': paren -= 1
    
    print(f'From main to end: braces={braces}, paren={paren}')
    print(f'At end of content: ...{content[-100:]}...')

if __name__ == '__main__':
    main()
