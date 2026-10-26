import os
import re
import sys

def fix_imports(directory):
    print(f"🔧 Scanning {directory} for missing imports...")
    
    # Regex to find object instantiation: obj = SomeClass()
    # Captures indentation and ClassName
    instantiation_pattern = re.compile(r'(\s+)obj = (\w+)\(\)')
    
    count = 0
    for filename in os.listdir(directory):
        if not filename.endswith(".py") or filename == "__init__.py":
            continue
            
        filepath = os.path.join(directory, filename)
        with open(filepath, 'r') as f:
            lines = f.readlines()
            
        modified = False
        new_lines = []
        added_imports = set()
        
        # Current file's module name (without .py)
        current_module = filename.replace(".py", "")

        for line in lines:
            match = instantiation_pattern.search(line)
            if match:
                indentation = match.group(1)
                class_name = match.group(2)
                
                # Check if target class file exists
                target_file = os.path.join(directory, f"{class_name}.py")
                
                # BUG FIX: Use strict inequality check, not substring
                if os.path.exists(target_file) and class_name != current_module:
                    
                    is_already_imported = any(f"import {class_name}" in l for l in lines)
                    
                    if not is_already_imported and class_name not in added_imports:
                        # Insert import with matching indentation
                        import_stmt = f"{indentation}from quantra.{class_name} import {class_name}\n"
                        new_lines.append(import_stmt)
                        added_imports.add(class_name)
                        print(f"   -> [FIXED] Injected '{class_name}' import into {filename}")
                        modified = True
                        count += 1
            
            new_lines.append(line)

        if modified:
            with open(filepath, 'w') as f:
                f.writelines(new_lines)
    
    print(f"✅ Scanning complete. Fixed {count} files.")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 fix_imports.py <directory>")
        sys.exit(1)
    
    fix_imports(sys.argv[1])