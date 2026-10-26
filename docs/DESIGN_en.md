# Design and implementation notes

Technical background, principle, implementation details and measurements for `maixcam_cpu`.
For a quick start see [../README_en.md](../README_en.md).

---

## 1. Background and findings

| Item | Value |
|---|---|
| Device | MaixCAM Pro / LicheeRV Nano, SoC **SG2002 (CV181x family)** |
| CPU | single-core **RISC-V C906** (hart 0), `nproc = 1` |
| Kernel | `Linux 5.10.4-tag- #14 PREEMPT`, built with Xuantie GCC 10.2 (musl) |
| CPU frequency | **850 MHz** |
| Frequency scaling | **none**: no cpufreq subsystem, no `/sys/devices/system/cpu/cpu0/cpufreq`, no governor / DVFS |
| Userspace API | MaixCDK / MaixPy only expose `cpu_freq()`, `cpu_temp()`, `npu_freq()` — all **read-only** |
| Temperature | about 59.5 °C idle, 60–63.7 °C under load |

So the CPU runs at a **fixed frequency** and does not scale with workload,
and the SDK provides no way to change it.

The authoritative frequency source is `/sys/kernel/debug/clk/clk_c906_0/clk_rate`
(MaixCDK's `cpu_freq()` parses the same `clk_c906_0` line from `clk_summary`).

---

## 2. Clock tree and registers

### 2.1 Clock tree

```
osc 25 MHz ──> clk_mpll (MPLL, 850 MHz = 25 x 34)
                    │
                    └──> clk_c906_0 (mux + divider) ──> CPU core (hart 0, C906)
```

* `clk_c906_0` is a mux/divider **without** `CLK_SET_RATE_PARENT`; calling
  `clk_set_rate()` on it only changes the divider and does not propagate to the PLL.
  The PLL (`clk_mpll`) must be targeted directly.
* `clk_mpll` has exactly one child (`clk_c906_0`), so changing it does not affect
  DDR / display / video / USB (those hang off `fpll`, `tpll`, `disppll`, `mipimpll`).

### 2.2 Clock controller and MPLL_CSR

Device tree `/proc/device-tree/clk-reset-controller`:

```
compatible = "cvitek,clk-reset"
reg        = <0x03002000 0x800>
```

MPLL control register: `MPLL_CSR = 0x03002000 + 0x908`. Field layout
(following mainline `drivers/clk/sophgo/clk-cv1800.c` / `clk-cv18xx-pll.c`):

| Bits | Field | Meaning |
|---|---|---|
| `[6:0]` | pre_div | reference divider |
| `[14:8]` | post_div | output divider |
| `[16:15]` | mode | integer / fractional mode |
| `[23:17]` | div | multiplier |
| `[26:24]` | ictrl | PLL loop filter / current control |

`rate = 25 MHz x div / (pre_div x post_div)`

Measured PLLs on the board (cross-check for the field decoding):

| PLL | Offset | CSR | Decoded |
|---|---|---|---|
| mpll | `0x908` | `0x00448101` | pre=1 post=1 div=34 → 850 MHz |
| tpll | `0x90c` | `0x07708101` | div=56 → 1400 MHz |
| fpll | `0x910` | `0x07788101` | div=60 → 1500 MHz |
| mipimpll | `0x808` | `0x05488101` | div=36 → 900 MHz |

---

## 3. Why `clk_set_rate()` is required

The first attempt was to change the divider directly with `devmem`:

```sh
devmem 0x03002908 32 0x00488101     # div 34 -> 36
```

Result:

* the register reads back as `0x00488101` and the value persists;
* but **the CPU performance does not change at all** (a fixed-workload benchmark shows no
  systematic difference between the 850 and the "900" settings);
* the kernel still reports 850 MHz (`clk_mpll`'s rate is cached; bypassing the driver
  does not update it).

Going through `clk_set_rate(mpll, 900000000)` produces:

```
0x07488101     # div=36, and ictrl changed from 0 to 7
```

and this time the frequency really changes (see section 6).

**Why**: the vendor PLL driver `cv181x_g6_pll_set_rate()` recomputes `ictrl`
(PLL loop filter / current control) together with the divider and waits for the PLL
to re-lock. Changing only `div` while keeping the old `ictrl` does not move the PLL
to the new frequency.

| State | `MPLL_CSR` | Frequency |
|---|---|---|
| Boot (div=34, ictrl=0) | `0x00448101` | 850 MHz |
| Raw `devmem`, div=36 | `0x00488101` | 850 MHz (no effect) |
| `clk_set_rate(900 MHz)` | `0x07488101` (div=36, ictrl=7) | 900 MHz |
| `clk_set_rate(850 MHz)` | `0x07448101` (div=34, ictrl=7) | 850 MHz |

> Note the last row: restoring 850 MHz uses "div=34 + ictrl=7", which differs from the
> boot value `0x00448101` (ictrl=0). It is the result of the driver recomputing ictrl and
> runs fine at 850 MHz.

---

## 4. Module implementation

Source: [`src/mpll_freq.c`](../src/mpll_freq.c).

### 4.1 Init (`module_init`)

1. `clk_get(NULL, "clk_mpll")`; on failure return the error so `insmod` fails safely;
2. `ioremap(0x03002000, 0x1000)` for register readback (warning only on failure);
3. log the current rate;
4. schedule the actual change with `INIT_DELAYED_WORK` / `schedule_delayed_work(delay_ms)`
   so that `insmod` returns first and the "about to change" message is already in dmesg.

### 4.2 Frequency change (workqueue callback)

```c
csr_before = readl(regs + MPLL_CSR_OFF);
before     = clk_get_rate(mpll);
ret        = clk_set_rate(mpll, target);   /* goes through g6_pll_set_rate */
after      = clk_get_rate(mpll);
csr_after  = readl(regs + MPLL_CSR_OFF);
pr_info(...);                              /* CSR before/after, ret, rates */
```

### 4.3 Exit (`module_exit`)

`cancel_delayed_work_sync()` → `iounmap()` → `clk_put()`.
The frequency is **not** restored.

### 4.4 Parameters

| Name | Default | Description |
|---|---|---|
| `target` | `900000000` | target rate in Hz (multiple of 25 MHz) |
| `delay_ms` | `5000` | delay before touching the PLL |

---

## 5. Build environment and vermagic

The device kernel was built with **Xuantie GCC 10.2 (musl)**, but the module does not
need the same compiler:

* `CONFIG_MODVERSIONS` is **off** → symbols are resolved by name, no CRC checks;
* `CONFIG_MODULE_SIG` is **off** → no signing required;
* only **vermagic** matters (release + SMP/PREEMPT/MODULE_UNLOAD + arch).

Therefore vanilla 5.10.4 + the device `.config` + any riscv64 GCC 10/11 is enough.

Environment used here:

| Item | Value |
|---|---|
| Build host | WSL2, Ubuntu 22.04 (dedicated distro `maix-build`, imported from ubuntu-base rootfs) |
| Toolchain | `riscv64-linux-gnu-gcc 11.4.0` |
| Kernel source | `linux-5.10.4.tar.xz` (kernel.org) |
| Config | exported from the device `/proc/config.gz`; `EXTRAVERSION = -tag-` in `Makefile` |
| Prepare | `make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- olddefconfig modules_prepare` |
| Build | `./build.sh /path/to/linux-5.10.4` |

Vermagic comparison:

| | vermagic |
|---|---|
| Device kernel | `5.10.4-tag- preempt mod_unload riscv64` |
| Vendor module `soph_ive.ko` | `5.10.4-tag- preempt mod_unload riscv64` |
| This module | `5.10.4-tag- preempt mod_unload riscv64` ✔ |

> The device has no `/lib/modules`, so `modprobe`/`depmod` are not usable;
> load it with `insmod /path/mpll_freq.ko`.

---

## 6. Measurements

### 6.1 Switching (dmesg)

```
mpll_freq: loaded, current rate = 850000000 Hz, will retune in 3000 ms
mpll_freq: MPLL_CSR before = 0x07448101
mpll_freq: rate before = 850000000 Hz, requesting 900000000 Hz
mpll_freq: MPLL_CSR after  = 0x07488101
mpll_freq: clk_set_rate() = 0, rate after = 900000000 Hz
```

* `clk_set_rate()` returns 0;
* the kernel reports `clk_c906_0/clk_rate = 900000000`;
* no hang, no warnings/oops;
* 900 MHz stayed stable for more than two minutes, and repeated 850 <-> 900 switches worked.

### 6.2 Performance

Fixed workload: 1.5 million integer additions, `time.process_time()`, median of 6 runs
(launcher stopped before the test):

| Setting | Median | Min |
|---|---|---|
| 850 MHz | 4.225 s | 4.184 s |
| 900 MHz | 3.734 s | 3.715 s |

900 MHz is about **11% faster** (theoretical 5.9%; the extra is likely measurement noise,
but the direction is clear).

### 6.3 Temperature

| State | Temperature |
|---|---|
| Idle at 850 MHz | about 59.5 °C |
| 900 MHz under load | 61.5–63.7 °C |

There is no cooling device scaling (no cpufreq).

---

## 7. Failure modes and risks

| Case | Symptom | Recovery |
|---|---|---|
| Unreasonable `target` (not a multiple of 25 MHz, out of range) | PLL fails to re-lock, board may hang | **Power cycle** (bootloader reprograms 850 MHz) |
| Reboot | frequency is back to 850 MHz | re-`insmod`, or add an init script |
| `rmmod` | frequency unchanged | `insmod target=850000000` |
| Writing `fpll/tpll/disppll/mipimpll` by mistake | may affect DDR/display/video/USB, possibly corrupting the filesystem | power cycle |
| Kernel mismatch | `insmod` reports vermagic mismatch | rebuild against the matching kernel tree |

Other notes:

* no voltage scaling (no cpufreq/OPP); higher rates (e.g. 1 GHz) are not guaranteed;
* `CLK_IS_CRITICAL` only prevents disabling the clock, it does not block `clk_set_rate()`;
* the module uses `clk_get` / `clk_set_rate`, which are GPL-only exports, hence `MODULE_LICENSE("GPL")`.

---

## 8. References

* Mainline clock driver: `drivers/clk/sophgo/clk-cv1800.c`, `drivers/clk/sophgo/clk-cv18xx-pll.c`
* Device tree node: `/proc/device-tree/clk-reset-controller`
* MaixCDK frequency read path: `components/basic/src/maix_sys.cpp` (`cpu_freq()` parses `clk_c906_0`)
* Kernel symbols (`/proc/kallsyms`): `cv181x_g2_pll_set_rate`, `cv181x_g6_pll_set_rate`, `cv181x_clk_mux_set_parent`
* Relevant config: `CONFIG_MODULES=y`, `CONFIG_MODULE_UNLOAD=y`,
  `# CONFIG_MODVERSIONS is not set`, `# CONFIG_MODULE_SIG is not set`
