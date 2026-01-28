#!/usr/bin/env python3
"""
OpenSprinkler ESP32-C5 Memory Map Analyzer
Analyzes linker .map file and generates detailed memory usage reports
"""

import re
import sys
from collections import defaultdict
from pathlib import Path

class MemoryMapAnalyzer:
    def __init__(self, map_file_path):
        self.map_file_path = Path(map_file_path)
        self.sections = defaultdict(list)
        self.symbols = []
        self.memory_config = {}
        self.total_sizes = defaultdict(int)
        
    def parse_map_file(self):
        """Parse the linker map file"""
        if not self.map_file_path.exists():
            print(f"Error: Map file not found: {self.map_file_path}")
            return False
            
        print(f"Parsing {self.map_file_path}...")
        
        with open(self.map_file_path, 'r', encoding='utf-8', errors='ignore') as f:
            content = f.read()
            
        # Parse memory configuration
        self._parse_memory_config(content)
        
        # Parse sections
        self._parse_sections(content)
        
        # Parse symbols
        self._parse_symbols(content)
        
        return True
    
    def _parse_memory_config(self, content):
        """Extract memory configuration from map file"""
        memory_section = re.search(r'Memory Configuration(.*?)Linker script and memory map', content, re.DOTALL)
        if memory_section:
            lines = memory_section.group(1).strip().split('\n')
            for line in lines:
                match = re.match(r'\s*(\w+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)', line)
                if match:
                    name, origin, length = match.groups()
                    self.memory_config[name] = {
                        'origin': int(origin, 16),
                        'length': int(length, 16)
                    }
    
    def _parse_sections(self, content):
        """Parse section allocations"""
        # Look for section headers like ".text", ".data", ".bss", etc.
        section_pattern = r'^(\.\w+(?:\.\w+)*)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)'
        
        for match in re.finditer(section_pattern, content, re.MULTILINE):
            section_name = match.group(1)
            address = int(match.group(2), 16)
            size = int(match.group(3), 16)
            
            if size > 0:
                self.sections[section_name].append({
                    'address': address,
                    'size': size
                })
                self.total_sizes[section_name] += size
    
    def _parse_symbols(self, content):
        """Parse symbol table"""
        # Pattern: address size file:symbol
        symbol_pattern = r'^\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(.*?)$'
        
        for match in re.finditer(symbol_pattern, content, re.MULTILINE):
            address = int(match.group(1), 16)
            size = int(match.group(2), 16)
            info = match.group(3).strip()
            
            if size > 0:
                self.symbols.append({
                    'address': address,
                    'size': size,
                    'info': info
                })
    
    def categorize_symbols(self):
        """Categorize symbols by component"""
        categories = defaultdict(lambda: {'size': 0, 'count': 0, 'symbols': []})
        
        psram_categories = defaultdict(lambda: {'size': 0, 'count': 0, 'symbols': []})
        flash_categories = defaultdict(lambda: {'size': 0, 'count': 0, 'symbols': []})
        
        for sym in self.symbols:
            info = sym['info'].lower()
            size = sym['size']
            address = sym['address']
            
            # Determine memory region
            region = 'unknown'
            if 0x3C000000 <= address < 0x3E000000:  # PSRAM range
                region = 'PSRAM'
            elif 0x40000000 <= address < 0x44000000:  # Flash/ROM range
                region = 'Flash'
            elif 0x4FF00000 <= address < 0x50000000:  # DRAM range
                region = 'DRAM'
            
            # Categorize by component
            category = 'Other'
            if 'matter' in info or 'chip' in info:
                category = 'Matter'
            elif 'opensprinkler' in info or 'main.cpp' in info:
                category = 'OpenSprinkler Core'
            elif 'sensor' in info:
                category = 'Sensors'
            elif 'openthings' in info or 'otf' in info:
                category = 'OpenThings Framework'
            elif 'wifi' in info or 'network' in info:
                category = 'WiFi/Network'
            elif 'ssl' in info or 'tls' in info or 'mbedtls' in info:
                category = 'SSL/TLS'
            elif 'ble' in info or 'bluetooth' in info:
                category = 'BLE'
            elif 'mqtt' in info:
                category = 'MQTT'
            elif 'websocket' in info:
                category = 'WebSocket'
            elif '.a(' in info:  # Library file
                # Extract library name
                lib_match = re.search(r'lib(\w+)\.a\(', info)
                if lib_match:
                    category = f'Lib: {lib_match.group(1)}'
            
            categories[category]['size'] += size
            categories[category]['count'] += 1
            categories[category]['symbols'].append(sym)
            
            # Also categorize by memory region
            if region == 'PSRAM':
                psram_categories[category]['size'] += size
                psram_categories[category]['count'] += 1
                psram_categories[category]['symbols'].append(sym)
            elif region == 'Flash':
                flash_categories[category]['size'] += size
                flash_categories[category]['count'] += 1
                flash_categories[category]['symbols'].append(sym)
        
        return categories, psram_categories, flash_categories
    
    def generate_report(self):
        """Generate comprehensive memory report"""
        print("\n" + "="*80)
        print("OpenSprinkler ESP32-C5 Memory Map Analysis")
        print("="*80)
        
        # Memory Configuration
        print("\n📍 Memory Configuration:")
        print("-" * 80)
        for name, config in sorted(self.memory_config.items()):
            print(f"  {name:20s} Origin: 0x{config['origin']:08X}  Length: {config['length']:>10,} bytes ({config['length']/1024/1024:.2f} MB)")
        
        # Section Summary
        print("\n📊 Section Summary:")
        print("-" * 80)
        print(f"{'Section':<30s} {'Size':>15s} {'Percentage':>12s}")
        print("-" * 80)
        
        total_size = sum(self.total_sizes.values())
        for section, size in sorted(self.total_sizes.items(), key=lambda x: x[1], reverse=True):
            if size > 0:
                pct = (size / total_size * 100) if total_size > 0 else 0
                print(f"{section:<30s} {size:>10,} bytes {pct:>10.2f}%")
        
        print("-" * 80)
        print(f"{'TOTAL':<30s} {total_size:>10,} bytes")
        
        # Component Analysis
        categories, psram_categories, flash_categories = self.categorize_symbols()
        
        print("\n🔧 Component Memory Usage (All Regions):")
        print("-" * 80)
        print(f"{'Component':<40s} {'Size':>15s} {'Count':>8s} {'Avg Size':>12s}")
        print("-" * 80)
        
        for category, data in sorted(categories.items(), key=lambda x: x[1]['size'], reverse=True):
            avg_size = data['size'] / data['count'] if data['count'] > 0 else 0
            print(f"{category:<40s} {data['size']:>10,} bytes {data['count']:>6d} {avg_size:>10.1f} B")
        
        # PSRAM Usage
        if psram_categories:
            print("\n💾 PSRAM Usage by Component:")
            print("-" * 80)
            print(f"{'Component':<40s} {'Size':>15s} {'Count':>8s}")
            print("-" * 80)
            
            total_psram = sum(c['size'] for c in psram_categories.values())
            for category, data in sorted(psram_categories.items(), key=lambda x: x[1]['size'], reverse=True):
                pct = (data['size'] / total_psram * 100) if total_psram > 0 else 0
                print(f"{category:<40s} {data['size']:>10,} bytes {data['count']:>6d}  ({pct:>5.1f}%)")
            
            print("-" * 80)
            print(f"{'TOTAL PSRAM':<40s} {total_psram:>10,} bytes")
        
        # Top 20 largest symbols
        print("\n🔝 Top 20 Largest Symbols:")
        print("-" * 80)
        print(f"{'Size':>12s}  {'Address':>10s}  {'Symbol'}")
        print("-" * 80)
        
        sorted_symbols = sorted(self.symbols, key=lambda x: x['size'], reverse=True)[:20]
        for sym in sorted_symbols:
            addr_str = f"0x{sym['address']:08X}"
            info = sym['info'][:60]  # Truncate long names
            print(f"{sym['size']:>10,} B  {addr_str}  {info}")
        
        # Matter-specific analysis
        matter_symbols = [s for s in self.symbols if 'matter' in s['info'].lower() or 'chip' in s['info'].lower()]
        if matter_symbols:
            matter_total = sum(s['size'] for s in matter_symbols)
            print(f"\n🏠 Matter Protocol Total: {matter_total:,} bytes ({len(matter_symbols)} symbols)")
        
        # OpenThings Framework analysis
        otf_symbols = [s for s in self.symbols if 'openthings' in s['info'].lower() or 'otf' in s['info'].lower()]
        if otf_symbols:
            otf_total = sum(s['size'] for s in otf_symbols)
            print(f"🌐 OpenThings Framework Total: {otf_total:,} bytes ({len(otf_symbols)} symbols)")
        
        # Sensor analysis
        sensor_symbols = [s for s in self.symbols if 'sensor' in s['info'].lower()]
        if sensor_symbols:
            sensor_total = sum(s['size'] for s in sensor_symbols)
            print(f"📡 Sensors Total: {sensor_total:,} bytes ({len(sensor_symbols)} symbols)")
        
        print("\n" + "="*80)
        print(f"Analysis complete. Total symbols analyzed: {len(self.symbols):,}")
        print("="*80 + "\n")

def main():
    map_file = Path(".pio/build/esp32-c5/firmware.map")
    
    if not map_file.exists():
        print(f"Error: Map file not found: {map_file}")
        print("Please build the project first with: pio run -e esp32-c5")
        sys.exit(1)
    
    analyzer = MemoryMapAnalyzer(map_file)
    
    if analyzer.parse_map_file():
        analyzer.generate_report()
    else:
        sys.exit(1)

if __name__ == "__main__":
    main()
