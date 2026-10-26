# 设计与实现说明

本文档记录 `maixcam_cpu` 的技术背景、原理、实现细节和实测数据。
快速上手请看 [../README.md](../README.md)。

---

## 1. 背景与发现

| 项 | 值 |
|---|---|
| 设备 | MaixCAM Pro / LicheeRV Nano，SoC **SG2002（CV181x 系列）** |
| CPU | 单核 **RISC-V C906**（hart 0），`nproc = 1` |
| 内核 | `Linux 5.10.4-tag- #14 PREEMPT`，Xuantie GCC 10.2（musl）编译 |
| CPU 实际频率 | **850 MHz** |
| 调频能力 | **无**：内核没有 cpufreq 子系统，`/sys/devices/system/cpu/cpu0/cpufreq` 不存在，没有 governor / DVFS |
| 用户态接口 | MaixCDK / MaixPy 只有 `cpu_freq()`、`cpu_temp()`、`npu_freq()`，都是**只读** |
| 温度 | 空闲约 59.5 °C，有负载 60～63.7 °C |

结论：CPU 是**固定频率**运行的，不随任务负载自动调节；现有 SDK 也没有提供设置频率的入口。

频率的权威来源是 `/sys/kernel/debug/clk/clk_c906_0/clk_rate`（MaixCDK 的 `cpu_freq()` 读的也是
`/sys/kernel/debug/clk/clk_summary` 里的 `clk_c906_0`）。

---

## 2. 时钟树与寄存器

### 2.1 时钟树

```
osc 25 MHz ──> clk_mpll (MPLL, 850 MHz = 25 × 34)
                    │
                    └──> clk_c906_0 (mux + divider) ──> CPU core (hart 0, C906)
```

* `clk_c906_0` 是一个 mux/divider，**没有** `CLK_SET_RATE_PARENT` 标志。
  对它的 `clk_set_rate()` 只会去调分频器，不会上溯到 PLL；要改频必须直接对 `clk_mpll` 调用。
* `clk_mpll` 在时钟树里只有一个子节点 `clk_c906_0`（只喂 CPU），
  因此改它不会影响 DDR / 显示 / 视频 / USB（那些挂在 `fpll`、`tpll`、`disppll`、`mipimpll` 上）。

### 2.2 时钟控制器与 MPLL_CSR

设备树 `/proc/device-tree/clk-reset-controller`：

```
compatible = "cvitek,clk-reset"
reg        = <0x03002000 0x800>
```

MPLL 控制寄存器：`MPLL_CSR = 0x03002000 + 0x908`。字段布局（对照 mainline
`drivers/clk/sophgo/clk-cv1800.c` / `clk-cv18xx-pll.c`）：

| 位域 | 字段 | 说明 |
|---|---|---|
| `[6:0]` | pre_div | 参考分频 |
| `[14:8]` | post_div | 输出分频 |
| `[16:15]` | mode | 整数 / 小数模式 |
| `[23:17]` | div | 倍频 |
| `[26:24]` | ictrl | PLL 环路滤波 / 电流控制 |

`rate = 25 MHz × div / (pre_div × post_div)`

板上各 PLL 实测（用于交叉验证字段解释）：

| PLL | 偏移 | CSR | 解码 |
|---|---|---|---|
| mpll | `0x908` | `0x00448101` | pre=1 post=1 div=34 → 850 MHz |
| tpll | `0x90c` | `0x07708101` | div=56 → 1400 MHz |
| fpll | `0x910` | `0x07788101` | div=60 → 1500 MHz |
| mipimpll | `0x808` | `0x05488101` | div=36 → 900 MHz |

---

## 3. 为什么必须走 `clk_set_rate()`

最初尝试用 `devmem` 直接改分频位：

```sh
devmem 0x03002908 32 0x00488101     # div 34 -> 36
```

结果：

* 寄存器读回确实是 `0x00488101`，并且能一直保持；
* 但 **CPU 性能没有任何变化**（固定工作量基准在 850 与"900"设置下无系统性差异）；
* 内核 `clk_rate` 仍报 850 MHz（`clk_mpll` 的速率是内核缓存值，devmem 绕过驱动不会更新）。

改用内核时钟框架 `clk_set_rate(mpll, 900000000)` 后，寄存器变成：

```
0x07488101     # div=36，并且 ictrl 从 0 变成 7
```

这一次频率真正改变了（见第 6 节基准）。

**原因**：厂商 PLL 驱动 `cv181x_g6_pll_set_rate()` 在改分频的同时会重算 `ictrl`
（PLL 环路滤波/电流控制），并等待 PLL 重新锁定。只改 `div` 而保持旧 `ictrl`，
PLL 不会按新频率工作。所以：

| 状态 | `MPLL_CSR` | 频率 |
|---|---|---|
| 开机（div=34, ictrl=0） | `0x00448101` | 850 MHz |
| 裸 `devmem` 改 div=36 | `0x00488101` | 850 MHz（无效） |
| `clk_set_rate(900 MHz)` | `0x07488101`（div=36, ictrl=7） | 900 MHz |
| `clk_set_rate(850 MHz)` | `0x07448101`（div=34, ictrl=7） | 850 MHz |

> 注意最后一行：恢复 850 MHz 用的是"div=34 + ictrl=7"，与开机时的 `0x00448101`（ictrl=0）
> 不完全相同，这是驱动重算 ictrl 的结果，850 MHz 下运行正常。

---

## 4. 模块实现

源码：[`src/mpll_freq.c`](../src/mpll_freq.c)。

### 4.1 初始化（`module_init`）

1. `clk_get(NULL, "clk_mpll")` 取得 PLL 时钟；失败则返回错误码，`insmod` 会直接失败（安全）；
2. `ioremap(0x03002000, 0x1000)` 映射时钟控制器页，用于读寄存器（失败只告警，不影响改频）；
3. 打印当前频率；
4. 用 `INIT_DELAYED_WORK` + `schedule_delayed_work(delay_ms)` 延迟执行改频，
   让 `insmod` 先返回，并保证"即将改频"的日志已经写进 dmesg。

### 4.2 改频（workqueue 回调）

```c
csr_before = readl(regs + MPLL_CSR_OFF);
before     = clk_get_rate(mpll);
ret        = clk_set_rate(mpll, target);   /* 走厂商的 g6_pll_set_rate */
after      = clk_get_rate(mpll);
csr_after  = readl(regs + MPLL_CSR_OFF);
pr_info(...);                              /* 前后 CSR、返回值、前后频率 */
```

### 4.3 卸载（`module_exit`）

`cancel_delayed_work_sync()` → `iounmap()` → `clk_put()`。
**不会恢复频率**，PLL 保持卸载前的值。

### 4.4 参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `target` | `900000000` | 目标频率（Hz），必须是 25 MHz 的整数倍 |
| `delay_ms` | `5000` | 加载后延迟多久真正改频 |

---

## 5. 编译环境与 vermagic

设备内核是厂商用 **Xuantie GCC 10.2（musl）** 编译的 `5.10.4-tag-`，但模块并不需要同款编译器：

* 内核 `CONFIG_MODVERSIONS` **未开启** → 加载时只按**符号名**解析，不校验 CRC；
* `CONFIG_MODULE_SIG` **未开启** → 不需要签名；
* 只校验 **vermagic**（内核 release + SMP/PREEMPT/MODULE_UNLOAD + 架构）。

所以用 vanilla 5.10.4 + 设备 `.config` + 任意 riscv64 GCC 10/11 即可。

本次实际使用的环境：

| 项 | 值 |
|---|---|
| 编译机 | WSL2，Ubuntu 22.04（独立发行版 `maix-build`，由 ubuntu-base rootfs 导入） |
| 工具链 | `riscv64-linux-gnu-gcc 11.4.0` |
| 内核源码 | `linux-5.10.4.tar.xz`（kernel.org） |
| 配置 | 设备 `/proc/config.gz` 导出；`Makefile` 里 `EXTRAVERSION = -tag-` |
| 准备 | `make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- olddefconfig modules_prepare` |
| 构建 | `./build.sh /path/to/linux-5.10.4` |

vermagic 对比：

| | vermagic |
|---|---|
| 设备内核 | `5.10.4-tag- preempt mod_unload riscv64` |
| 厂商模块 `soph_ive.ko` | `5.10.4-tag- preempt mod_unload riscv64` |
| 本模块 | `5.10.4-tag- preempt mod_unload riscv64` ✔ |

> 设备上没有 `/lib/modules`，所以不能用 `modprobe`/`depmod`，直接 `insmod /path/mpll_freq.ko` 即可。

---

## 6. 实测数据

### 6.1 切换过程（dmesg）

```
mpll_freq: loaded, current rate = 850000000 Hz, will retune in 3000 ms
mpll_freq: MPLL_CSR before = 0x07448101
mpll_freq: rate before = 850000000 Hz, requesting 900000000 Hz
mpll_freq: MPLL_CSR after  = 0x07488101
mpll_freq: clk_set_rate() = 0, rate after = 900000000 Hz
```

* `clk_set_rate()` 返回 0；
* 内核 `clk_c906_0/clk_rate` 变成 `900000000`；
* 设备无死机，dmesg 无 warn/oops；
* 900 MHz 连续观测 2 分钟以上没有回落，多次 850↔900 往返切换正常。

### 6.2 性能

固定工作量：150 万次整数累加，`time.process_time()`，6 次取中位数（测试前停掉 launcher）：

| 设置 | 中位数 | 最小 |
|---|---|---|
| 850 MHz | 4.225 s | 4.184 s |
| 900 MHz | 3.734 s | 3.715 s |

900 MHz 比 850 MHz 快约 **11%**（理论 5.9%，实测偏大，与测试时系统负载有关，但方向明确）。

### 6.3 温度

| 状态 | 温度 |
|---|---|
| 空闲 850 MHz | 约 59.5 °C |
| 900 MHz（有负载） | 61.5 ～ 63.7 °C |

没有 cooling device 参与降频（内核没有 cpufreq）。

---

## 7. 失败模式与风险

| 情况 | 表现 | 恢复 |
|---|---|---|
| `target` 不合理（非 25 MHz 倍数、超范围） | PLL 重锁失败，可能死机 | **断电重启**（bootloader 会重新配置 850 MHz） |
| 重启 | 频率回到 850 MHz | — （需要重新 `insmod`，或写 init 脚本） |
| `rmmod` | 频率保持不变 | 重新 `insmod target=850000000` |
| 误改 `fpll/tpll/disppll/mipimpll` | 可能影响 DDR/显示/视频/USB，严重时损坏文件系统 | 断电重启 |
| 内核版本/配置不匹配 | `insmod` 报 vermagic 不匹配 | 用匹配的内核树重编 |

其他注意：

* 没有电压联动（无 cpufreq/OPP），更高的频率（如 1 GHz）不保证稳定，需自行验证；
* `CLK_IS_CRITICAL` 只阻止关闭时钟，不阻止 `clk_set_rate()`；
* 模块依赖 `clk_get` / `clk_set_rate`，它们是 GPL-only 导出符号，所以模块必须是 `MODULE_LICENSE("GPL")`。

---

## 8. 参考

* 内核时钟驱动（mainline）：`drivers/clk/sophgo/clk-cv1800.c`、`drivers/clk/sophgo/clk-cv18xx-pll.c`
* 设备树节点：`/proc/device-tree/clk-reset-controller`
* MaixCDK 频率读取：`components/basic/src/maix_sys.cpp`（`cpu_freq()` 解析 `clk_c906_0`）
* 内核符号（`/proc/kallsyms`）：`cv181x_g2_pll_set_rate`、`cv181x_g6_pll_set_rate`、`cv181x_clk_mux_set_parent`
* 相关内核配置：`CONFIG_MODULES=y`、`CONFIG_MODULE_UNLOAD=y`、
  `# CONFIG_MODVERSIONS is not set`、`# CONFIG_MODULE_SIG is not set`
