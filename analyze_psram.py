#!/usr/bin/env python3
"""
OpenSprinkler ESP32-C5 PSRAM Memory Analysis
Detaillierte Analyse der PSRAM-Nutzung aus der Linker-Map
"""

import re
from pathlib import Path

def analyze_psram_usage():
    map_file = Path(".pio/build/esp32-c5/firmware.map")
    
    if not map_file.exists():
        print(f"❌ Map-Datei nicht gefunden: {map_file}")
        return
    
    print("=" * 100)
    print("OpenSprinkler ESP32-C5 PSRAM Memory Analysis")
    print("=" * 100)
    
    with open(map_file, 'r', encoding='utf-8', errors='ignore') as f:
        content = f.read()
    
    # Finde Memory Configuration Ausgabe
    memory_usage_match = re.search(
        r'Memory region\s+Used Size\s+Region Size\s+%age Used\s+(.*?)(?=Retrieving|Checking|$)',
        content,
        re.DOTALL
    )
    
    if memory_usage_match:
        print("\n📊 Memory Region Usage (vom Linker):")
        print("-" * 100)
        print(f"{'Region':<20s} {'Used Size':>15s} {'Total Size':>15s} {'Usage %':>10s}")
        print("-" * 100)
        
        lines = memory_usage_match.group(1).strip().split('\n')
        for line in lines:
            line = line.strip()
            if not line:
                continue
            
            match = re.match(r'(\w+):\s+(\d+)\s+B\s+(\d+)\s+B\s+([\d.]+)%', line)
            if match:
                region, used, total, pct = match.groups()
                used_int = int(used)
                total_int = int(total)
                
                # Hervorhebung für PSRAM (extern_ram_seg)
                marker = "🔥" if region == "extern_ram_seg" else "  "
                
                print(f"{marker} {region:<18s} {used_int:>10,} bytes {total_int:>10,} bytes {pct:>8s}%")
    
    # Finde ext_ram.dummy Section (PSRAM usage)
    ext_ram_section = re.search(
        r'\.ext_ram\.dummy\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)',
        content
    )
    
    if ext_ram_section:
        addr = int(ext_ram_section.group(1), 16)
        size = int(ext_ram_section.group(2), 16)
        
        print(f"\n💾 PSRAM Section (.ext_ram.dummy):")
        print("-" * 100)
        print(f"  Address: 0x{addr:08X}")
        print(f"  Size:    {size:,} bytes ({size/1024:.2f} KB / {size/1024/1024:.2f} MB)")
    
    # Finde alle EXT_RAM_BSS_ATTR Symbole
    print("\n🔍 PSRAM-allocated Variables (EXT_RAM_BSS_ATTR):")
    print("-" * 100)
    
    # Suche nach Symbolen in PSRAM-Bereich (0x3C000000 - 0x3E000000)
    # Oder nach explizit markierten extram Symbolen
    extram_symbols = []
    
    # Pattern für Symbol-Einträge in der Map
    symbol_pattern = r'^\s+0x([0-9a-fA-F]+)\s+(\S.*?)$'
    
    in_extram_section = False
    for line in content.split('\n'):
        # Prüfe ob wir in einer .extram Section sind
        if re.match(r'^\.(ext_ram|extram)', line):
            in_extram_section = True
        elif line.startswith('.') and not line.startswith('.ext'):
            in_extram_section = False
        
        # Wenn wir in einer extram Section sind, sammle Symbole
        if in_extram_section:
            match = re.search(r'0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(.+)$', line)
            if match:
                addr = int(match.group(1), 16)
                size = int(match.group(2), 16)
                name = match.group(3).strip()
                
                if size > 0 and 0x3C000000 <= addr < 0x3E000000:
                    extram_symbols.append({
                        'address': addr,
                        'size': size,
                        'name': name
                    })
    
    if extram_symbols:
        print(f"  Gefunden: {len(extram_symbols)} Symbole")
        print(f"\n{'Size':>12s}  {'Address':>10s}  {'Symbol'}")
        print("-" * 100)
        
        sorted_symbols = sorted(extram_symbols, key=lambda x: x['size'], reverse=True)
        for sym in sorted_symbols[:30]:  # Top 30
            print(f"{sym['size']:>10,} B  0x{sym['address']:08X}  {sym['name'][:80]}")
        
        total_extram = sum(s['size'] for s in extram_symbols)
        print("-" * 100)
        print(f"{'TOTAL PSRAM':<15s} {total_extram:>10,} bytes ({total_extram/1024:.2f} KB)")
    else:
        print("  ⚠️  Keine expliziten PSRAM-Variablen gefunden (möglicherweise dynamisch alloziert)")
    
    # Analyse Matter-Symbole
    print("\n🏠 Matter Protocol Analysis:")
    print("-" * 100)
    
    matter_pattern = r'(matter|chip|CHIP)[\w_]*'
    matter_symbols = []
    
    # Suche alle Matter-bezogenen Symbole
    for match in re.finditer(r'0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+.*?(matter|chip|CHIP)[^\n]+', content, re.IGNORECASE):
        addr = int(match.group(1), 16)
        size = int(match.group(2), 16)
        line = match.group(0)
        
        if size > 0:
            matter_symbols.append({
                'address': addr,
                'size': size,
                'info': line
            })
    
    if matter_symbols:
        total_matter = sum(s['size'] for s in matter_symbols)
        matter_in_psram = [s for s in matter_symbols if 0x3C000000 <= s['address'] < 0x3E000000]
        total_matter_psram = sum(s['size'] for s in matter_in_psram)
        
        print(f"  Total Matter Symbols: {len(matter_symbols):,}")
        print(f"  Total Matter Size:    {total_matter:,} bytes ({total_matter/1024:.2f} KB)")
        print(f"  Matter in PSRAM:      {len(matter_in_psram):,} symbols, {total_matter_psram:,} bytes ({total_matter_psram/1024:.2f} KB)")
    
    # Analyse OpenSprinkler-spezifische Symbole
    print("\n🌱 OpenSprinkler Core Analysis:")
    print("-" * 100)
    
    os_keywords = ['opensprinkler', 'sensor', 'station', 'program', 'mqtt', 'otf', 'openthings']
    os_symbols = []
    
    for keyword in os_keywords:
        pattern = rf'0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+.*?{keyword}[^\n]+'
        for match in re.finditer(pattern, content, re.IGNORECASE):
            addr = int(match.group(1), 16)
            size = int(match.group(2), 16)
            line = match.group(0)
            
            if size > 0:
                os_symbols.append({
                    'address': addr,
                    'size': size,
                    'keyword': keyword,
                    'info': line
                })
    
    if os_symbols:
        total_os = sum(s['size'] for s in os_symbols)
        os_in_psram = [s for s in os_symbols if 0x3C000000 <= s['address'] < 0x3E000000]
        total_os_psram = sum(s['size'] for s in os_in_psram)
        
        print(f"  Total OpenSprinkler Symbols: {len(os_symbols):,}")
        print(f"  Total OS Size:                {total_os:,} bytes ({total_os/1024:.2f} KB)")
        print(f"  OS in PSRAM:                  {len(os_in_psram):,} symbols, {total_os_psram:,} bytes ({total_os_psram/1024:.2f} KB)")
        
        # Gruppierung nach Keyword
        print(f"\n  Breakdown by Component:")
        by_keyword = {}
        for s in os_symbols:
            kw = s['keyword']
            if kw not in by_keyword:
                by_keyword[kw] = {'count': 0, 'size': 0}
            by_keyword[kw]['count'] += 1
            by_keyword[kw]['size'] += s['size']
        
        for kw in sorted(by_keyword.keys()):
            data = by_keyword[kw]
            print(f"    {kw:<20s} {data['size']:>10,} bytes ({data['count']:>4d} symbols)")
    
    # Zusammenfassung
    print("\n" + "=" * 100)
    print("📌 ZUSAMMENFASSUNG:")
    print("=" * 100)
    print("""
Die PSRAM-Nutzung erfolgt hauptsächlich über:
  1. Statische Variablen mit EXT_RAM_BSS_ATTR (in .ext_ram.dummy Section)
  2. Dynamische Allokation via heap_caps_malloc(MALLOC_CAP_SPIRAM)
     - Matter Endpoints (std::unique_ptr mit placement new)
     - std::unordered_map mit PSRAMAllocator
     - OpenThings Framework Buffers
  3. mbedTLS Buffers >1KB (via esp_mbedtls_mem_alloc Hook)

Hinweis: Die Map-Datei zeigt nur statische Allocations (.ext_ram.dummy).
Runtime PSRAM-Nutzung durch heap_caps_malloc ist deutlich höher!

Erwartete Runtime PSRAM-Nutzung:
  - Matter Endpoints:      ~40-50 KB (Maps + Objects)
  - OpenThings Buffers:    ~12 KB pro Client
  - Large Buffers:         ~8 KB (ether_buffer + tmp_buffer)
  - SSL/TLS:               Variable (je nach Verbindungen)
  - GESAMT:                ~60-100+ KB
""")
    
    print("=" * 100 + "\n")

if __name__ == "__main__":
    analyze_psram_usage()
