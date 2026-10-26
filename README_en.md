# maixcam_cpu — CPU frequency control for MaixCAM Pro (SG2002 / CV181x)

A tiny out-of-tree kernel module that retunes the CPU PLL (`clk_mpll`) through the
kernel clock framework (`clk_set_rate()`), switching the RISC-V C906 core between
integer steps such as **850 MHz and 900 MHz**.

Target: **MaixCAM Pro / LicheeRV Nano (SG2002, single-core RISC-V C906)**, kernel `5.10.4-tag-`.

> Only the CPU PLL (`clk_mpll`) is touched. DDR / display / video / USB clocks
> (`fpll`, `tpll`, `disppll`, `mipimpll`) are left alone.

---

## 1. Overview

### 1.1 What we found

* The device is a MaixCAM Pro (SG2002, single-core RISC-V C906) running kernel `5.10.4-tag-`.
* The CPU actually runs at **850 MHz** (`clk_c906_0`), fed by `clk_mpll = 25 MHz x 34 = 850 MHz`.
* **The frequency is fixed and does not scale with workload**:
  * there is no cpufreq subsystem (`/sys/devices/system/cpu/cpu0/cpufreq` does not exist),
    no governor and no DVFS;
  * MaixCDK / MaixPy only expose read helpers (`cpu_freq()`, `cpu_temp()`, `npu_freq()`),
    there is no API to set the frequency.
* Temperature: about 59.5 °C idle, 60–63.7 °C under load.

### 1.2 How the CPU clock is controlled

```
osc 25 MHz ──> clk_mpll (MPLL, 850 MHz = 25 x 34)
                    │
                    └──> clk_c906_0 (mux + divider) ──> CPU core (hart 0, C906)
```

* `clk_c906_0` is a mux/divider **without** `CLK_SET_RATE_PARENT`, so calling
  `clk_set_rate()` on it does not propagate to the PLL; the PLL (`clk_mpll`)
  has to be targeted directly.
* The clock controller base is `0x03002000` (device tree `clk-reset-controller`,
  `compatible = "cvitek,clk-reset"`). The MPLL control register is
  `MPLL_CSR = base + 0x908`:

| Bits | Field |
|---|---|
| `[6:0]` | pre_div |
| `[14:8]` | post_div |
| `[16:15]` | mode (integer / fractional) |
| `[23:17]` | div (multiplier) |
| `[26:24]` | ictrl (PLL loop filter / current control) |

`rate = 25 MHz x div / (pre_div x post_div)`

* **Changing the divider alone is not enough**: writing `devmem` to change div from 34 to 36
  (`0x00488101`) leaves the clock at 850 MHz; going through `clk_set_rate()` makes the vendor
  driver `cv181x_g6_pll_set_rate()` recompute `ictrl` as well, and only then does the PLL switch.
  Measured values:

| State | `MPLL_CSR` | Frequency |
|---|---|---|
| Boot (div=34, ictrl=0) | `0x00448101` | 850 MHz |
| Raw `devmem`, div=36 | `0x00488101` | 850 MHz (no effect) |
| `clk_set_rate(900 MHz)` | `0x07488101` (div=36, ictrl=7) | 900 MHz |

### 1.3 What this project does

* Provides a kernel module `mpll_freq.ko`: `clk_get(NULL, "clk_mpll")` → delay →
  `clk_set_rate(mpll, target)`.
* Uses `ioremap(0x03002000)` to read `MPLL_CSR` before and after the change and logs it
  together with `clk_get_rate()` to dmesg for verification.
* Parameters: `target` (frequency in Hz, an integer multiple of 25 MHz) and
  `delay_ms` (how long to wait after loading before touching the PLL).
* Touches only `clk_mpll` (which feeds the CPU), never the DDR / display / video / USB PLLs.
* Unloading only calls `clk_put()` and **does not restore the frequency**; after a reboot the
  bootloader programs 850 MHz again.

---

## 2. Repository layout

```
maixcam_cpu/
├── README.md              Chinese readme
├── README_en.md           this file
├── build.sh               build script (run on Linux with a kernel tree)
├── src/
│   ├── mpll_freq.c        module source
│   └── Makefile           kbuild Makefile
├── prebuilt/
│   └── mpll_freq.ko       prebuilt module (vermagic 5.10.4-tag- ...)
└── scripts/
    └── cpu_freq.sh        convenience script for the device
```

---

## 3. Building

### 3.1 Requirements

| Item | Requirement |
|---|---|
| Host | Linux (bare metal, VM or WSL2) |
| Kernel source | **5.10.4** with `EXTRAVERSION = -tag-` (vanilla 5.10.4 + device `.config` works) |
| Config | `zcat /proc/config.gz > .config` (exported from the device) |
| Toolchain | `riscv64-linux-gnu-gcc` (gcc 10 or 11) |
| Packages | `make bc bison flex libssl-dev libelf-dev` |

> The device kernel is `5.10.4-tag- #14 PREEMPT`, built with Xuantie GCC 10.2.
> A module built with Ubuntu 22.04 `gcc-riscv64-linux-gnu 11.4.0` loads fine:
> `CONFIG_MODVERSIONS` is off, and vermagic does not include the compiler version.

### 3.2 Steps

```bash
# 1) prepare kernel tree (example: WSL2 + Ubuntu 22.04)
wget https://cdn.kernel.org/pub/linux/kernel/v5.x/linux-5.10.4.tar.xz
tar xf linux-5.10.4.tar.xz && cd linux-5.10.4
sed -i 's/^EXTRAVERSION *=.*/EXTRAVERSION = -tag-/' Makefile

# 2) import the device config
cp /path/to/device.config .config
make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- olddefconfig
make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- modules_prepare

# 3) build the module
cd /path/to/maixcam_cpu
./build.sh /path/to/linux-5.10.4
```

Expected output:

```
built: maixcam_cpu/prebuilt/mpll_freq.ko
vermagic: 5.10.4-tag- preempt mod_unload riscv64
expected on device: 5.10.4-tag- preempt mod_unload riscv64
```

The vermagic must match the vendor modules on the device
(compare with `/mnt/system/ko/*.ko`).

---

## 4. Usage (on the device)

```bash
# upload
scp prebuilt/mpll_freq.ko scripts/cpu_freq.sh root@<device>:/root/cpu_tool/

# switch to 900 MHz (delay_ms lets insmod return before the PLL is touched)
insmod /root/cpu_tool/mpll_freq.ko target=900000000 delay_ms=3000
dmesg | tail -8

# verify
cat /sys/kernel/debug/clk/clk_c906_0/clk_rate     # 900000000
devmem 0x03002908 32                              # 0x07488101
cat /sys/class/thermal/thermal_zone0/temp         # temperature, milli-degC

# back to 850 MHz (rmmod does NOT restore the frequency)
rmmod mpll_freq
insmod /root/cpu_tool/mpll_freq.ko target=850000000 delay_ms=1500
rmmod mpll_freq
```

Or use the helper script:

```bash
./cpu_freq.sh status            # PLL / rate / temperature / module state
./cpu_freq.sh set 900000000     # set 900 MHz
./cpu_freq.sh restore           # set 850 MHz and unload
```

Module parameters:

| Parameter | Default | Description |
|---|---|---|
| `target` | `900000000` | target rate in Hz (must be an integer multiple of 25 MHz) |
| `delay_ms` | `5000` | delay before actually changing the PLL |

---

## 5. Measured results (MaixCAM Pro, `maixcam-bca0`)

* `uname -r`: `5.10.4-tag-`
* single-core C906, no cpufreq

| Item | 850 MHz | 900 MHz |
|---|---|---|
| `MPLL_CSR` | `0x07448101` | `0x07488101` |
| `clk_rate` | 850000000 | 900000000 |
| Benchmark (1.5M integer adds, median of 6) | 4.225 s | 3.734 s |
| Relative performance | 1.00x | **approx. 1.11x** |
| Temperature | about 59.5–62 °C | about 61.5–63.7 °C |

* 900 MHz stayed stable for more than two minutes of observation; no warnings or oops in dmesg;
* repeated 850 <-> 900 switches worked fine;
* a reboot returns to 850 MHz (the bootloader programs the PLL again).

---

## 6. Limitations and risks

1. **Not persistent**: after a reboot the CPU is back at 850 MHz. To apply it at
   boot, add an `insmod` line to `/etc/init.d/`.
2. **`rmmod` does not restore the frequency**: unload only calls `clk_put()`.
3. **The PLL re-locks during the change**: an invalid `target` (not a multiple of
   25 MHz, or out of range) may hang the board. Recover with a **power cycle**;
   the bootloader always programs 850 MHz at boot.
4. **Only `clk_mpll`**: never touch `fpll` / `tpll` / `disppll` / `mipimpll`
   (DDR, display, video, USB — a bad write can corrupt the filesystem).
5. **No voltage scaling**: the kernel has no cpufreq/OPP, so core voltage does not
   follow the frequency. 900 MHz was stable in our tests; higher values (e.g. 1 GHz)
   are not guaranteed and must be validated.
6. The module must match the kernel vermagic; this build targets
   `5.10.4-tag- preempt mod_unload riscv64`.

---

## 7. References

* Kernel clock driver: `drivers/clk/sophgo/clk-cv1800.c`, `clk-cv18xx-pll.c` (mainline)
* Device tree: `/proc/device-tree/clk-reset-controller` (`reg = <0x03002000 0x800>`)
* MaixCDK frequency read path: `components/basic/src/maix_sys.cpp` (`cpu_freq()` parses `clk_c906_0`)
* Kernel symbols: `cv181x_g2_pll_set_rate` / `cv181x_g6_pll_set_rate` / `cv181x_clk_mux_set_parent`
