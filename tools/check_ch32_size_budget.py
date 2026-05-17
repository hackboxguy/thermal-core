#!/usr/bin/env python3
"""tools/check_ch32_size_budget.py

Stage 18 CH32V003 firmware size-budget gate.

Unlike the ESP32 (where the budget is the core/+protocol/ slice of a
much larger image), the CH32V003 STANDALONE firmware *is* the whole
deliverable -- ch32fun runtime + tiny-profile core/ + BSP + glue --
so the budget is the whole linked image against the part:

    flash  = .text + .data   <=  16 KB   (16384 B)
    SRAM   = .data + .bss     <=   2 KB   (2048 B)

Hard limits fail the build. PRD Appendix D.2 budgets the STANDALONE
firmware at ~14.5-15.5 KB flash / ~1.3 KB SRAM; exceeding those soft
targets warns but does not fail (real BSPs land incrementally).

Usage: check_ch32_size_budget.py <firmware.elf>
"""
import subprocess
import sys

SIZE_TOOL = "riscv64-unknown-elf-size"

FLASH_HARD = 16 * 1024
SRAM_HARD = 2 * 1024
FLASH_SOFT = 15 * 1024          # PRD D.2 ~14.5-15.5 KB
SRAM_SOFT = 1536                # PRD D.2 ~1.3 KB


def main():
    if len(sys.argv) != 2:
        print("usage: check_ch32_size_budget.py <firmware.elf>")
        return 2
    elf = sys.argv[1]

    try:
        out = subprocess.run([SIZE_TOOL, elf], capture_output=True,
                             text=True, check=True).stdout
    except FileNotFoundError:
        print(f"ch32-size-budget: {SIZE_TOOL} not found")
        return 2
    except subprocess.CalledProcessError as e:
        print(f"ch32-size-budget: {SIZE_TOOL} failed: {e.stderr.strip()}")
        return 2

    # `size` prints a header row then one data row:
    #    text    data     bss     dec     hex filename
    rows = [ln.split() for ln in out.splitlines() if ln.strip()]
    if len(rows) < 2:
        print(f"ch32-size-budget: could not parse {SIZE_TOOL} output:\n{out}")
        return 2
    text, data, bss = (int(rows[1][0]), int(rows[1][1]), int(rows[1][2]))

    flash = text + data
    sram = data + bss
    print(f"ch32-size-budget: flash (.text+.data) = {flash} B "
          f"of {FLASH_HARD} ({100.0 * flash / FLASH_HARD:.1f}%)")
    print(f"ch32-size-budget: SRAM  (.data+.bss)  = {sram} B "
          f"of {SRAM_HARD} ({100.0 * sram / SRAM_HARD:.1f}%)")

    failed = False
    if flash > FLASH_HARD:
        print(f"ch32-size-budget: FAIL -- flash {flash} B exceeds the "
              f"{FLASH_HARD} B part limit")
        failed = True
    if sram > SRAM_HARD:
        print(f"ch32-size-budget: FAIL -- SRAM {sram} B exceeds the "
              f"{SRAM_HARD} B part limit")
        failed = True
    if failed:
        return 1

    if flash > FLASH_SOFT:
        print(f"ch32-size-budget: WARN -- flash {flash} B is over the "
              f"PRD D.2 soft target ({FLASH_SOFT} B)")
    if sram > SRAM_SOFT:
        print(f"ch32-size-budget: WARN -- SRAM {sram} B is over the "
              f"PRD D.2 soft target ({SRAM_SOFT} B)")

    print("ch32-size-budget: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
