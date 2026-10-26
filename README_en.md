# maixcam_cpu — CPU frequency control for MaixCAM Pro

A small kernel module that retunes the CPU PLL (`clk_mpll`) through `clk_set_rate()`,
switching the RISC-V C906 core between integer steps such as **850 MHz and 900 MHz**.
Target: MaixCAM Pro / LicheeRV Nano (SG2002, single-core C906), kernel `5.10.4-tag-`.

## What we know

- The CPU runs at **850 MHz**, a **fixed frequency that does not scale with workload**
  (no cpufreq / DVFS in the kernel);
- MaixCDK / MaixPy only provide read helpers (`cpu_freq()`, `cpu_temp()`), no setter;
- This tool fills that gap with a kernel module and only touches `clk_mpll`
  (the CPU PLL), never DDR / display / video / USB clocks.

## Quick start

```sh
# upload
scp prebuilt/mpll_freq.ko root@<device>:/root/cpu_tool/

# switch to 900 MHz (delay_ms lets insmod return before the PLL is touched)
insmod /root/cpu_tool/mpll_freq.ko target=900000000 delay_ms=3000
dmesg | tail -6

# verify
cat /sys/kernel/debug/clk/clk_c906_0/clk_rate     # 900000000
devmem 0x03002908 32                              # 0x07488101
cat /sys/class/thermal/thermal_zone0/temp         # temperature, milli-degC

# back to 850 MHz (rmmod does NOT restore the frequency)
rmmod mpll_freq
insmod /root/cpu_tool/mpll_freq.ko target=850000000 delay_ms=1500
rmmod mpll_freq
```

Or use the helper script on the device:

```sh
./cpu_freq.sh status            # rate / PLL / temperature / module state
./cpu_freq.sh set 900000000     # set 900 MHz
./cpu_freq.sh restore           # set 850 MHz and unload
```

Parameters:

| Name | Default | Description |
|---|---|---|
| `target` | `900000000` | target rate in Hz (must be a multiple of 25 MHz) |
| `delay_ms` | `5000` | delay before actually changing the PLL |

## Layout

```
maixcam_cpu/
├── README.md / README_en.md   this file / Chinese
├── docs/                      design notes and implementation details
│   ├── DESIGN.md
│   └── DESIGN_en.md
├── build.sh                   build script
├── src/                       module source + Makefile
├── prebuilt/mpll_freq.ko      prebuilt module
└── scripts/cpu_freq.sh        helper script for the device
```

## Build

Requires Linux (WSL2 works), a 5.10.4 kernel tree with `EXTRAVERSION=-tag-`,
the device `.config`, and `riscv64-linux-gnu-gcc`:

```sh
./build.sh /path/to/linux-5.10.4
```

The result is `prebuilt/mpll_freq.ko`; its vermagic must be
`5.10.4-tag- preempt mod_unload riscv64`. See [docs/DESIGN_en.md](docs/DESIGN_en.md) for details.

## Frequency sweep (850–1000 MHz)

Method: switch with the module, then run 6 repetitions of a fixed workload
(1.5 M integer additions, best value; best of two sweeps), followed by a 25 s
full-load burn for the temperature. The launcher was stopped before the test.

| Target | `MPLL_CSR` | Benchmark (best) | vs 850 | Temperature (idle / load) |
|---|---|---|---|---|
| 850 MHz | `0x07448101` | 3.936 s | 1.00x | 60.9 / 61.2 °C |
| 875 MHz | `0x07468101` | 3.816 s | 1.03x | 60.5 / 61.2 °C |
| 900 MHz | `0x07488101` | 3.723 s | 1.06x | 60.5 / 60.9 °C |
| 925 MHz | `0x074A8101` | 3.612 s | 1.09x | 60.2 / 61.2 °C |
| 950 MHz | `0x074C8101` | 3.590 s | 1.10x | 60.2 / 60.9 °C |
| 975 MHz | `0x074E8101` | 3.433 s | 1.15x | 60.2 / 61.2 °C |
| 1000 MHz | `0x07508101` | 3.437 s | 1.15x | 60.2 / 60.9 °C |

Observations:

* from 850 to 975 MHz the speedup is roughly linear in frequency
  (measured 1.03/1.06/1.09/1.10/1.15 vs theoretical 1.03/1.06/1.09/1.12/1.15);
* **975 → 1000 MHz brings no further gain** (1.15x vs 1.18x theoretical), probably because the
  CPU AXI port (`clk_cpu_axi0`, fixed at 500 MHz) or memory bandwidth becomes the bottleneck;
* **the temperature stays within 60.2–61.2 °C and does not visibly depend on frequency**:
  25 s of load is not enough to create a gradient, and the sensor resolution is about 0.35 °C;
* the system is stable at 1000 MHz (no hang, no warnings/oops), but there is no voltage scaling,
  so long-term stability has to be evaluated by the user.

> Note: the Python-loop benchmark is sensitive to background load (two sweeps of the same
> frequency can differ by 20–30%). The table takes the best of two sweeps, so the absolute
> values are only meaningful for relative comparison within this table.

---

## Notes

- **Not persistent**: a reboot returns to 850 MHz;
- **`rmmod` does not restore the frequency** (it only calls `clk_put()`);
- the PLL re-locks when changing frequency; a bad value may hang the board —
  **power-cycle to recover**;
- only `clk_mpll` is touched; never `fpll` / `tpll` / `disppll` / `mipimpll`.

## Further reading

Principle, register details, implementation, measurements and risks:
[docs/DESIGN_en.md](docs/DESIGN_en.md).
