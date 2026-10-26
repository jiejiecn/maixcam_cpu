# maixcam_cpu — MaixCAM Pro CPU 频率控制

用内核模块调用 `clk_set_rate()` 修改 CPU 的 MPLL（`clk_mpll`），在 **850 MHz ↔ 900 MHz** 等整数档位之间切换。
适用于 MaixCAM Pro / LicheeRV Nano（SG2002，单核 RISC-V C906），内核 `5.10.4-tag-`。

## 现状

- CPU 实际工作 **850 MHz**，**固定频率，不随任务负载自动调节**（内核没有 cpufreq / DVFS）；
- MaixCDK / MaixPy 只有 `cpu_freq()`、`cpu_temp()` 等**读取**接口，没有设置接口；
- 本工具通过内核模块补上这个缺口，只动 CPU 的 `clk_mpll`，不碰 DDR/显示/视频/USB 时钟。

## 快速使用

```sh
# 上传
scp prebuilt/mpll_freq.ko root@<device>:/root/cpu_tool/

# 设为 900 MHz（delay_ms 让 insmod 先返回，再真正改频）
insmod /root/cpu_tool/mpll_freq.ko target=900000000 delay_ms=3000
dmesg | tail -6

# 验证
cat /sys/kernel/debug/clk/clk_c906_0/clk_rate     # 900000000
devmem 0x03002908 32                              # 0x07488101
cat /sys/class/thermal/thermal_zone0/temp         # 温度（毫摄氏度）

# 恢复 850 MHz（rmmod 不会恢复频率，需要重新 insmod）
rmmod mpll_freq
insmod /root/cpu_tool/mpll_freq.ko target=850000000 delay_ms=1500
rmmod mpll_freq
```

也可以直接用设备上的脚本：

```sh
./cpu_freq.sh status            # 看频率 / PLL / 温度 / 模块状态
./cpu_freq.sh set 900000000     # 设 900 MHz
./cpu_freq.sh restore           # 恢复 850 MHz 并卸载
```

参数：

| 参数 | 默认 | 说明 |
|---|---|---|
| `target` | `900000000` | 目标频率（Hz），必须是 25 MHz 的整数倍 |
| `delay_ms` | `5000` | 加载后延迟多久真正改频 |

## 目录

```
maixcam_cpu/
├── README.md / README_en.md   本文件 / English
├── docs/                      技术原理与实现细节
│   ├── DESIGN.md
│   └── DESIGN_en.md
├── build.sh                   编译脚本
├── src/                       模块源码 + Makefile
├── prebuilt/mpll_freq.ko      编译好的模块
└── scripts/cpu_freq.sh        设备端便捷脚本
```

## 编译

需要 Linux（推荐 WSL2）+ 5.10.4 内核树（`EXTRAVERSION=-tag-`）+ 设备 `.config` + `riscv64-linux-gnu-gcc`：

```sh
./build.sh /path/to/linux-5.10.4
```

产物在 `prebuilt/mpll_freq.ko`，其 vermagic 必须是 `5.10.4-tag- preempt mod_unload riscv64`。
完整步骤见 [docs/DESIGN.md](docs/DESIGN.md)。

## 频率扫描实测（850–1000 MHz）

测试方法：每个频点用模块切频后，跑 6 次固定工作量基准（150 万次整数累加，取最好值；
两轮扫描取最优），再用 25 秒满载测温度。测试前已停掉 launcher 以减小干扰。

| 目标频率 | `MPLL_CSR` | benchmark 最好值 | 相对 850 | 温度（空闲 / 满载） |
|---|---|---|---|---|
| 850 MHz | `0x07448101` | 3.936 s | 1.00× | 60.9 / 61.2 °C |
| 875 MHz | `0x07468101` | 3.816 s | 1.03× | 60.5 / 61.2 °C |
| 900 MHz | `0x07488101` | 3.723 s | 1.06× | 60.5 / 60.9 °C |
| 925 MHz | `0x074A8101` | 3.612 s | 1.09× | 60.2 / 61.2 °C |
| 950 MHz | `0x074C8101` | 3.590 s | 1.10× | 60.2 / 60.9 °C |
| 975 MHz | `0x074E8101` | 3.433 s | 1.15× | 60.2 / 61.2 °C |
| 1000 MHz | `0x07508101` | 3.437 s | 1.15× | 60.2 / 60.9 °C |

观察：

* 850 → 975 MHz 性能基本按频率线性提升（实测 1.03/1.06/1.09/1.10/1.15，理论 1.03/1.06/1.09/1.12/1.15）；
* **975 → 1000 MHz 没有进一步收益**（实测 1.15× vs 理论 1.18×），可能是 CPU 侧 AXI（`clk_cpu_axi0` 固定 500 MHz）
  或内存带宽开始成为瓶颈；
* **温度在 60.2～61.2 °C 之间波动，基本不随频率变化**：25 秒负载不足以拉开差距，
  且热传感器分辨率约 0.35 °C；
* 1000 MHz 下系统稳定（无死机、dmesg 无 warn/oops），但没有电压联动，长期稳定性需自行评估。

> 注：Python 循环基准受系统负载影响较大（同一频点两轮结果可差 20%～30%），表中取两轮扫描的最好值，
> 因此绝对值只适合同表内横向比较。

---

## 注意

- **不持久**：重启后回到 850 MHz；
- **`rmmod` 不恢复频率**：卸载只 `clk_put()`；
- 改频瞬间 PLL 会重锁，参数不合理可能死机，**断电重启即可恢复**；
- 只改 `clk_mpll`，不要动 `fpll/tpll/disppll/mipimpll`。

## 延伸阅读

原理、寄存器细节、实现方式、实测数据与风险分析：[docs/DESIGN.md](docs/DESIGN.md)。
