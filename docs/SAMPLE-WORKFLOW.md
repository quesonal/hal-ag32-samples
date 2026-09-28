# AGM sample 工作流:新建 / 编译 / 烧录 / 调试

适用对象:在本仓库(hal_ag32 module repo)里加示例、在 agrv2k_407 开发板上验证的人。
相关文档:`FLASH-AND-CAPTURE.md`(烧录/抓串口的规则与故障对照表,**必读**)、
`../tools/README.md`(每个脚本逐个说明)、`BOARD-VE-FROM-DTS.md`(比特流侧流程)、
`开发记录（未随本仓库发布）`(各外设状态)、`开发记录（未随本仓库发布）`(外设实测细节)。

---

## 0. 先搞清两件事(90% 的"改了不生效"来自这里)

### 0.1 两个仓库:改的是这个,编译的是那个

`hal_ag32` 是面向 Zephyr 的 AgRV2K HAL **模块**(驱动、SoC、板、devicetree、west runner、
模块级工具与 docs);`hal_ag32_samples` 是它拉来作 west 子模块的**伴生仓库**,
装 58 个样例与样例专属工具。下面的"仓库"分别指这两份。

| 路径 | 角色 |
|---|---|
| `~/hal_ag32` | **module repo**:`drivers/`、`dts/`、`boards/`、`include/`、`soc/`、`scripts/`、`tests/`、`tools/`(模块级)、`docs/` 的唯一信源 |
| `~/hal_ag32_samples` | **伴生仓库**(`hal_ag32_samples`):`samples/`、`tools/`(样例专属) ——  `west update` 把这份拉成 `<ws>/modules/hal_ag32_samples/`,所以样例的源是 `<ws>/modules/hal_ag32_samples/samples/` |
| `~/zephyrproject/modules/hal_ag32` | west 给模块建的**另一份 checkout**;`west build` 实际编译的是这一份 |
| `~/zephyrproject/modules/hal_ag32_samples` | west 把伴生仓库放的另一份 checkout;样例的源码也在这一份里 —— 改完样例后这里也得看到 |
| `~/zephyrproject/zephyr` | Zephyr 树(本移植尽量保持干净,不放 AGM 专属改动) |

⇒ 在两个 repo 改完代码,**构建前必须让第二份看到**。两种方式:

**改哪一份**:`west build` 编译的是 `<ws>/modules/hal_ag32` 与
`<ws>/modules/hal_ag32_samples` 这两份 checkout(`hal_ag32` 作为 manifest 被 west
放在那里,`west.yml` 拉伴生仓库为子模块)。所以要么直接在那两份里改,要么在自己另建的
checkout 里改完、把改动同步过去再构建 —— 两种都行,关键是**构建读到的那份必须和你以为
的一致**。

如果你习惯维护两份 checkout(一份编辑、一份给构建用),`hal_ag32/tools/devsync.sh`
是这类镜像的助手:rsync 按内容比对,保护 `.git` 与构建生成物,动作只有 `status` /
`push` / `restore` 三个,默认 dry-run。它是**可选**的 —— 直接在
`<ws>/modules/hal_ag32` 里开发同样可以,不依赖任何本机脚本。之所以能这么做:hal_ag32
是工作区的 manifest 仓库,而 `west update` 不会改动 manifest 仓库的内容。

> ⚠ **证据要来自干净树**:要写进 `docs/` 的"实测",必须能对上某个提交或某个明确的工作区
> 状态;临时改出来、还没定稿的树适合看现象、不适合当证据(记结论时带上板卡、比特流文件
> 与构建参数)。

### 0.2 比特流决定引脚与时钟,固件必须与它一致

开发板约定:

| 资产 | 位置/取值 |
|---|---|
| 控制台 | `/dev/ttyACM0`(probe 的 UART 桥,115200 8N1) |
| 固件烧录地址 | `0x80000000` |
| **比特流**烧录地址 | `0x800e7000`(`AGM_BITSTREAM_ADDR`,Plan A 后由 `west flash --bitstream-only` / 默认模式一起写) |
| 比特流默认路径 | `${CMAKE_BINARY_DIR}/zephyr/board.bin`(`west build -t bitstream` 产物;显式 override 用 `$AGM_BITSTREAM_BIN` env 前缀,或建 build dir 时 `west build … -- -DAGM_BITSTREAM_BIN=<path>`。**`west flash` 不认 `-D`**。板级默认 runner 是 `agrv_openocd`(2026-09-17 起),Plan A 的三态是它的默认行为,不用写 `--runner`) |

**开发阶段唯一 canonical 比特流**(回归测试、驱动开发、sample 设计都按这份来,只有特殊
需要才换别的):

```
<your canonical bitstream>   ← 厂方 SDK reference
<your canonical bitstream> (+ .ve)        ← 仓库内副本,md5 相同 —— 见
                                                      samples/bitstreams/README.md
md5 6378549f3a8f82dd386353077f3d4a02                ← 99944 B
SYSCLK 200 / BUSCLK 100 / HSECLK 8
```

它与 407 板级默认(200/100 MHz)一致 → **不用 overlay**;2026-09-16 读回 `0x800e7000`
起的 100000 B 与之逐字节相同(板上就是它),它的 `.ve`/`.v` 与 `tools/tests/fixtures/`
那份也逐字节相同。

**用法**(Plan A 之后):

```bash
# override 指过去(避免 cp,比特流留在原位;env 前缀,不是 `west flash -D...`)
AGM_BITSTREAM_BIN=<your canonical bitstream> \
    west flash -d /tmp/b_<name>

# 或拷一份到当前 build dir,走默认路径
cp <your canonical bitstream> <build_dir>/zephyr/board.bin
west flash -d <build_dir>
```

两个坑:厂方那份叫 `example_board.bin`(只有我们 build 树的产物叫 `board.bin`),
`-D...=~/x` 里的波浪号 shell 不展开,所以写 `$HOME`。

其它比特流(特殊需要时才用;引脚与能力对照见 `开发记录（未随本仓库发布）`的
"开发板比特流对照"表):

| 比特流 | 用途 |
|---|---|
| `$HOME/spi_full_bitstream_97pad/example_board.bin` | SPI0 经 fabric → flash,且引出 IO0–IO3:**quad 测试用** |
| `$HOME/spi_full_bitstream_without_flash/example_board.bin` | 只有 fabric 补丁 + 回环跳线,无 flash |

**固件 DT 的时钟必须与比特流一致**:`&clk0`(SYSCLK,UART/SPI 分频都用它)、`&cpu0`(内核 tick)、
`&sys` 的 `flash-max-frequency`。canonical 是 200 MHz,与板级默认一致,所以**默认构建不带
overlay**;换了别的比特流(尤其那几份 100 MHz 的)才需要确认频率并相应加
`-DEXTRA_DTC_OVERLAY_FILE=/tmp/agm100.overlay`(该文件不落库,用
`bash tools/make_agm100_overlay.sh` 生成)。两招判定:

* 开机横幅现在会打印 `SYSCLK: N MHz (clk0), cpu0 N MHz`(从 DT 读,不是硬编码);
* 同一份固件烧两遍(带/不带 overlay):哪次串口清晰,比特流就是哪个频率。

### 0.3 卡住时先读"容易误判"清单

"调不通"时先看 [`FLASH-AND-CAPTURE.md` §9](FLASH-AND-CAPTURE.md) —— 那里列了最容易走错的
几步(把软件 bug 判成硬件限制、拿 `west build` 当回归网、结论写在验证之前……),
每条都给了正确做法。其中对本文最直接的两条:

* **先确认"代码路径有没有被走到",再调参数**(典型:一个 Kconfig 没开,参数在编译期就被
  截断了,而 C 不会报错);
* **`west build` 只打 warning,twister 才是回归网**(带 `WARNINGS_AS_ERRORS` 与
  `--edtlib-Werror`),改完驱动或样例必须跑一次 twister。

---

## 1. 新建 sample

### 1.1 目录结构

```
samples/<name>/
├── CMakeLists.txt          # find_package(Zephyr) + target_sources
├── prj.conf                # 该 sample 需要的 CONFIG_*
├── sample.yaml             # twister 元数据(build_only + platform_allow + tags)
├── Kconfig                 # 可选:sample 自己的 APP_* 旋钮(末尾必须 source "Kconfig.zephyr")
├── boards/<board>.overlay  # 可选:启用节点/加 pinctrl/sample 专属引脚
├── ip/<name>.v             # 可选:fabric IP 的 RTL(AGM_LOGIC_IP 按名字取;见 §1.4)
├── src/main.c
└── README.md               # 硬件前置(比特流+跳线)、抓取命令、实测输出
```

`../samples/README.md` 的 "Adding a new sample" 是同一套约定的简版;本文是展开版(从这里访问
该 README:`samples/README.md`,相对本文件)。

### 1.2 每个文件的最小内容

`CMakeLists.txt`

```cmake
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28.0)

# 可选:fabric IP,按名字声明(源码取自本 sample 的 ip/<name>.v,外加
# 可选的 ip/<name>_core.v)。第二个及之后的 IP 用 AGM_USER_RTL 列出来 ——
# 每一条都会被 stage 进 <build>/logic 并登记进 board.qsf,但**只有
# AGM_LOGIC_IP 那一个**会被 gen_vlog 实例化(见 §1.4 / docs/CUSTOM-IP.md 1.5)。
# set(AGM_LOGIC_IP "full_duplex_spi")
# set(AGM_USER_RTL "${CMAKE_CURRENT_SOURCE_DIR}/ip/dual_ip.v"
#                  "${CMAKE_CURRENT_SOURCE_DIR}/ip/full_duplex_spi.v")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(<name> LANGUAGES C)

target_sources(app PRIVATE src/main.c)
```

`prj.conf`(示例:控制台 + 一个外设;按需增减)

```conf
CONFIG_SERIAL=y
CONFIG_UART_CONSOLE=y
CONFIG_PRINTK=y
CONFIG_STDOUT_CONSOLE=y
CONFIG_XIP=y
CONFIG_<SUBSYS>=y
```

`sample.yaml`

```yaml
# SPDX-License-Identifier: Apache-2.0
sample:
  name: <name>
  description: |
    一句话说清它证明什么,以及需要什么硬件/bitstream。
tests:
  sample.<name>.agm_agrv2k_407:
    build_only: true
    platform_allow:
      - agrv2k_407
    integration_platforms:
      - agrv2k_407
    tags:
      - ci_build
      - hal_ag32
```

* `build_only: true`:开发板不在 CI 里,只保证能构建;
* **板级 `boards/agm/<board>/twister.yaml` 必须存在**,否则 twister 会静默丢掉这块 platform
  (现象:`unrecognized platform` / 用例被过滤)。新增板卡时第一件事就是补它。

`boards/<board>.overlay`(只在需要时;Zephyr 会自动发现这个路径)

```dts
/* SPDX-License-Identifier: Apache-2.0 */
/* sample 需要但默认 disabled 的节点 */
&spi1 {
	status = "okay";
};

&cpld0 {
	status = "okay";
};

&dma0 {
	status = "okay";
};
```

`Kconfig`(有旋钮才需要)

```
mainmenu "<name> sample"

config APP_<NAME>_<KNOB>
	bool "..."
	default y
	help
	  ...

source "Kconfig.zephyr"
```

`src/main.c` —— 写"验收友好的样例"三条:

1. 每个用例输出**一行可判定的结论**(`-> PASS` / `-> FAIL`),机器/人眼都能判;
2. 把判定依据一起打出来(寄存器值、校验和、计数、前几个字节);
3. 先自检(外设是否在位),再做对照(两条通路读同一份数据),最后判定。

```c
/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

int main(void)
{
	printk("\n<name>: <what this proves>\n");
	printk("<name>: [1] <check> = 0x%08x -> %s\n", value, ok ? "PASS" : "FAIL");
	return 0; /* 需要常驻就 while (1) { k_msleep(1000); } */
}
```

### 1.3 风格与提交约定

* C:Tab 缩进、单行 ≤100 列、公共符号 `agm_<驱动>_<动作>`、新文件带 SPDX + Copyright 头;
* 检查:`checkpatch.pl`(内核风格)、`yamllint`(YAML);
* 用例 id:`sample.<name>.<platform>`;需要开发板的用例 `build_only: true` + `platform_allow`;
* 每个新增驱动 API 至少 **1 正例 + 1 反例**,负例同样要在串口可判(例如"该被拒绝的请求返回 -ENOTSUP");
* commit:英文祈使句、标题 ≤72 字符、正文说清"为什么",必须带
  `Signed-off-by: quesonal <quwescnotp@outlook.com>`;
* 文档里**实测与推断分开写**(`开发记录（未随本仓库发布）` 是范例)。

### 1.4 fabric IP(可选,`AGM_LOGIC_IP` / `AGM_USER_RTL`)

sample 需要比特流里放自己的 Verilog 时(而不是只用板级 pin map),两种声明方式:

* **按名字** —— `set(AGM_LOGIC_IP "<name>")`,源码按约定取自 `<app>/ip/<name>.v`
  (+ 可选 `<name>_core.v`),等价于厂商 `platformio.ini` 的 `ip_name`;
* **按路径** —— `set(AGM_USER_RTL "<a.v>;<b.v>")`(列表,`;` 或空格分隔),显式覆盖。

两者都把文件 stage 进 `<build>/logic/` 并登记成 Quartus 的 `VERILOG_FILE`,然后交给
`gen_vlog -m <AGM_LOGIC_IP>.v` —— **只有这一个会被实例化**。因此:

* **第二个及之后的 IP 不靠第二条 `-m`**(SDK 的 `-m` 是单值):写一个顶层 wrapper
  `.v`,让它 instantiate 其它模块,把 wrapper 设成 `AGM_LOGIC_IP`、其余文件列进
  `AGM_USER_RTL`。现成例子:`samples/dual_ip`(厂商 `custom_ip` RAM + `full_duplex_spi`,
  wrapper 里按地址位分窗口);单 IP 的例子是 `samples/spi_quad_read`;
* **MCU 要"读回"的那几根焊盘,case-A 行得再写一遍**(`agm,mcu-input-pins`,`PIN_92:INPUT`
  那种):少了它 wrapper 会把 MCU 的接收通路接成 `1'b0`,板上就是 `RDID = 00 00 00`
  (2026-09-25 实测,见 `开发记录（未随本仓库发布）`);
* **用户逻辑区只有 4 块 M9K**:两个吃块 RAM 的 IP 要自己分配(SPI IP 的 RX FIFO 占 1,
  `custom_ip` 默认 4 KiB RAM 要 4 —— 超了 Quartus 报 `Error (170051)` 然后
  `Can't fit design in device`;`custom_ip` 的 `RAM_SIZE` 是参数,降到 2048 就是 2 块);
* 新旋钮 `AGM_LOGIC_IP_FLOW=off` 跳过 Step 0b 的 IP-prepare,只保留 `-m`。

细节与边界(声明是列表但只实例化第 0 个、厂商的层次化套路)见
[`CUSTOM-IP.md`](CUSTOM-IP.md) §1.5;把它拿到 Quartus 机器上的规矩见
[`FLASH-AND-CAPTURE.md`](FLASH-AND-CAPTURE.md) §0 规则 3。

---

## 2. 编译

```sh
source <your-venv>/bin/activate
cd ~/zephyrproject
west build -d /tmp/b_<name> -b agrv2k_407 \
    modules/hal_ag32_samples/samples/<name> \
    -- -DCONFIG_COMPILER_WARNINGS_AS_ERRORS=y     # canonical 比特流:不用 overlay
```

| 要点 | 说明 |
|---|---|
| `-d /tmp/b_*` | 构建目录放 `/tmp`,不污染工作区 |
| `--pristine=always` | **改了 `prj.conf` / overlay / Kconfig 后必须用**(否则 cmake cache 会让改动看似"不生效");日常小改用 `--pristine=auto` |
| 时钟 overlay | canonical 比特流是 200 MHz,与板级默认一致 → **不加**;换 100 MHz 比特流才加 `-- -DEXTRA_DTC_OVERLAY_FILE=/tmp/agm100.overlay` |
| 多个 overlay | 用 `;` 分隔:`-DEXTRA_DTC_OVERLAY_FILE="/tmp/a.overlay;/tmp/b.overlay"` |
| 与 twister 对齐 | twister 默认开 `-DCONFIG_COMPILER_WARNINGS_AS_ERRORS=y`,本地也加上更省事 |

全量回归(每个 sample 都要能过):

```sh
west twister -T modules/hal_ag32_samples/samples -p agrv2k_407 --build-only -O /tmp/tw_<x>
# 期望:选中 69 个 scenario = 69 个 configuration、7 个被静态过滤,
#       **62 个构建、0 failed / 0 errored、0 warning**(2026-09-25,含 sample.dual_ip)。
# 数字变了先看是不是新 sample 少了 platform_allow,或板级 twister.yaml 丢了
# —— 旧值 2026-09-18 是 52 scenario,2026-09-24 是 59。
```

看资源与配置:

```sh
west build -d /tmp/b_<name> -t rom_report
west build -d /tmp/b_<name> -t ram_report
west build -d /tmp/b_<name> -t menuconfig
```

常见编译问题:

| 症状 | 原因 / 处理 |
|---|---|
| `#error "no ... node is enabled"` | sample 的 `boards/<board>.overlay` 没启用该节点,或路径/命名不对 |
| 隐式声明 / 链接缺符号 | 少了对应 `CONFIG_*`(如 `CONFIG_SPI_ASYNC`、`CONFIG_PM_DEVICE_RUNTIME`、`CONFIG_DMA`) |
| 改了驱动却不生效 | 构建读的是 `modules/hal_ag32`,没同步 |
| `-Werror` 报 unused | 用 `IS_ENABLED()`/`#if defined(CONFIG_*)` 包住只在某配置下的变量与函数 |
| overlay 没生效 | 确认是 `samples/<name>/boards/<board>.overlay`(自动发现),而不是随手放的文件 |

---

### 2.1 设备在环(HIL):让 twister 也"跑",而不只是"编"

> 放在"编译"这一章的尾上,因为它就是 `west twister` 的另一种用法,不是新的流程。

普通回归跑的是 `west twister -T samples -p agrv2k_407 --build-only` —— **只构建不运行**,
所以它能挡住编译期问题,挡不住"构建通过、上板却错"的回归。2026-09-25 那条正好是后者:
`soc.c` 里 AHB 域门控只在 `CONFIG_CRC_AGM` 分支被调用,DMAC0 一直处于门控态,
`dma_memcpy` 与 SPI 长读全部失败,而**所有样例都构建绿**。

现在两个样例各带一个 `harness: console` 的 HIL scenario,twister 会自己
构建 → 烧录 → 跑 → grep 串口:

```sh
export AGM_BITSTREAM_BIN=$HOME/spi_full_mac_bitstream_200mhz/example_board.bin
west twister -T modules/hal_ag32/samples -p agrv2k_407 \
    --device-testing --device-serial /dev/ttyACM0 --west-flash \
    -s sample.dma_memcpy.hil.agm_agrv2k_407 \
    -s sample.spi_flash_rw.hil.agm_agrv2k_407
```

| scenario | 断言 | 说明 |
|---|---|---|
| `sample.hello_world.hil.agm_agrv2k_407` | `*** Booting Zephyr OS` | 最便宜的"板子+串口+比特流"金丝雀;它红就别看别的了 |
| `sample.dma_memcpy.hil.agm_agrv2k_407` | `dma_memcpy: iter N OK` | SRAM↔SRAM DMA;门控/时钟一动就红 |
| `sample.spi_flash_rw.hil.agm_agrv2k_407` | `spi_flash_rw: PASS` | 含 SPI 引擎的 RX DMA;**会擦写片上 NOR 的 0x000000..0x000fff**,只在开发板板跑 |
| `sample.rtc_alarm.hil.agm_agrv2k_407` | `rtc_alarm: alarm cancelled (err 0) - PASS` | LSE 1 Hz + 三次 5 s alarm(~20 s) |
| `sample.wdt_feed.hil.agm_agrv2k_407` | `phase A done - N ISR timeouts without reset` → `RST_CNTL=0x…(WDOG=1` | WDOG0 两半:INT-only 不复位、停喂后复位 |
| `sample.iwdg_basic.hil.agm_agrv2k_407` | `iwdg_basic: IWDG reset confirmed (run #2 PASS)` | backup 域 IWDG,复位后自证 |

注意事项:

* `AGM_BITSTREAM_BIN` 要指比特流(`west flash` 默认写固件 + 比特流;不设、build dir 里又没有
  `board.bin` 时,比特流那步会失败或跳过);
* 需要探针与串口空闲(与 `minicom` / `cat` 之类互斥);
* 探针/CMSIS-DAP 偶发 flake,报错先原样重跑;
* **一次带多个 `-s` 时 twister 会并行构建**:以前几个 job 同时写源码树的同一批生成物,
  会看到孤立的 `CMake Error ... configure_file: No such file or directory`。**2026-09-25 起
  生成物都在 `<build>/logic/`,这条竞态已根治**(同日 `-j 32` 全量 61/61 构建 0 错误),
  `-j 1` 不再是必需;撞上孤立的 CMake 失败仍先**单跑那一条**复验;
* 这些 scenario 带 `tags: hil`,普通 `--build-only` 计数里能看到它们但不产生通过项
  (计数见 `开发记录（未随本仓库发布）`的构建矩阵);
* 它们**不是** `build_only`:twister 会把它们当真用例调度,所以别塞进没有板子的 CI 机;
* 跑完清理:`tools/clean_twister.sh --apply`(脚本里带了 `--clobber-output`,单次 `-O`
  不再堆 `.1/.2` 副本,但历次 build dir 仍要清 —— twister 默认的"清理"其实是改名保留)。
* 2026-09-25 板上实测:`../tools/test_hil.sh`(默认 `-j 1`)一次跑完 **6/6 PASS**,约 **107 s**;
  早前把四条一起并行跑时 `hello_world.hil` 被上面那条竞态打中,单跑即过。

## 3. 烧录

### 3.1 一步到位(推荐):烧固件 → 复位 → 抓串口 → 判定

```sh
cd ~/hal_ag32_samples   # 或者 modules/hal_ag32_samples/ —— 都有 test_uart_capture.sh
bash modules/hal_ag32/tools/test_uart_capture.sh -t 12 /tmp/b_<name>/zephyr/zephyr.bin
```

* `-t N` 抓取窗口(等待型 sample 给 25–40 s);
* `-n` 不擦固件区(仅当目标页已是 0xFF;快速迭代用);
* **不带固件参数**则只做 `reset run`(抓当前固件,换比特流后常用);
* 输出末尾的判定:`printable_ratio`(≥90% 正常)+ `RESULT`;捕获文件在
  `/tmp/uart_capture_<board>_<ts>.bin`,用 `strings` 看文本;
* 读串口的进程在复位**之前**启动,所以不会漏掉开机横幅。

### 3.2 分开烧(按需)

```sh
west flash -d /tmp/b_<name>                    # 默认 firmware + 比特流(Plan A)
west flash ... --skip-bitstream                # 只写固件(sector erase 保留比特流)
west flash ... --bitstream-only                # 只写比特流(firmware 区零接触)
west flash ... --runner agrv32flash            # 没探针时走 UART ROM bootloader(只写固件)

# 旧入口已 DEPRECATED,直接调用仍能用,但已不接 west target:
#   bash tools/flash_fw.sh <zephyr.bin>            # 等价于 west flash --skip-bitstream
#   bash tools/flash_logic.sh [<board.bin>]       # 等价于 west flash --bitstream-only
#   west build -t flash                            # CMake target 已删,报 "no rule to make target"
#   west build -t flash-logic                      # 同上
```

换比特流后**固件不用重编**,但先确认 DT 时钟与比特流一致,不一致就重编并带/不带
`/tmp/agm100.overlay`(canonical 200 MHz 那份不需要 overlay)。

### 3.3 比特流侧(需要改引脚/加 fabric 逻辑时)

```
dtsi ──tools/build_bitstream.sh(AGM_DTS=auto,本机)──► logic/
                                                        │  quartus_sh -t af_quartus.tcl
                                                        │        ↑ 这一步在装有 Quartus 的机器上(用户)
                                                        ▼
                                          simulation/modelsim/<design>.vo
                                                        │  tools/compile_bitstream.sh(本机 Linux Supra)
                                                        ▼
                                                 <design>.bin ──► $AGM_BITSTREAM_BIN
```

* `west build -t logic` 也会生成 `logic/`(它用构建后的 `zephyr.dts`,sample overlay 改的引脚
  会一并生效);
* **Quartus 那一步要把 `logic/` 目录原样综合**:`board.v` 与 `board.vex` 是同一次
  `gen_vlog` 的两半,在里面重跑 SDK prepare logic 会把焊盘约束换成另一种命名
  (`SPI0_SI_IO0 PIN_92` vs `si_io0`),Quartus 不报错、Supra 只 warning 就把这些 IO
  摆到别的焊盘上 —— 板上表现是 SPI 读不到 flash。`../tools/build_bitstream.sh` 的 Step 1c
  在生成时就会挡(`exit 8`;`AGM_ALLOW_VEX_PORT_MISMATCH=1` 降级成 warning)。回传只需要
  `simulation/modelsim/<design>.vo`;
* sample 带自己的 fabric IP 时,`logic/` 里会有 `ip` 的源文件(`AGM_LOGIC_IP` /
  `AGM_USER_RTL` 的产物)与 `logic_ip/` 的 IP 工程,见 §1.4;
* 详细说明:`BOARD-VE-FROM-DTS.md`、`../tools/README.md`。

---

## 4. 调试

### 4.1 串口取证(第一手段)

```sh
f=$(ls -t /tmp/uart_capture_agrv2k_407_*.bin | head -1)
strings "$f" | tail -30                  # 看文本
strings "$f" | grep -aE "PASS|FAIL"       # 看结论
```

| 现象 | 先怀疑 | 处理 |
|---|---|---|
| 断行 / 缺字符 / `[....` 没有右括号 | UART0 AFSEL 引脚路由 | `开发记录（未随本仓库发布）`|
| 整段乱码 | DT 时钟 ≠ 比特流 | §0.2 的两招判定 |
| 捕获为空 | probe/USB 桥掉线 | 重插 USB;`tools/openocd_warmup.py` 的预热失效会报 `could not read product string` |

### 4.2 OpenOCD(看寄存器、PC、复位)

```sh
bash tools/openocd_reset_run.sh      # 最小 init / reset run / shutdown
bash tools/openocd_monitor.sh        # 前台会话,带 semihosting + 串口抓取
```

本仓库里真正用来定位问题的方式(都在公开文档里能找到源委):

* **外设"没反应"时先读它的寄存器**:例如 SPI 的 `CTRL` / `PHASE_CTRL`,看到各相位 `DONE`
  但全局 `SPI_DONE` 未置,就能判定引擎卡在等待数据(§3.27.8 的 TX-DMA 死锁);
* **中断不进去**时读 PLIC 的 `ENABLE` / `PENDING`:曾据此发现 `irq_enable()` 用了错的编号
  (PLIC ENABLE 位没置 → 永远不产生 MEIP);
* **时钟门**看 `SYS.APB_CLKENABLE`;注意**门关着时读外设寄存器不可信**;
* 需要 GDB 时用 `west attach --runner agrv_openocd`(只连、**不烧写**);
  **不要**用 `west debug`:它的 runner 会先"flash the program",而这条路径对本板会整片擦除
  (见 §3.2),比特流会被抹掉。

### 4.3 "留证据"的原则(SPI 那轮的经验)

1. **先自检**:外设/补丁是否在位(例:fabric 补丁的 CTRL 回读是否等于预期值);
2. **再对照**:用两条独立通路读同一份数据(裸命令 vs 上游驱动、单线 vs 四线、
   fabric 捕获 vs 引擎 RX、软件轮询 vs 硬件 POLL);
3. **最后判定**:校验和 / 计数 / 逐字节比较,并把结论打成 `PASS`/`FAIL`;
4. **别用"看起来对"当结论**:反面例子见 §3.27.9 —— 全空白的 flash 让"整页读通过"成了
   假通过,真正的 bug(读偏移 4 字节)被掩盖了一整天。

### 4.4 已知陷阱速查

| 陷阱 | 现象 | 处理 |
|---|---|---|
| 构建用 `modules/hal_ag32` | 改了代码"没生效" | 同步 |

| DT 时钟 ≠ 比特流 | 串口乱码 / tick 漂移 | 加/去 `agm100.overlay`;看横幅 |
| UART0 AFSEL | 打印断行 | 见 §4.1 |
| 门控后读寄存器 | 读到 0 或陈旧值 | 先 `pm_device_runtime_get()` 或开门 |
| probe 用久了掉线 | `/dev/ttyACM0` 消失 | 重插 USB |
| twister 看不见板卡 | `unrecognized platform` | 补 `boards/agm/<board>/twister.yaml` |
| 比特流未引出某 IO | 外设"没反应" | 查比特流顶层端口(§3.26 比特流对照表) |
| `-DEXTRA_DTC_OVERLAY_FILE` 拼错 | 节点仍 disabled / 时钟没改 | 看构建目录里 `zephyr.dts` 的实际值 |

### 4.5 收尾(把开发板还回已知状态)

```sh
# 比特流:走 default `west flash` 时会落到 `<build>/zephyr/board.bin`,Plan A 之后无单独回 canonical 命令;
# 如果要从原始厂方源文件回 canonical 而不重新 Quartus,直接 override:
#   AGM_BITSTREAM_BIN=<your canonical bitstream> \
#       west flash -d /tmp/b_hello
west build -d /tmp/b_hello -b agrv2k_407 --pristine=always \
    modules/hal_ag32_samples/samples/hello_world
bash modules/hal_ag32/tools/test_uart_capture.sh -t 6 \
    /tmp/b_hello/zephyr/zephyr.bin # 固件:回 hello_world
```

顺手清理 `/tmp/b_*` 与旧捕获文件;确认 `git status` 干净、twister 通过、文档已更新
(实测与推断分开)、提交带 `Signed-off-by`,module repo push 后再同步 `modules/hal_ag32`。

---

## 附录 A:命令速查

| 目的 | 命令 |
|---|---|
| 激活环境 | `source <your-venv>/bin/activate`(west / pyserial / pyusb / pyyaml / devicetree / pytest) |
| 编译一个 sample | `west build -d /tmp/b_x -b agrv2k_407 <sample>`(canonical 比特流不用 overlay;换 100 MHz 比特流才加 `-- -DEXTRA_DTC_OVERLAY_FILE=/tmp/agm100.overlay`) |
| 全量构建回归 | `west twister -T modules/hal_ag32_samples/samples -p agrv2k_407 --build-only -O /tmp/tw_x` |
| 原生 API 测试(不用板子,模块仓库的 native_sim 用例) | `west twister -T ~/hal_ag32/tests -p native_sim -O /tmp/tw_native`(构建**并运行** boot 驱动的公开 API 用例,见 `the development notes (not published here)`) |
| 编译+烧录+抓串口 | `bash modules/hal_ag32/tools/test_uart_capture.sh -t 12 /tmp/b_x/zephyr/zephyr.bin` |
| **烧录(默认 firmware + 比特流)** | `west flash -d /tmp/b_x`(默认 runner `agrv_openocd`) |
| 只烧固件(sector erase 保留比特流) | `west flash -d /tmp/b_x --skip-bitstream` |
| 只烧比特流 | `west flash -d /tmp/b_x --bitstream-only` |
| 没探针时(UART ROM bootloader) | `west flash -d /tmp/b_x --runner agrv32flash`(只写固件) |
| 烧录 + 抓 banner | `bash modules/hal_ag32/tools/test_uart_capture.sh [-n] -t <s> [/path/to/zephyr.bin]` |
| 只抓 banner | `bash modules/hal_ag32/tools/test_uart_capture.sh -t <s>` |
| 生成 Quartus 目录 | `AGM_DTS=auto tools/build_bitstream.sh <board_dir> <logic_dir>`(或 `west build -t logic`) |
| 出比特流(Quartus 之后) | `west build -d /tmp/b_x -t bitstream -b agrv2k_407`(等价于 `tools/compile_bitstream.sh <logic_dir>`) |
| 复位/看 semihosting | `bash tools/openocd_reset_run.sh` / `bash tools/openocd_monitor.sh` |
| 看上一次捕获 | `strings $(ls -t /tmp/uart_capture_agrv2k_407_*.bin | head -1)` |

## 附录 B:一次完整的"新建 sample"演练

以 `samples/spi_flash_rw` 为模板(它集合了本文的所有要素):

1. **建目录**:`samples/<name>/{src,boards}` + 上面 §1.2 的六个文件;
2. **写 main.c**:每个用例一行 `-> PASS/FAIL`,并把依据(寄存器/校验和)打出来;
3. **overlay**:按需 `&spi1 { status = "okay"; }; &dma0 { status = "okay"; };`
   (需要 flash 时还要 `flash@0 { compatible = "jedec,spi-nor"; ... }`);
4. **编译**:`west build -d /tmp/b_<name> -b agrv2k_407 modules/hal_ag32_samples/samples/<name>`(canonical 比特流 200 MHz → 不用 overlay;换 100 MHz 比特流才加 `-- -DEXTRA_DTC_OVERLAY_FILE=/tmp/agm100.overlay`);
5. **烧录 + 抓取**:`bash modules/hal_ag32/tools/test_uart_capture.sh -t 30 /tmp/b_<name>/zephyr/zephyr.bin`;
6. **判定**:串口里每个用例都要 `PASS`;不一致的用例先按 §4.3 的顺序找自检/对照点;
7. **回归**:`west twister -T modules/hal_ag32_samples/samples -p agrv2k_407 --build-only`(全绿);
8. **文档**:sample 的 `README.md` 写清硬件前置(比特流+跳线)、抓取命令与实测输出;
   需要的话在 `the development notes (not published here)` 补一节(标清实测/推断);
9. **提交**:英文祈使句标题 ≤72 字符 + `Signed-off-by`,push 后同步 `modules/hal_ag32_samples`;
10. **收尾**:按 §4.5 把开发板恢复到 hello_world + 默认比特流。
