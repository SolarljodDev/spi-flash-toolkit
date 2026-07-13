# ==============================================================
# cmake/build_config.cmake
# User-editable build flags.
# Edit here вЂ” NO need to re-run setup.ps1.
# Re-run cmake configure (Ctrl+Shift+B) to apply changes.
# ==============================================================

# ---------- Optimization ----------
# -O0  None  - no optimization, best for debugging            (default)
# -O1  Light - mild optimizations, mostly still debuggable
# -Os  Size  - minimize Flash usage  (good for space-limited chips)
# -O2  Speed - balanced speed and size
# -O3  Max   - maximum speed, may increase code size
set(BUILD_OPT "-O0")

# ---------- C standard ----------
# c99 / c11 / c17 / gnu11  (gnu11 = C11 + GCC extensions, recommended)
set(BUILD_C_STD "-std=gnu11")

# ---------- Warnings ----------
# -Wall     standard warnings  (recommended minimum)
# -Wextra   more warnings  (stricter, some false positives possible)
# -Werror   treat all warnings as errors  (CI-friendly, strict)
set(BUILD_WARNINGS "-Wall")

# ---------- Debug info ----------
# -g   full DWARF debug info (needed to step through code in VS Code / GDB)
# -g0  no debug info         (smaller .elf, cannot debug)
set(BUILD_DEBUG "-g")

# ---------- Float ABI override ----------
# ARM options: "soft" | "softfp" | "hard"
# RISC-V: not used вЂ” float ABI is encoded in -march/-mabi (see CPU flags below).
set(BUILD_FLOAT_ABI_OVERRIDE "soft")

# ---------- CPU / ISA flags override ----------
# Leave empty "" to fall back to mcu_config.cmake defaults.
# -march and -mabi must always go together for RISC-V.
set(BUILD_CPU_FLAGS_OVERRIDE "-mcpu=cortex-m3 -mthumb")

# ---------- Extra preprocessor defines ----------
# Space-separated, each prefixed with -D
# Example: "-DDEBUG -DUSE_FULL_ASSERT -DBOARD_REV=2"
set(BUILD_EXTRA_DEFINES "")

# ---------- Extra compiler flags ----------
# Anything not covered above.
# Example: "-fno-exceptions -fno-rtti -Wno-unused-variable"
set(BUILD_EXTRA_FLAGS "")