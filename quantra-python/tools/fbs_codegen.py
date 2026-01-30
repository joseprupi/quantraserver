#!/usr/bin/env python3
"""
FlatBuffers Schema to Python Dataclass Generator

Reads .fbs files and generates Pythonic wrapper classes that serialize
to/from FlatBuffers.
"""

import re
import os
from pathlib import Path
from dataclasses import dataclass
from typing import List, Dict, Optional, Set, Tuple


@dataclass
class Field:
    name: str
    fbs_type: str
    python_type: str
    is_required: bool
    is_list: bool
    is_enum: bool
    is_table: bool
    is_union: bool
    default: Optional[str]
    enum_namespace: Optional[str] = None


@dataclass  
class Table:
    name: str
    fields: List[Field]
    namespace: str


@dataclass
class Enum:
    name: str
    values: List[str]
    namespace: str


@dataclass
class Union:
    name: str
    types: List[str]
    namespace: str


class FBSParser:
    """Parse .fbs schema files"""
    
    # Map FlatBuffers types to Python types
    TYPE_MAP = {
        'bool': 'bool',
        'byte': 'int',
        'ubyte': 'int', 
        'short': 'int',
        'ushort': 'int',
        'int': 'int',
        'uint': 'int',
        'long': 'int',
        'ulong': 'int',
        'float': 'float',
        'double': 'float',
        'string': 'str',
    }
    
    def __init__(self, fbs_dir: str):
        self.fbs_dir = Path(fbs_dir)
        self.tables: Dict[str, Table] = {}
        self.enums: Dict[str, Enum] = {}
        self.unions: Dict[str, Union] = {}
        self.current_namespace = "quantra"
        
    def parse_all(self):
        """Parse all .fbs files in directory"""
        # Parse enums.fbs first (dependencies)
        enums_file = self.fbs_dir / "enums.fbs"
        if enums_file.exists():
            self.parse_file(enums_file)
            
        # Parse rest
        for fbs_file in sorted(self.fbs_dir.glob("*.fbs")):
            if fbs_file.name != "enums.fbs":
                self.parse_file(fbs_file)
                
    def parse_file(self, filepath: Path):
        """Parse a single .fbs file"""
        content = filepath.read_text()
        
        # Remove comments
        content = re.sub(r'//.*$', '', content, flags=re.MULTILINE)
        content = re.sub(r'/\*.*?\*/', '', content, flags=re.DOTALL)
        
        # Extract namespace
        ns_match = re.search(r'namespace\s+([\w.]+);', content)
        if ns_match:
            self.current_namespace = ns_match.group(1)
        
        # Parse enums
        for match in re.finditer(r'enum\s+(\w+)\s*:\s*\w+\s*\{([^}]+)\}', content):
            enum_name = match.group(1)
            values_str = match.group(2)
            values = [v.strip().rstrip(',') for v in values_str.split('\n') if v.strip() and not v.strip().startswith('//')]
            values = [v.split('=')[0].strip() for v in values if v]  # Remove = assignments
            self.enums[enum_name] = Enum(enum_name, values, self.current_namespace)
            
        # Parse unions
        for match in re.finditer(r'union\s+(\w+)\s*\{([^}]+)\}', content):
            union_name = match.group(1)
            types_str = match.group(2)
            types = [t.strip().rstrip(',') for t in types_str.split(',') if t.strip()]
            self.unions[union_name] = Union(union_name, types, self.current_namespace)
            
        # Parse tables
        for match in re.finditer(r'table\s+(\w+)\s*\{([^}]+)\}', content):
            table_name = match.group(1)
            fields_str = match.group(2)
            fields = self._parse_fields(fields_str)
            self.tables[table_name] = Table(table_name, fields, self.current_namespace)
            
    def _parse_fields(self, fields_str: str) -> List[Field]:
        """Parse field definitions from table body"""
        fields = []
        for line in fields_str.split(';'):
            line = line.strip()
            if not line or line.startswith('//'):
                continue
                
            # Pattern: field_name:type (attributes) = default
            match = re.match(r'(\w+)\s*:\s*(\[?)(\w+(?:\.\w+)?)\]?\s*(?:\(([^)]*)\))?\s*(?:=\s*(\S+))?', line)
            if match:
                name = match.group(1)
                is_list = match.group(2) == '['
                fbs_type = match.group(3)
                attrs = match.group(4) or ''
                default = match.group(5)
                
                is_required = 'required' in attrs
                
                # Determine Python type
                is_enum = False
                is_table = False
                is_union = False
                enum_namespace = None
                
                # Handle namespaced types (enums.DayCounter)
                if '.' in fbs_type:
                    parts = fbs_type.split('.')
                    enum_namespace = parts[0]
                    fbs_type_simple = parts[1]
                    if fbs_type_simple in self.enums or enum_namespace == 'enums':
                        is_enum = True
                        python_type = fbs_type_simple
                    else:
                        python_type = fbs_type_simple
                elif fbs_type in self.TYPE_MAP:
                    python_type = self.TYPE_MAP[fbs_type]
                elif fbs_type in self.enums:
                    is_enum = True
                    python_type = fbs_type
                elif fbs_type in self.unions:
                    is_union = True
                    python_type = fbs_type
                else:
                    is_table = True
                    python_type = fbs_type
                    
                if is_list:
                    python_type = f'List[{python_type}]'
                    
                fields.append(Field(
                    name=name,
                    fbs_type=fbs_type,
                    python_type=python_type,
                    is_required=is_required,
                    is_list=is_list,
                    is_enum=is_enum,
                    is_table=is_table,
                    is_union=is_union,
                    default=default,
                    enum_namespace=enum_namespace
                ))
                
        return fields


class PythonGenerator:
    """Generate Python wrapper classes"""
    
    def __init__(self, parser: FBSParser):
        self.parser = parser
        
    def generate_enums(self) -> str:
        """Generate Python Enum classes"""
        lines = [
            '"""Auto-generated enum definitions from FlatBuffers schema"""',
            '',
            'from enum import IntEnum',
            '',
        ]
        
        for enum in self.parser.enums.values():
            lines.append(f'class {enum.name}(IntEnum):')
            for i, value in enumerate(enum.values):
                lines.append(f'    {value} = {i}')
            lines.append('')
            
        return '\n'.join(lines)
    
    def generate_models(self) -> str:
        """Generate Python dataclass models"""
        lines = [
            '"""Auto-generated model definitions from FlatBuffers schema"""',
            '',
            'from __future__ import annotations',
            'from dataclasses import dataclass, field',
            'from typing import List, Optional, Union',
            '',
            'from .enums import *',
            '',
        ]
        
        # Sort tables by dependency order (simple approach: alphabetical for now)
        for table in sorted(self.parser.tables.values(), key=lambda t: t.name):
            lines.extend(self._generate_table(table))
            lines.append('')
            
        return '\n'.join(lines)
    
    def _generate_table(self, table: Table) -> List[str]:
        """Generate a single dataclass"""
        lines = [
            '@dataclass',
            f'class {table.name}:',
            f'    """Generated from {table.namespace}.{table.name}"""',
        ]
        
        # Sort fields: required first, then optional
        required_fields = [f for f in table.fields if f.is_required]
        optional_fields = [f for f in table.fields if not f.is_required]
        
        for f in required_fields + optional_fields:
            python_name = self._to_snake_case(f.name)
            type_hint = self._get_type_hint(f)
            
            if f.is_required:
                lines.append(f'    {python_name}: {type_hint}')
            else:
                default = self._get_default(f)
                lines.append(f'    {python_name}: {type_hint} = {default}')
                
        if not table.fields:
            lines.append('    pass')
            
        return lines
    
    def _get_type_hint(self, f: Field) -> str:
        """Get Python type hint for field"""
        if f.is_list:
            inner = f.python_type.replace('List[', '').rstrip(']')
            return f'List[{inner}]'
        elif f.is_required:
            return f.python_type
        else:
            return f'Optional[{f.python_type}]'
            
    def _get_default(self, f: Field) -> str:
        """Get default value for optional field"""
        if f.is_list:
            return 'field(default_factory=list)'
        elif f.default:
            if f.is_enum:
                return f'{f.python_type}.{f.default}'
            elif f.python_type == 'str':
                return f'"{f.default}"'
            elif f.python_type == 'bool':
                return f.default.lower()
            else:
                return f.default
        else:
            return 'None'
            
    def _to_snake_case(self, name: str) -> str:
        """Convert camelCase to snake_case"""
        s1 = re.sub('(.)([A-Z][a-z]+)', r'\1_\2', name)
        return re.sub('([a-z0-9])([A-Z])', r'\1_\2', s1).lower()

    def generate_serializers(self) -> str:
        """Generate serialization functions"""
        lines = [
            '"""Auto-generated serialization functions"""',
            '',
            'import flatbuffers',
            'from typing import List',
            '',
            'from .models import *',
            'from .enums import *',
            '',
            '# Import generated FlatBuffers code',
            'from quantra import (',
        ]
        
        # Import all tables
        for table_name in sorted(self.parser.tables.keys()):
            lines.append(f'    {table_name} as FB{table_name},')
        lines.append(')')
        lines.append('')
        lines.append('from quantra.enums import (')
        for enum_name in sorted(self.parser.enums.keys()):
            lines.append(f'    {enum_name} as FB{enum_name},')
        lines.append(')')
        lines.append('')
        
        # Generate serializer for each table
        for table in sorted(self.parser.tables.values(), key=lambda t: t.name):
            lines.extend(self._generate_serializer(table))
            lines.append('')
            
        return '\n'.join(lines)
    
    def _generate_serializer(self, table: Table) -> List[str]:
        """Generate serializer function for a table"""
        func_name = f'serialize_{self._to_snake_case(table.name)}'
        lines = [
            f'def {func_name}(builder: flatbuffers.Builder, obj: {table.name}) -> int:',
            f'    """Serialize {table.name} to FlatBuffer"""',
        ]
        
        # Pre-create strings and nested objects
        pre_creates = []
        for f in table.fields:
            snake_name = self._to_snake_case(f.name)
            if f.python_type == 'str' or (f.is_list and 'str' in f.python_type):
                pre_creates.append(f)
            elif f.is_table and not f.is_list:
                pre_creates.append(f)
            elif f.is_list and f.is_table:
                pre_creates.append(f)
                
        for f in pre_creates:
            snake_name = self._to_snake_case(f.name)
            if f.python_type == 'str':
                lines.append(f'    {snake_name}_off = builder.CreateString(obj.{snake_name}) if obj.{snake_name} else None')
            elif f.is_table and not f.is_list:
                inner_func = f'serialize_{self._to_snake_case(f.fbs_type)}'
                lines.append(f'    {snake_name}_off = {inner_func}(builder, obj.{snake_name}) if obj.{snake_name} else None')
            elif f.is_list:
                inner_type = f.fbs_type
                if f.is_table:
                    inner_func = f'serialize_{self._to_snake_case(inner_type)}'
                    lines.append(f'    {snake_name}_offsets = [{inner_func}(builder, x) for x in obj.{snake_name}] if obj.{snake_name} else []')
                    lines.append(f'    FB{table.name}.Start{f.name}Vector(builder, len({snake_name}_offsets))')
                    lines.append(f'    for off in reversed({snake_name}_offsets):')
                    lines.append(f'        builder.PrependUOffsetTRelative(off)')
                    lines.append(f'    {snake_name}_off = builder.EndVector()')
                    
        # Start building
        lines.append(f'    FB{table.name}.Start(builder)')
        
        for f in table.fields:
            snake_name = self._to_snake_case(f.name)
            fbs_add = f'FB{table.name}.Add{f.name}'
            
            if f.python_type == 'str':
                lines.append(f'    if {snake_name}_off: {fbs_add}(builder, {snake_name}_off)')
            elif f.is_enum:
                lines.append(f'    if obj.{snake_name} is not None: {fbs_add}(builder, obj.{snake_name}.value)')
            elif f.is_table or f.is_list:
                lines.append(f'    if {snake_name}_off: {fbs_add}(builder, {snake_name}_off)')
            else:
                lines.append(f'    if obj.{snake_name} is not None: {fbs_add}(builder, obj.{snake_name})')
                
        lines.append(f'    return FB{table.name}.End(builder)')
        
        return lines

    def _to_snake_case(self, name: str) -> str:
        """Convert camelCase to snake_case"""
        s1 = re.sub('(.)([A-Z][a-z]+)', r'\1_\2', name)
        return re.sub('([a-z0-9])([A-Z])', r'\1_\2', s1).lower()


def main():
    import argparse
    
    parser = argparse.ArgumentParser(description='Generate Python wrappers from FBS schemas')
    parser.add_argument('fbs_dir', help='Directory containing .fbs files')
    parser.add_argument('output_dir', help='Output directory for generated Python files')
    args = parser.parse_args()
    
    # Parse schemas
    fbs_parser = FBSParser(args.fbs_dir)
    fbs_parser.parse_all()
    
    # Generate Python
    generator = PythonGenerator(fbs_parser)
    
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    
    # Write enums
    (output_dir / 'enums.py').write_text(generator.generate_enums())
    print(f"Generated {output_dir / 'enums.py'}")
    
    # Write models
    (output_dir / 'models.py').write_text(generator.generate_models())
    print(f"Generated {output_dir / 'models.py'}")
    
    # Write serializers (basic version)
    # (output_dir / 'serializers.py').write_text(generator.generate_serializers())
    # print(f"Generated {output_dir / 'serializers.py'}")
    
    print(f"\nParsed {len(fbs_parser.enums)} enums, {len(fbs_parser.tables)} tables, {len(fbs_parser.unions)} unions")


if __name__ == '__main__':
    main()
