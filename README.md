# maixcam_cpu —— MaixCAM Pro（SG2002 / CV181x）CPU 频率控制

通过内核模块调用时钟框架的 `clk_set_rate()` 修改 CPU 的 MPLL，实现 **850 MHz ↔ 900 MHz** 等整数档位切换。
适用于 **MaixCAM Pro / LicheeRV Nano（SG2002，单核 RISC-V C906）**，内核 `5.10.4-tag-`。

> 本工具只改 CPU 核的 PLL（`clk_mpll`），不碰 DDR/显示/视频/USB 相关时钟。

---

## 1. 原理

### 1.1 时钟树

```
osc 25 MHz ──> clk_mpll (MPLL, 850 MHz = 25 × 34)
                    │
                    └──> clk_c906_0 (mux + divider) ──> CPU core (hart 0, C906)
```

* `clk_c906_0` 是个 mux/divider，**没有** `CLK_SET_RATE_PARENT` 标志，
  所以对它调 `clk_set_rate()` 不会上溯到 PLL，必须直接改 `clk_mpll`。
* 内核**没有 cpufreq 子系统**（`/sys/devices/system/cpu/cpu0/cpufreq` 不存在），
  MaixCDK / MaixPy 也只有 `cpu_freq()`、`cpu_temp()`、`npu_freq()` 三个**读取**接口，没有设置接口。

### 1.2 为什么 `devmem` 直接写 PLL 无效

时钟控制器基址 `0x03002000`（设备树 `clk-reset-controller`，`compatible = "cvitek,clk-reset"`），
MPLL 控制寄存器 `MPLL_CSR = base + 0x908`，字段布局（参见 mainline `drivers/clk/sophgo/clk-cv1800.c`）：

| 位域 | 含义 |
|---|---|
| `[6:0]` | pre_div |
| `[14:8]` | post_div |
| `[16:15]` | mode（整数/小数） |
| `[23:17]` | div（倍频） |
| `[26:24]` | ictrl（PLL 环路滤波/电流控制） |

`rate = 25 MHz × div / (pre_div × post_div)`

实测：

| 状态 | `MPLL_CSR` | 频率 |
|---|---|---|
| 开机（div=34, ictrl=0） | `0x00448101` | 850 MHz |
| 裸 `devmem` 只改 div=36 | `0x00488101` | **仍是 850 MHz**（无效） |
| 经 `clk_set_rate()` 设 900 | `0x07488101`（div=36, **ictrl=7**） | 900 MHz |
| 经 `clk_set_rate()` 设 850 | `0x07448101`（div=34, ictrl=7） | 850 MHz |

**结论：只改分频位不够，驱动在改频时会同时重算 `ictrl`。**
`clk_set_rate()` 走的是厂商驱动 `cv181x_g6_pll_set_rate`，它会写 CSR 并等待 PLL 锁定，
这才是正确、可用的改频方式。

### 1.3 模块做了什么

1. `clk_get(NULL, "clk_mpll")` 拿到 PLL 时钟；
2. 延迟 `delay_ms` 后在 workqueue 里调 `clk_set_rate(mpll, target)`；
3. 用 `ioremap(0x03002000)` 读 `MPLL_CSR` 前后值，连同 `clk_get_rate()` 结果打到 dmesg；
4. 卸载时只 `clk_put()`（**不会**把频率恢复）。

---

## 2. 目录结构

```
maixcam_cpu/
├── README.md              本文件（中文）
├── README_en.md           English version
├── build.sh               编译脚本（在带内核树的 Linux 上运行）
├── src/
│   ├── mpll_freq.c        模块源码
│   └── Makefile           kbuild Makefile
├── prebuilt/
│   └── mpll_freq.ko       已编译好的模块（vermagic 5.10.4-tag- ...）
└── scripts/
    └── cpu_freq.sh        设备上的便捷脚本（status / set / restore）
```

---

## 3. 编译

### 3.1 需要什么

| 项 | 要求 |
|---|---|
| 系统 | Linux（物理机 / 虚拟机 / WSL2 均可） |
| 内核源码 | **5.10.4**，且 `EXTRAVERSION = -tag-`（可用 vanilla 5.10.4 + 设备 `.config`） |
| 配置 | `zcat /proc/config.gz > .config`（在设备上导出） |
| 工具链 | `riscv64-linux-gnu-gcc`（gcc 10 或 11） |
| 其他 | `make bc bison flex libssl-dev libelf-dev` |

> 设备内核是 `5.10.4-tag- #14 PREEMPT`，厂商用 Xuantie GCC 10.2 编译；
> 实测 Ubuntu 22.04 的 `gcc-riscv64-linux-gnu 11.4.0` 编出来的模块可以直接加载
> （`CONFIG_MODVERSIONS` 未开，vermagic 不包含编译器版本）。

### 3.2 步骤

```bash
# 1) 准备内核树（示例：WSL2 + Ubuntu 22.04）
wget https://cdn.kernel.org/pub/linux/kernel/v5.x/linux-5.10.4.tar.xz
tar xf linux-5.10.4.tar.xz && cd linux-5.10.4
sed -i 's/^EXTRAVERSION *=.*/EXTRAVERSION = -tag-/' Makefile

# 2) 导入设备配置（在设备上执行 zcat /proc/config.gz > device.config 后传回）
cp /path/to/device.config .config
make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- olddefconfig
make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- modules_prepare

# 3) 编译模块
cd /path/to/maixcam_cpu
./build.sh /path/to/linux-5.10.4
```

编译成功输出：

```
built: maixcam_cpu/prebuilt/mpll_freq.ko
vermagic: 5.10.4-tag- preempt mod_unload riscv64
expected on device: 5.10.4-tag- preempt mod_unload riscv64
```

`vermagic` 必须与设备上的厂商模块一致（可对照 `/mnt/system/ko/*.ko`）。

---

## 4. 使用（设备上）

```bash
# 上传
scp prebuilt/mpll_freq.ko scripts/cpu_freq.sh root@<device>:/root/cpu_tool/

# 设为 900 MHz（delay_ms 让 insmod 先返回，再真正改频）
insmod /root/cpu_tool/mpll_freq.ko target=900000000 delay_ms=3000
dmesg | tail -8

# 验证
cat /sys/kernel/debug/clk/clk_c906_0/clk_rate     # 900000000
devmem 0x03002908 32                              # 0x07488101
cat /sys/class/thermal/thermal_zone0/temp         # 温度，毫摄氏度

# 改回 850 MHz（rmmod 不会恢复频率，必须重新 insmod）
rmmod mpll_freq
insmod /root/cpu_tool/mpll_freq.ko target=850000000 delay_ms=1500
rmmod mpll_freq
```

或者用脚本：

```bash
./cpu_freq.sh status            # 看当前 PLL / 频率 / 温度 / 模块状态
./cpu_freq.sh set 900000000     # 设 900 MHz
./cpu_freq.sh restore           # 设回 850 MHz 并卸载模块
```

参数：

| 参数 | 默认 | 说明 |
|---|---|---|
| `target` | `900000000` | 目标频率（Hz），必须是 25 MHz 的整数倍 |
| `delay_ms` | `5000` | 加载后延迟多久真正改频 |

---

## 5. 实测数据（MaixCAM Pro，`maixcam-bca0`）

* `uname -r`：`5.10.4-tag-`
* 设备库/内核：无 cpufreq，单核 C906

| 项目 | 850 MHz | 900 MHz |
|---|---|---|
| `MPLL_CSR` | `0x07448101` | `0x07488101` |
| `clk_rate` | 850000000 | 900000000 |
| 基准（1.5M 次整数累加，6 次中位数） | 4.225 s | 3.734 s |
| 相对性能 | 1.00× | **约 1.11×** |
| 温度 | 约 59.5～62 °C | 约 61.5～63.7 °C |

* 900 MHz 连续观测 2 分钟以上没有回落，`dmesg` 无 warn/oops；
* 多次 850↔900 往返切换均正常；
* 重启后自动回到 850 MHz（PLL 由 bootloader 重新配置）。

---

## 6. 限制与风险

1. **不持久**：重启后回到 850 MHz。要开机生效需要把 `insmod` 放进 `/etc/init.d/`。
2. **`rmmod` 不恢复频率**：卸载只做 `clk_put()`，PLL 保持当前值。
3. **改频瞬间会重锁 PLL**：如果 `target` 不合理（例如不是 25 MHz 倍数、超出 PLL 范围），
   可能死机；真挂了**断电重启**即可恢复（PLL 每次启动由 bootloader 重写）。
4. **只改 `clk_mpll`**：不要动 `fpll/tpll/disppll/mipimpll`（挂 DDR、显示、视频、USB，写错可能损坏文件系统）。
5. **没有电压联动**：内核没有 cpufreq/OPP，核心电压不随频率调整。900 MHz 实测稳定，
   更高频率（如 1 GHz）不保证稳定，需自行验证。
6. 模块需要与内核 vermagic 完全匹配；本模块对应 `5.10.4-tag- preempt mod_unload riscv64`。

---

## 7. 参考

* 内核时钟驱动：`drivers/clk/sophgo/clk-cv1800.c`、`clk-cv18xx-pll.c`（mainline）
* 设备树：`/proc/device-tree/clk-reset-controller`（`reg = <0x03002000 0x800>`）
* MaixCDK 的频率读取实现：`components/basic/src/maix_sys.cpp`（`cpu_freq()` 解析 `clk_c906_0`）
* 内核符号：`cv181x_g2_pll_set_rate` / `cv181x_g6_pll_set_rate` / `cv181x_clk_mux_set_parent`
