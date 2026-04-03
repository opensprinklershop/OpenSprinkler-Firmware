import os
import re
import subprocess
Import("env")

print("=> Enabling LTO (Link-Time Optimization) for ESP8266 — two-pass mode...")

# =============================================================================
# HOW IT WORKS — Two-pass LTO for ESP8266
# =============================================================================
#
# The pre-compiled Newlib libc.a / libgcc.a and SDK libraries shipped with the
# ESP8266 Arduino Core were compiled without LTO.  When the GCC LTO linker
# plugin is active, it disrupts Xtensa section placement (literal/text ordering)
# inside these non-LTO archives, causing fatal "l32r: literal placed after use"
# relocations and undefined symbol errors.
#
# Solution: split the link into two passes.
#
#   Pass 1 (LTO combine):
#     xtensa-lx106-elf-gcc -flto -fuse-linker-plugin -Os -r -nostdlib \
#         -o lto_combined.o  <all LTO objects and archives>
#     This runs the LTO optimiser on ALL our code (project sources + libraries)
#     and emits a single relocatable object with real machine code.
#
#   Pass 2 (standard link):
#     xtensa-lx106-elf-gcc -nostdlib <normal ESP8266 flags> \
#         -o firmware.elf  lto_combined.o -lhal ... -lc -lgcc
#     Because there is NO LTO plugin in this pass, the standard ESP8266 linker
#     script controls section placement and l32r relocations resolve correctly.
#
# =============================================================================

# --- 1. Compile with LTO, optimisation and text-section-literals -----------
env.Append(
    CCFLAGS=[
        "-flto",
        "-mtext-section-literals",
        "-Os",
        "-ffunction-sections",
        "-fdata-sections",
        "-fno-exceptions",
    ],
    CXXFLAGS=[
        "-fno-rtti",
    ],
)

# --- 2. Use GCC-aware archive tools so LTO metadata survives in .a files ---
env.Replace(
    AR="xtensa-lx106-elf-gcc-ar",
    RANLIB="xtensa-lx106-elf-gcc-ranlib",
    NM="xtensa-lx106-elf-gcc-nm",
)

# --- 3. Override the link action with two-pass LTO -------------------------

def two_pass_lto_link(target, source, env):
    """Two-pass LTO link for ESP8266."""
    target_elf = str(target[0])
    build_dir = env.subst("$BUILD_DIR")
    # IMPORTANT: name the output *.cpp.o so the ESP8266 linker script's
    # *.cpp.o(.literal* .text*) pattern places it into .irom0.text (flash)
    # rather than .text1 (32 KB IRAM)
    combined_o = os.path.join(build_dir, "lto_combined.cpp.o")

    # Get the C++ compiler (used as the linker driver)
    cxx = env.subst("$CXX")
    # Full path if needed
    if "xtensa" not in cxx:
        toolchain_dir = env.PioPlatform().get_package_dir("toolchain-xtensa")
        cxx = os.path.join(toolchain_dir, "bin", "xtensa-lx106-elf-g++")

    # Classify inputs: LTO objects/archives vs non-LTO SDK libraries
    lto_inputs = []        # project .o and PlatformIO-built .a files (contain LTO bytecode)
    sdk_lib_flags = []     # flags for pre-compiled SDK libraries
    sdk_lib_dirs = []      # -L flags (expanded)

    pio_build_dir = os.path.abspath(build_dir)

    # Use SCons LIBS and LIBPATH directly — avoids breaking on spaces in paths
    for raw_lib in env.get("LIBS", []):
        # PlatformIO wraps some lib entries in containers (list, NodeList, CLVar)
        if hasattr(raw_lib, '__iter__') and not isinstance(raw_lib, (str, bytes)):
            items = list(raw_lib)
        else:
            items = [raw_lib]

        for lib in items:
            # Get absolute path — handle both SCons File nodes and strings
            if hasattr(lib, 'abspath'):
                abs_path = lib.abspath
                lib_str = abs_path
            elif hasattr(lib, 'get_abspath'):
                abs_path = lib.get_abspath()
                lib_str = abs_path
            else:
                lib_str = env.subst(str(lib))
                abs_path = os.path.abspath(lib_str) if not os.path.isabs(lib_str) else lib_str

            # Check if it's a full path to a .a file
            if abs_path.endswith(".a") and os.path.isfile(abs_path):
                if abs_path.startswith(pio_build_dir):
                    # PlatformIO-built library → has LTO bytecode
                    lto_inputs.append(abs_path)
                else:
                    # Non-PIO library → non-LTO
                    sdk_lib_flags.append(abs_path)
            elif lib_str.endswith(".a"):
                # Path-style but not absolute — try to resolve
                if lib_str.startswith(".pio/") or pio_build_dir in abs_path:
                    lto_inputs.append(abs_path)
                else:
                    sdk_lib_flags.append(abs_path)
            else:
                # Simple library name → becomes -l flag (SDK/toolchain)
                sdk_lib_flags.append("-l" + lib_str)

    for lp in env.get("LIBPATH", []):
        expanded = env.subst(str(lp))
        sdk_lib_dirs.append("-L" + expanded)

    # Collect project source objects
    for src in source:
        src_path = str(src)
        if src_path.endswith(".o"):
            lto_inputs.append(src_path)

    # Debug: show classification
    print("   LTO inputs (%d): %s" % (len(lto_inputs),
          ", ".join(os.path.basename(x) for x in lto_inputs[:5]) + ("..." if len(lto_inputs) > 5 else "")))
    print("   SDK libs (%d): %s" % (len(sdk_lib_flags),
          " ".join(sdk_lib_flags[:8]) + ("..." if len(sdk_lib_flags) > 8 else "")))
    print("   Lib dirs (%d): %s" % (len(sdk_lib_dirs),
          " ".join(sdk_lib_dirs[:3]) + ("..." if len(sdk_lib_dirs) > 3 else "")))

    print("=> Pass 1: LTO combine (%d objects/archives)" % len(lto_inputs))

    # Force materialisation of symbols needed by non-LTO SDK archives.
    # We create a tiny NON-LTO object that references these symbols.
    # This prevents the LTO Whole Program Analyser from removing them,
    # because non-LTO references are treated as external and must be kept.
    forced_symbols = [
        "malloc", "free", "calloc", "realloc",
        "_malloc_r", "_free_r", "_calloc_r", "_realloc_r",
        "_read_r", "_write_r", "_lseek_r", "_close_r", "_fstat_r",
        "abort", "_exit",
        "postmortem_report",  # Referenced by -Wl,-wrap,system_restart_local
    ]
    bridge_c = os.path.join(build_dir, "lto_bridge_nolto.c")
    bridge_o = os.path.join(build_dir, "lto_bridge_nolto.o")
    with open(bridge_c, "w") as f:
        f.write("/* Auto-generated: force LTO to keep symbols needed by SDK */\n")
        for sym in forced_symbols:
            f.write("extern void* %s;\n" % sym)
        f.write("void* volatile __lto_force_syms[] __attribute__((used, section(\".rodata\"))) = {\n")
        for sym in forced_symbols:
            f.write("    (void*)&%s,\n" % sym)
        f.write("};\n")

    cc = env.subst("$CC")
    if "xtensa" not in cc:
        cc = os.path.join(env.PioPlatform().get_package_dir("toolchain-xtensa"),
                          "bin", "xtensa-lx106-elf-gcc")
    ret = subprocess.call([cc, "-c", "-Os", "-mlongcalls", "-Wno-builtin-declaration-mismatch", bridge_c, "-o", bridge_o])
    if ret != 0:
        print("=> ERROR: Failed to compile LTO bridge (exit %d)" % ret)
        return ret
    print("   Non-LTO bridge object compiled: %s" % os.path.basename(bridge_o))

    # Separate .o source files from .a archives
    lto_objects = [x for x in lto_inputs if x.endswith(".o")]
    lto_archives = [x for x in lto_inputs if x.endswith(".a")]

    # Also force C++ virtual methods that vtables reference.
    # These are in LTO archives but LTO may discard them since
    # they appear unreachable despite being in vtables.
    cpp_forced = []
    for sym in [
        "_ZN6String10invalidateEv",       # String::invalidate()
        "_ZN8ENC28J609sendFrameEPKht",    # ENC28J60::sendFrame()
        "_ZN8ENC28J609readFrameEPht",     # ENC28J60::readFrame()
        "_ZN11OLEDDisplay5writeEh",       # OLEDDisplay::write(uint8_t)
        "_ZN5Print5writeEPKhj",           # Print::write(const uint8_t*, size_t)
        "_ZN9DNSServer17setErrorReplyCodeERK12DNSReplyCode",  # DNSServer::setErrorReplyCode()
    ]:
        cpp_forced += ["-Wl,-u," + sym]

    # ---- Pass 1: LTO combine via gcc -r -flto ----
    #
    # BACKGROUND: GCC's -r with -flto normally produces GIMPLE bytecode
    # (not native machine code) because the lto-plugin internally uses
    # -flinker-output=rel.  We work around this by installing a thin shim
    # wrapper around lto-wrapper that replaces -flinker-output=rel with
    # -flinker-output=nolto-rel, forcing full LTO compilation (WPA + LTRANS)
    # to native Xtensa machine code.  The shim is installed once and
    # restored after the build.

    toolchain_dir = env.PioPlatform().get_package_dir("toolchain-xtensa")
    lto_wrapper_dir = os.path.join(toolchain_dir, "libexec", "gcc",
                                   "xtensa-lx106-elf", "10.3.0")
    lto_wrapper_real = os.path.join(lto_wrapper_dir, "lto-wrapper-real")
    lto_wrapper_shim = os.path.join(lto_wrapper_dir, "lto-wrapper")

    # Install shim if not already present
    shim_installed = False
    # Always rewrite the shim to ensure latest version
    shim_needs_update = True
    if not os.path.isfile(lto_wrapper_real):
        import shutil
        shutil.copy2(lto_wrapper_shim, lto_wrapper_real)
    if shim_needs_update:
        with open(lto_wrapper_shim, "w") as f:
            f.write('#!/bin/bash\n')
            f.write('DIR="$(dirname "$(readlink -f "$0")")"\n')
            f.write('args=()\n')
            f.write('for arg in "$@"; do\n')
            f.write('    if [[ "$arg" == @* ]]; then\n')
            f.write('        RFILE="${arg#@}"\n')
            f.write('        if [[ -f "$RFILE" ]]; then\n')
            f.write('            sed -i \'s/-flinker-output=rel/-flinker-output=nolto-rel/g\' "$RFILE"\n')
            f.write('        fi\n')
            f.write('        args+=("$arg")\n')
            f.write('    elif [[ "$arg" == "-flinker-output=rel" ]]; then\n')
            f.write('        args+=("-flinker-output=nolto-rel")\n')
            f.write('    else\n')
            f.write('        args+=("$arg")\n')
            f.write('    fi\n')
            f.write('done\n')
            f.write('exec "$DIR/lto-wrapper-real" "${args[@]}"\n')
        os.chmod(lto_wrapper_shim, 0o755)
        shim_installed = True
        print("   lto-wrapper shim installed")

    # Split archives into the Arduino framework (needs --whole-archive)
    # and other libraries.
    #
    # Strategy:
    # - Framework archives are included in Pass 1 with --whole-archive
    #   because they contain heap/syscall stubs that LTO would discard.
    # - Non-framework PIO archives are NOT included in Pass 1. Instead,
    #   they are individually compiled from LTO GIMPLE to native code
    #   (per-archive gcc -flto -r), then included in Pass 2.
    #   This is necessary because:
    #   (a) Archive member extraction in -r LTO mode is unreliable
    #   (b) Including all archives in Pass 1 via --whole-archive or
    #       extracted objects triggers an ICE in the LTO partitioner
    #
    # IMPORTANT: umm_malloc.cpp.o is extracted from the framework archive
    # and kept separate for Pass 2.  The ESP8266 linker script uses
    # EXCLUDE_FILE (umm_malloc.cpp.o) to keep the heap allocator in IRAM.
    # If it's merged into lto_combined.cpp.o, the filename matching fails
    # and umm_malloc ends up in flash → crash at boot (umm_init runs
    # before flash cache is ready).
    framework_archives = []
    other_archives = []
    for a in lto_archives:
        if "libFrameworkArduino" in os.path.basename(a):
            framework_archives.append(a)
        else:
            other_archives.append(a)

    # Extract non-LTO objects from framework archive(s) before LTO combine.
    # Two categories must be separated:
    #   1. umm_malloc — needs GIMPLE→native compilation + section rename for IRAM
    #   2. Assembly objects (.S.o) — already native, no LTO metadata possible;
    #      including them in the pure-LTO Pass 1 causes the linker to downgrade
    #      from WPA (Whole Program Analysis) to nolto-rel, defeating optimisation.
    iram_native_objs = []   # native .o files that MUST keep their filename for linker script
    nolto_fw_objs = []      # assembly objects extracted to avoid LTO/non-LTO mix
    ar_tool = env.subst("$AR")
    if "gcc-ar" not in ar_tool:
        ar_tool = "xtensa-lx106-elf-gcc-ar"
    iram_dir = os.path.join(build_dir, "lto_iram_objs")
    nolto_dir = os.path.join(build_dir, "lto_nolto_fw")
    os.makedirs(iram_dir, exist_ok=True)
    os.makedirs(nolto_dir, exist_ok=True)

    objdump_tool = cxx.replace("g++", "objdump")

    patched_fw_archives = []
    for fw_a in framework_archives:
        import shutil
        # List members
        result = subprocess.run([ar_tool, "t", fw_a], capture_output=True, text=True)
        members = [m.strip() for m in result.stdout.strip().split("\n")
                   if m.strip()] if result.returncode == 0 else []
        umm_members = [m for m in members if "umm_malloc" in m]

        # Detect non-LTO (assembly) members by checking for .gnu.lto sections.
        # Extract each member to a temp dir, check, then classify.
        asm_members = []
        check_dir = os.path.join(build_dir, "lto_check_tmp")
        if os.path.isdir(check_dir):
            shutil.rmtree(check_dir)
        os.makedirs(check_dir, exist_ok=True)
        for m in members:
            if not m or m in umm_members:
                continue  # handled separately
            ret_ex = subprocess.call([ar_tool, "x", fw_a, m], cwd=check_dir)
            check_o = os.path.join(check_dir, m)
            if ret_ex != 0 or not os.path.isfile(check_o):
                continue
            res = subprocess.run([objdump_tool, "-h", check_o],
                                 capture_output=True, text=True)
            has_lto = "gnu.lto" in res.stdout if res.returncode == 0 else True
            os.remove(check_o)
            if not has_lto:
                asm_members.append(m)
                # Re-extract to nolto dir for Pass 2
                subprocess.call([ar_tool, "x", fw_a, m], cwd=nolto_dir)
                dst = os.path.join(nolto_dir, m)
                if os.path.isfile(dst):
                    nolto_fw_objs.append(dst)
        # Cleanup check dir
        shutil.rmtree(check_dir, ignore_errors=True)

        excluded_members = umm_members + asm_members

        if not excluded_members:
            patched_fw_archives.append(fw_a)
            continue

        # Extract umm_malloc objects (they contain LTO GIMPLE bytecode)
        for m in umm_members:
            subprocess.call([ar_tool, "x", fw_a, m], cwd=iram_dir)
            gimple_o = os.path.join(iram_dir, m)
            if os.path.isfile(gimple_o):
                # Compile GIMPLE → native code via gcc -flto -r
                # The lto-wrapper shim forces -flinker-output=nolto-rel
                # which produces real machine code instead of GIMPLE.
                # IMPORTANT: keep the output filename as umm_malloc.cpp.o
                # so the linker script EXCLUDE_FILE rule matches it.
                native_o = os.path.join(iram_dir, m)  # overwrite in-place
                tmp_o = os.path.join(iram_dir, m + ".tmp")
                os.rename(gimple_o, tmp_o)
                ret_umm = subprocess.call([
                    cxx, "-flto", "-fuse-linker-plugin",
                    "-Os", "-mlongcalls", "-mtext-section-literals",
                    "-ffunction-sections", "-fdata-sections",
                    "-nostdlib", "-r",
                    "-o", native_o,
                    tmp_o,
                ])
                if ret_umm == 0 and os.path.isfile(native_o):
                    # Rename .text.* sections → .iram.text.* so the linker
                    # script places them in IRAM (.text1) instead of flash
                    # (.irom0.text).  Without this, EXCLUDE_FILE doesn't
                    # match because our file path has a directory prefix.
                    objcopy_umm = cxx.replace("g++", "objcopy")
                    result_hdr = subprocess.run(
                        [objcopy_umm.replace("objcopy", "objdump"), "-h", native_o],
                        capture_output=True, text=True)
                    if result_hdr.returncode == 0:
                        rename_args = []
                        for line in result_hdr.stdout.splitlines():
                            parts = line.split()
                            if len(parts) >= 2:
                                sec = parts[1]
                                # Rename .text.xxx → .iram.text.xxx (not bare .text or .irom0.text.*)
                                if sec.startswith(".text.") and not sec.startswith(".irom0."):
                                    rename_args += ["--rename-section",
                                                    "%s=.iram.text.%s" % (sec, sec[len(".text."):])]
                                # Rename non-empty .literal.xxx → .iram.literal.xxx
                                elif sec.startswith(".literal.") and not sec.startswith(".irom0."):
                                    rename_args += ["--rename-section",
                                                    "%s=.iram.literal.%s" % (sec, sec[len(".literal."):])]
                        if rename_args:
                            ret_ren = subprocess.call([objcopy_umm] + rename_args + [native_o])
                            if ret_ren == 0:
                                print("   [LTO] Renamed %d sections → .iram.* for IRAM placement"
                                      % (len(rename_args) // 2))
                            else:
                                print("   [LTO] WARNING: objcopy rename failed (exit %d)" % ret_ren)

                    iram_native_objs.append(native_o)
                    os.remove(tmp_o)
                    print("   [LTO] Compiled IRAM object to native: %s" % m)
                else:
                    # Fallback: keep GIMPLE version (will be handled by LTO)
                    os.rename(tmp_o, gimple_o)
                    print("   [LTO] WARNING: failed to compile %s to native" % m)

        # Create a copy of the archive without excluded members
        patched_a = os.path.join(build_dir, "libFrameworkArduino_lto_only.a")
        shutil.copy2(fw_a, patched_a)
        for m in excluded_members:
            subprocess.call([ar_tool, "d", patched_a, m])
        patched_fw_archives.append(patched_a)
        print("   [LTO] Framework archive patched: removed %s" % ", ".join(excluded_members))
        if asm_members:
            print("   [LTO] Extracted %d assembly (non-LTO) objects for Pass 2: %s"
                  % (len(asm_members), ", ".join(asm_members)))

    # Force symbol retention via -Wl,-u flags.
    # NOTE: bridge_o (a non-LTO object) is INTENTIONALLY included in Pass 1
    # to disable WPA (Whole Program Analysis).  WPA breaks ESP8266's
    # yield()/cont_suspend() continuation mechanism — the assembly in
    # cont.S does non-local jumps (stack switching) that WPA can't see,
    # causing interprocedural optimisations to corrupt the yield path
    # (WDT reset on first yield after boot).  Without WPA, the LTO linker
    # still converts each GIMPLE TU to native code individually, giving
    # good per-TU optimisation without dangerous cross-module assumptions.
    forced_undef_flags = []
    for sym in forced_symbols:
        forced_undef_flags += ["-Wl,-u," + sym]

    cmd_pass1 = [
        cxx,
        "-flto",
        "-fuse-linker-plugin",
        "-Os",
        "-mlongcalls",
        "-mtext-section-literals",
        "-ffunction-sections",
        "-fdata-sections",
        "-nostdlib",
        "-r",                     # Relocatable output (shim forces native code)
        "-o", combined_o,
    ] + forced_undef_flags + cpp_forced + lto_objects + [
        bridge_o,                 # Non-LTO object — MUST be in Pass 1 to disable WPA
                                  # (see comment above forced_undef_flags)
    ] + [
        "-Wl,--whole-archive",
    ] + patched_fw_archives + [
        "-Wl,--no-whole-archive",
    ]

    print("   gcc -r -flto -> %s" % os.path.basename(combined_o))
    ret = subprocess.call(cmd_pass1)
    if ret != 0:
        print("=> ERROR: LTO combine pass failed (exit %d)" % ret)
        return ret

    final_size = os.path.getsize(combined_o)
    print("=> Pass 1 done: %s (%d bytes)" % (os.path.basename(combined_o), final_size))

    # Diagnostic: verify IRAM sections survived LTO
    res_sections = subprocess.run(
        [objdump_tool, "-h", combined_o],
        capture_output=True, text=True)
    if res_sections.returncode == 0:
        iram_secs = [l.split()[1] for l in res_sections.stdout.splitlines()
                     if len(l.split()) >= 2 and ".iram." in l.split()[1]]
        if iram_secs:
            print("   IRAM sections preserved (%d): %s" % (len(iram_secs),
                  ", ".join(iram_secs[:5]) + ("..." if len(iram_secs) > 5 else "")))
        else:
            print("   WARNING: No .iram.* sections found — IRAM functions may have been"
                  " inlined into flash!")

    # Globalize static symbols that are referenced via linker wraps or
    # inline assembly but which LTO demoted to local (internal) linkage.
    # Also weaken __wrap_system_restart_local so we can replace it with
    # a long-range version (the original uses `j` which can't span the
    # multi-megabyte combined object).
    objcopy = env.subst("$CC").replace("gcc", "objcopy")
    subprocess.call([
        objcopy,
        "--globalize-symbol=postmortem_report",
        "--weaken-symbol=__wrap_system_restart_local",
        combined_o,
    ])

    # Create a replacement __wrap_system_restart_local using l32r+jx
    # (indirect jump, unlimited range) instead of `j` (±128K range).
    fix_s = os.path.join(build_dir, "lto_postmortem_fix.S")
    fix_o = os.path.join(build_dir, "lto_postmortem_fix.o")
    with open(fix_s, "w") as f:
        f.write('.section .text.__wrap_system_restart_local,"ax",@progbits\n')
        f.write('.literal_position\n')
        f.write('.literal .Lpm_addr, postmortem_report\n')
        f.write('.align 4\n')
        f.write('.global __wrap_system_restart_local\n')
        f.write('.type __wrap_system_restart_local, @function\n')
        f.write('__wrap_system_restart_local:\n')
        f.write('    mov a2, a1\n')
        f.write('    l32r a0, .Lpm_addr\n')
        f.write('    jx a0\n')
        f.write('.size __wrap_system_restart_local, .-__wrap_system_restart_local\n')
    cc = env.subst("$CC")
    subprocess.call([cc, "-c", "-o", fix_o, fix_s])
    postmortem_fix_o = fix_o

    # ---- Pass 1b: Compile non-framework archives GIMPLE → native ----
    # Each PIO archive is individually compiled via gcc -flto -r so that
    # the lto-wrapper shim forces native code generation. This gives us
    # native relocatable objects that can participate in the standard Pass 2
    # link without needing the LTO plugin.
    native_lib_objs = []
    if other_archives:
        native_dir = os.path.join(build_dir, "lto_native_libs")
        os.makedirs(native_dir, exist_ok=True)
        for archive in other_archives:
            aname = os.path.basename(archive)
            native_o = os.path.join(native_dir, aname.replace(".a", ".cpp.o"))
            ret_lib = subprocess.call([
                cxx, "-flto", "-fuse-linker-plugin",
                "-Os", "-mlongcalls", "-mtext-section-literals",
                "-ffunction-sections", "-fdata-sections",
                "-nostdlib", "-r",
                "-Wl,--whole-archive",
                "-o", native_o,
                archive,
            ])
            if ret_lib == 0 and os.path.isfile(native_o):
                native_lib_objs.append(native_o)
            else:
                print("   WARNING: failed to compile %s to native" % aname)
        print("=> Pass 1b: compiled %d/%d library archives to native"
              % (len(native_lib_objs), len(other_archives)))

    # ---- Pass 2: standard link (NO LTO plugin) ----
    # Reconstruct the link command WITHOUT -flto, using the combined object
    # plus standard ESP8266 SDK libraries and linker scripts.

    # Collect non-LTO link flags from the standard environment
    # (linker scripts, wraps, sections, etc.)
    # Use the list directly — don't split a string (paths may have spaces)
    pass2_flags = []
    all_linkflags = env.get("LINKFLAGS", [])
    skip_next = False
    for i, f in enumerate(all_linkflags):
        f_str = str(f)
        if skip_next:
            skip_next = False
            continue
        # Skip all LTO-related flags
        if "-flto" in f_str or "-fuse-linker-plugin" in f_str:
            continue
        # Skip the -Wl,-T for our lto_extern_symbols.ld (not needed in pass 2)
        if "lto_extern" in f_str:
            continue
        pass2_flags.append(f_str)

    # SDK undefined symbols that the ESP8266 builder adds
    # These should already be in pass2_flags from LINKFLAGS, but add as safety net
    for sym in ["app_entry", "_printf_float", "_scanf_float",
                "_DebugExceptionVector", "_DoubleExceptionVector",
                "_KernelExceptionVector", "_NMIExceptionVector",
                "_UserExceptionVector"]:
        if not any(sym in f for f in pass2_flags):
            pass2_flags += ["-u", sym]

    cmd_pass2 = [
        cxx,
        "-nostdlib",
        "-mlongcalls",
    ] + pass2_flags + sdk_lib_dirs + [
        "-o", target_elf,
    ] + iram_native_objs + [    # umm_malloc.cpp.o — MUST keep filename for EXCLUDE_FILE
        combined_o,
        postmortem_fix_o,
        # bridge_o is already merged into combined_o via Pass 1
    ] + nolto_fw_objs + native_lib_objs + [
        "-Wl,--start-group",
    ] + sdk_lib_flags + [
        "-Wl,--end-group",
    ]

    # De-duplicate and clean
    cmd_pass2 = [x for x in cmd_pass2 if x]

    print("=> Pass 2: Final link -> %s" % target_elf)
    if iram_native_objs:
        print("   IRAM objects: %s" % ", ".join(os.path.basename(x) for x in iram_native_objs))
    if nolto_fw_objs:
        print("   Assembly objects: %s" % ", ".join(os.path.basename(x) for x in nolto_fw_objs))
    print("   SDK libs: %s" % " ".join(sdk_lib_flags[:10]))
    print("   Flags: %s" % " ".join(pass2_flags[:10]))
    ret = subprocess.call(cmd_pass2)
    if ret != 0:
        print("=> ERROR: Final link failed (exit %d)" % ret)
        # Print the full command for debugging
        print("   Full command:")
        print("   " + " ".join(cmd_pass2))
        return ret

    print("=> LTO two-pass link complete: %s" % target_elf)
    return 0


# Replace the standard link action with our two-pass version
env.Replace(
    LINKCOM=env.Action(two_pass_lto_link, "LTO two-pass link: $TARGET")
)

# Remove -flto from LINKFLAGS (we handle it ourselves in the custom action)
# but keep other useful flags.
current_linkflags = env.get("LINKFLAGS", [])
env.Replace(LINKFLAGS=[f for f in current_linkflags if "-flto" not in str(f)])

print("   Two-pass LTO link action registered")