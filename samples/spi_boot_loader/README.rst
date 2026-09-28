.. _spi_boot_loader:

spi_boot_loader -- 片内二段启动 loader:A/B + DFU
################################################

.. contents::
   :local:

这是什么(以及不是什么)
**********************

AgRV2K 只有两种芯片启动档:片内 flash(BOOT0=0)和 Boot ROM(BOOT0=1,串口下载)。
**没有"从外部 flash 启动"这一档** —— 详见 ``开发记录（未随本仓库发布）`` 的厂商依据。所以本样例做的是**二段启动**:

* 片内 flash 放这个 loader(默认 200 MHz 比特流下 XIP 运行),**默认也放镜像仓库** ——
  记录、两个 A/B 边、应用槽、比特流 staging 全在片内(布局见下),板上有没有那颗 2 MiB
  SPI NOR 都行;
* loader 按"启动记录"决定:把选中的那份拷进应用槽再跳过去,或者停在自己的
  控制台;新镜像先以 TRIAL 启动,应用确认后才转正,起不来就自动回退。

它和 `samples/spi_boot_app` 配对:那个样例按槽位地址链接(XIP),就是这里被拷贝、
被跳转的负载。**只有这一种形态** —— 早期的 ``mode external``(把镜像拷进 SRAM 再跑)
已在 2026-09-24 删除,理由是没有用途、且让负载必须维护两套链接地址。

存储布局
********

**默认:全在片内 flash,不碰外部 NOR**(``boards/agrv2k_407.overlay``)。节点只给两个
尺寸 —— ``loader-size``(默认 96 KiB)和 ``app-size``(**默认 200 KiB**)—— 偏移由驱动
推出来,所以"应用能多大"就是一个数:

======================  ==========  ====================================================
片内偏移                大小        内容
======================  ==========  ====================================================
``0x000000``            96 KiB      loader 自己(按同尺寸的分区链接,编超了构建期报错)
``0x018000``            8 KiB       启动记录(两个 4 KiB 扇区,append log,§3.26.24)
``0x01a000``            200 KiB     store A(上传目标)
``0x04c000``            200 KiB     store B
``0x07c000``            200 KiB     应用槽(执行区,跑在 ``0x8007c000``)
``0x0b0000``            100 KiB     比特流 update slot 1(**SoC 固定**;签名档镜像在 +32 B 头之后)
``0x0c9000``            4 KiB       bind-salt(每芯片绑定的盐,``CONFIG_BOOT_AGM_BIND``,§3.26.39)
``0x0ca000``            12 KiB      空闲(两槽之间 16 KiB 间隙的剩余部分)
``0x0cd000``            100 KiB     比特流 update slot 2(**SoC 固定**)
``0x0e6000``            4 KiB       比特流 A/B record(**SoC 固定**,``fcb.c`` 每个镜像都读)
``0x0e7000``            100 KiB     fabric 比特流预留(**SoC 固定**,option byte 指这里)
======================  ==========  ====================================================

整片 1 MiB 到此**分配满**,唯一空着的是 salt 间隙里那 12 KiB;``app-size`` 调大后如果
撞上下一个区域,是**构建期的 ``BUILD_ASSERT``** 告诉你。签名档(65308 B)和试启动看门狗
(604 B)现在都装得下 —— 旧的片内布局(loader 只有 64 KiB、store 只有 60 KiB)两个都装不下。
(这张表在 2026-09-24 校正过:原来写"比特流 staging 104 KiB / 还剩 116 KiB",是在 SoC 固定
两个比特流槽和 record 之前写的;完整地图见 ``docs/FLASH-LAYOUT.md``。)

**两 flash 变体**(``boards/agrv2k_407_ext_nor.overlay``):启动记录和两个 store 放板上
SPI NOR,应用槽与 fabric 那一片仍在片内 —— 注意**比特流 staging 不会跟着搬到 NOR**:
两个比特流 update 槽、比特流 record 和 factory 区都是 SoC 固定在片内的(``fcb.c`` 要在任何
驱动跑起来之前找到那份 record,每个镜像都跑这段)。这一族在节点里**逐项写偏移**
(两块 flash 的地址空间互相独立)。用 ``-DEXTRA_DTC_OVERLAY_FILE=...`` 选它,构建命令见
下面"构建与运行"。

启动策略(优先级从高到低)
************************

1. **RTC 备份域里的一次性覆盖**(``BKP_DR0/1``):跨复位有效、**只能生效一次**,
   用完即清。这是厂商 DFU 里 ``mscratch`` 技巧的等价物 —— 运行中的应用可以在复位
   前"预约下一次从哪启动";
2. **记录里的持久模式**:``auto``(A/B 策略 + 控制台窗口)或 ``internal``(同一策略,
   但**不等**控制台窗口 —— 上传前切换用);
3. 两种模式跑的都是 A/B 策略:选中的那份被拷进片内应用槽 ``0x8007c000`` 并 XIP 执行;
   没有可用镜像就停在控制台。

控制台命令(UART0,115200 8N1,一行一条)
***************************************

* ``help`` —— 命令列表
* ``info`` —— 一次性覆盖、启动记录、payload 状态
* ``mode <auto|internal>`` —— 写持久模式(会擦写启动记录扇区)
* ``once <none|internal>`` —— 只作用于下一次启动(备份域,掉电即失)
* ``upload slot`` —— 把镜像**直接写进片内应用槽** ``0x8007c000``(内嵌 flash DFU;
  等同 ``tools/agm_upload.py <port> slot app.bin``,或 mcumgr 的 ``--slot 2``)
* ``install-slot`` —— 把 store 里的负载**擦写进片内槽位** ``0x8007c000``
  (负载按槽位地址链接,见 ``spi_boot_app``);
  写完会逐字节回读比对
* ``erase`` —— 擦掉负载区(让外部镜像失效)
* ``boot`` —— 立刻按策略启动一次

启动前有 **1.5 s 窗口**(``BOOT_ABORT_WINDOW_MS``):这段时间里收到任意字符就取消启动、
停在控制台。它是必需的 —— 记录一旦指向坏镜像,没有这个窗口就再也进不了控制台
(实测踩过:见下面"实测结果"一节)。

从 host 上传:两条标准路径
**************************

除了自带协议的 ``upload <a|b|比特流>``(见 ``tools/agm_upload.py``),loader 还实现了
两条**标准 host 工具**的路径,都写同一对 A/B 存储、都走同一套"发布(TRIAL/active)+
按策略启动"的流程(细节与实测见 ``开发记录（未随本仓库发布）``):

.. code-block:: sh

   # 0) 复位并"抢进"loader 控制台:host 工具一开端口就开始说话,而 loader 只有
   #    1.5 s 的启动窗口,窗口内没有字符它就直接启动记录里的镜像、控制台随之消失
   #    (实测:复位后直接跑 agrv32flash 会 "Failed to init device")。
   tools/loader_session.sh --port /dev/ttyACM0 -- \
       <下面的 host 命令>

   # 1) AN3155(厂商的 agrv32flash)。窗口是"厂商工具眼里的片内 flash",由驱动映射到
   #    真正的承载位置(默认布局下两者本来就是同一片 flash):
   #    0x80000000 = store A(槽位以下的整个区域)+ 记录/loader 的窗口
   #    0x8007E000 = 片内应用槽(内嵌 flash DFU,直写片内 flash)
   #    0x800B0000 = store B(槽位以上的空隙,含比特流 staging)
   #    比特流窗口 = **非活动槽**(0x800B0000 或 0x800CD000,随更新交替;控制台提交时会打印)
   #       —— 更新写非活动槽,**factory 区 0x800E7000 永不被设备改写**;
   #       这两条是硬性设计约束:更新只写非活动槽,factory 区永不被设备改写
   #    控制台是 8N1,工具默认 8e1,所以 -m 8n1 不能少。
   #    (agrv32flash 在 $AGRV_SDK_PATH/packages/tool-agrv_flashloader/bin/)
   agrv32flash -b 115200 -m 8n1 -S 0x8007E000 -w app.bin -g 0x8007E000 /dev/ttyACM0  # 片内槽
   agrv32flash -b 115200 -m 8n1 -S 0x80000000 -w app.bin -g 0x80000000 /dev/ttyACM0  # store A

   # 2) mcumgr / smpmgr(SMP over console,和 Zephyr 自己的 UART 传输同一套帧)。
   #    image 0 = store A,image 1 = store B,image 2 = 片内应用槽;
   #    上传完用 os reset 触发启动。
   smpmgr -p /dev/ttyACM0 image upload --format any --slot 2 app.bin
   smpmgr -p /dev/ttyACM0 os reset
   smpmgr -p /dev/ttyACM0 image state-read          # 三个 image 的 bootable/active/confirmed
   smpmgr -p /dev/ttyACM0 image state-write --confirm   # 把正在运行的镜像转正(CONFIRMED)
   smpmgr -p /dev/ttyACM0 image erase 2             # 删掉片内应用槽的镜像(擦回空白)

   # 2b) 比特流也走 AN3155:staging 区 + GO = 写 BSB1 头 + 烧片内比特流区(重启后生效)。

   # 3) 自带协议的 `upload slot`(tools/agm_upload.py <port> slot app.bin)

**瘦身档**:两个协议服务器各是一个 Kconfig —— ``CONFIG_BOOT_AGM_AN3155`` 与
``CONFIG_BOOT_AGM_SMP``(后者连带 mcumgr 那套)。两个都关掉时上面三条只剩自带协议
(``tools/agm_upload.py``),镜像小 11.2 KB(实测 57588 B → **46388 B**,同一次构建配置):

.. code-block:: sh

   west build -d /tmp/b_trim -b agrv2k_407 --pristine=auto \
       modules/hal_ag32/samples/spi_boot_loader -- \
       -DCONFIG_BOOT_AGM_AN3155=n -DCONFIG_BOOT_AGM_SMP=n

这个组合由 twister 用例 ``sample.spi_boot_loader.trimmed`` 钉着 —— 在 2026-09-19 之前它
根本编不过(控制台无条件调用 AN3155 的入口),所以"关掉就能瘦身"只是纸面说法。

**片内 flash DFU**:上面三条都支持把镜像直接烧进**片内**应用槽 ``0x8007c000``
(不需要外部 NOR,启动时不拷贝)。记录里用 ``src`` 字段区分来源:``on-die`` 走
``on-die image verified`` 后直接跳转,``ext`` 才做 ``store_write_slot()`` 拷贝。
细节与实测见 ``开发记录（未随本仓库发布）``。

**两种布局可选**:默认是**全片内**的 A/B(布局表见上);板上那片 SPI NOR 想要就用两 flash
变体 —— 记录 + A/B store + 比特流 staging 放进 NOR(``store-flash`` 指 ``&ext_flash``),
应用槽仍在片内。这一族在节点里逐项写偏移,因为两块 flash 的地址空间独立;应用槽地址与
默认布局相同,所以同一份 payload 通用:

.. code-block:: sh

   west build -d /tmp/b_nor -b agrv2k_407 --pristine=auto \
       modules/hal_ag32/samples/spi_boot_loader -- \
       -DEXTRA_DTC_OVERLAY_FILE=boards/agrv2k_407_ext_nor.overlay

这一族的 loader 区是 192 KiB(应用槽在 ``0x80030000``),签名与看门狗都放得下。

**签名档(可选)**:``CONFIG_BOOT_AGM_SIGNATURE`` 有三档 —— ``NONE``(默认,只查 CRC,
**不是**安全边界)、``ECDSA_P256``、``RSA2048_PSS``。后两档只收 MCUboot 容器,发布前与每次
启动前各验一次,裸镜像一律拒收(所以换成签名档之后,store 里原先的未签名镜像就起不来了)。
公钥用 ``-DSPI_BOOT_PUBKEY=<file>`` 编进来,**长度与编码按档**走:ECDSA 要 64 B 裸点 X‖Y,
RSA 要 270 B PKCS#1 ``RSAPublicKey`` DER —— 两个长度不一样是硬要求(``AGM_BOOT_PUBKEY_LEN``),
构建时长度不对直接 ``FATAL_ERROR``。host 侧用同一个工具出容器和公钥:

.. code-block:: sh

   # 签名:档位由 key 文件的类型推断,不用额外参数
   python3 modules/hal_ag32/tools/sign_image.py app.bin key.pem \
       -o app.signed.bin --pubkey-out app.pub --slot-base 0x8007c000

   # 装带公钥的 loader(二选一)
   west build -d /tmp/b_ldr -b agrv2k_407 --pristine=auto \
       modules/hal_ag32/samples/spi_boot_loader -- \
       -DCONFIG_BOOT_AGM_SIG_ECDSA_P256=y -DSPI_BOOT_PUBKEY=app.pub
   west build -d /tmp/b_ldr -b agrv2k_407 --pristine=auto \
       modules/hal_ag32/samples/spi_boot_loader -- \
       -DCONFIG_BOOT_AGM_SIG_RSA2048_PSS=y -DSPI_BOOT_PUBKEY=app.pub

   # 上传的是容器(app.signed.bin),不是裸 app.bin
   python3 modules/hal_ag32/tools/agm_upload.py /dev/ttyACM0 a app.signed.bin

``--slot-base`` 必须等于**这份镜像将要运行的槽**(默认布局是 ``0x8007c000``):它写进 header 的
``ih_load_addr``,而 loader 只接受"header 说的地址 == 本布局要跑它的地址"的容器。体积上 RSA
档明显更胖(96 KiB 的 loader 区里:未签名 57588 B、ECDSA 65708 B、RSA 81976 B),但单次验签
快约 1100 倍;两档的真机实录见 ``开发记录（未随本仓库发布）``。

**比特流也签名(可选)**:``CONFIG_BOOT_AGM_BITSTREAM_SIGNED`` 把两个比特流 update 槽也变成
MCUboot 容器:发布前(写记录之前)与**每次启动流 fabric 之前**各验一次,验不过就不流它 ——
板子退回 factory 区那份(不会出现"流一份坏 fabric 把核冻住"的形态)。
容器用同一个工具签:

.. code-block:: sh

   # 比特流容器:工具自己钉死戳/槽大小,并要求输入正好 99944 B
   python3 modules/hal_ag32/tools/sign_image.py --bitstream example_board.bin key.pem \
       -o bs.signed.bin --pubkey-out bs.pub

   # 装带比特流公钥的 loader(ECDSA 应用档下可以省掉:会回落用 SPI_BOOT_PUBKEY)
   west build -d /tmp/b_ldr -b agrv2k_407 --pristine=auto \
       modules/hal_ag32/samples/spi_boot_loader -- \
       -DCONFIG_BOOT_AGM_SIG_ECDSA_P256=y -DCONFIG_BOOT_AGM_BITSTREAM_SIGNED=y \
       -DCONFIG_ISR_STACK_SIZE=4096 \
       -DSPI_BOOT_PUBKEY=bs.pub -DSPI_BOOT_BITSTREAM_PUBKEY=bs.pub

   # 上传的是容器
   python3 modules/hal_ag32/tools/agm_upload.py /dev/ttyACM0 bitstream bs.signed.bin

两条与直觉不同、但都是实测约束的地方:①比特流档**固定是 ECDSA P-256**,不看应用档 ——
验签跑在 ``PRE_KERNEL_1``(``soc.c`` 的 SYS_INIT),那里 Zephyr 的堆还没初始化
(``malloc_prepare()`` 是 POST_KERNEL 的),PSA/RSA 的验签会分配内存、在板上表现为
**一个字都不打**(实测),只有不分配的 tinycrypt P-256 能用;代价是每次启动多 ~110 ms。
②所以 ``CONFIG_ISR_STACK_SIZE`` 必须 ≥ 3072(验证跑在中断栈上),不够会在 ``fcb.c``
里编译期报错而不是留一块开不了机的板。

**试启动与确认**:loader 跳进一个 TRIAL 镜像之前会武装备份域看门狗并把当前记录序号当作
``ticket`` 写进备份寄存器;应用跑过自检后调 ``agm_boot_trial_confirm()``
(``include/zephyr/drivers/misc/boot_agm.h`` 里的 inline)回写并关掉看门狗,下一次启动时槽
才升成 CONFIRMED。应用从不确认(或者卡死)时,看门狗会自己复位板子,三次之后槽判 BAD 并
回退另一侧 —— 不需要有人按复位。``samples/spi_boot_app`` 已经这么调用,可以照着抄。

**生产档(``CONFIG_BOOT_AGM_LOCK_PRODUCTION``,可选)**:这一档把"改内容"的口收成一条 ——
开一次上传会话、publish、``erase`` 各要一条 ECDSA P-256 签名命令,``once`` 直接拒,
``confirm``/``rollback``/``mode``/``install-slot`` 仍然免签(它们只在已发布镜像之间移动)。
推荐的构建形状是**一个预设**(``CONFIG_BOOT_AGM_PRODUCTION_PROFILE``):它一次性拉齐门控、
SMP(唯一能签发命令的路径)、签名比特流(顺带给 RSA 档提供那把 P-256 授权密钥)与
mcumgr/zcbor/net_buf/base64 这条链,默认关掉 AN3155、默认签名档 ECDSA P-256;而且
**构建时必须有公钥** —— 这个预设 + 没有 ``-DSPI_BOOT_PUBKEY=`` 会让 CMake 直接
``FATAL_ERROR``(锁了却没密钥的板子只能物理恢复)。升级流程:

.. code-block:: sh

   west build -d /tmp/b_ldr -b agrv2k_407 --pristine=auto \
       modules/hal_ag32/samples/spi_boot_loader -- \
       -DCONFIG_BOOT_AGM_PRODUCTION_PROFILE=y \
       -DCONFIG_ISR_STACK_SIZE=4096 \
       -DSPI_BOOT_PUBKEY=app.pub

   # 锁定档的唯一带内升级路径:一条命令花掉两次授权(开会话 + publish)
   python3 modules/hal_ag32/tools/smp_cli.py /dev/ttyACM0 upload 0 app.signed.bin \
       --authorize key.pem

(``-DSPI_BOOT_PUBKEY=`` 的相对路径按 ``samples/spi_boot_loader/`` 解析,所以 CI 场景可以直接写
``fixtures/ci_production.pub``;``CONFIG_ISR_STACK_SIZE=4096`` 是签名比特流验签要的,漏了会在
``fcb.c`` 里编译期报错。)

三件必须知道的事:

1. **console 的 ``upload`` 相与 ``agrv32flash`` 在锁定档完不成上传** —— 它们拿不到流中间
   那条 publish 授权(一次 grant 只覆盖一条命令,而这两条路径期间插不进 SMP 帧)。
   ``tools/agm_upload.py`` 读到 loader 的拒绝行时会直接给出上面那条 ``smp_cli.py`` 提示,
   不用等超时。
2. **不要同时关掉 SMP**(``CONFIG_BOOT_AGM_SMP=n``):它是唯一能签发 group 0x40 命令的服务端,
   关掉之后这块板子只能靠 SWD / ``BOOT0`` + ROM bootloader 恢复。锁定档的启动日志会打出
   这一条(以及"没有 P-256 公钥"那一条),所以配错时不用等到操作员上传才发现。
3. **丢了私钥 = 只能物理恢复**:锁定档的设备端没有任何秘密,验签用的是编进去的公钥,所以
   恢复路径就是 SWD / ROM bootloader(见 ``docs/FLASH-AND-CAPTURE.md`` §10)。

存储/DFU/启动策略**不在这个 sample 里**:它们属于模块的 bootloader 驱动
``drivers/misc/boot_agm.c``(布局来自 ``boards/agrv2k_407.overlay`` 的 ``boot`` 节点,绑定
``dts/bindings/misc/agm,agrv2k-boot.yaml``,API ``include/zephyr/drivers/misc/boot_agm.h``);
sample 只是控制台 + 两个"把编进本二进制的内容推进驱动"的 ``install*`` 命令。AN3155 与 mcumgr
两个协议服务器也是驱动的一部分(``boot_agm_an3155.c`` / ``boot_agm_smp.c``)。见
``开发记录（未随本仓库发布）``。

``tools/loader_session.sh``(= 先开串口、再复位、在窗口里持续发 ``\r\n``)就是第 0 步;
它跑完把端口交给你给的那条命令。AN3155 会话结束时工具发的 RESET(``0xA2``)只被 ACK、
不重启(否则它会立刻启动刚发布的镜像,下一次会话就没有控制台了)——启动由 ``-g`` 或
控制台的 ``reboot`` 显式触发。

上传用的负载就是 ``samples/spi_boot_app``(入口 ``0x8007c000``,即默认布局的应用槽)。

构建与运行
**********

.. code-block:: sh

   # 1) 先编负载(按槽位链接,XIP)
   west build -d /tmp/b_boot_app -b agrv2k_407 --pristine=auto \
       modules/hal_ag32/samples/spi_boot_app

   # 2) 再编 loader
   west build -d /tmp/b_boot_load -b agrv2k_407 --pristine=auto \
       modules/hal_ag32/samples/spi_boot_loader

   # 3) 烧 loader(比特流保持 canonical 200 MHz,不要一起写)
   west flash -d /tmp/b_boot_load --skip-bitstream

   # 4) 把负载传进 loader(direct-to-slot,最快的一条)
   tools/agm_upload.py /dev/ttyACM0 slot /tmp/b_boot_app/zephyr/zephyr.bin
   #    → 复位后 loader 把它交给应用槽并 XIP 执行;
   #      也可以走 A/B 侧:... a|b <app.signed.bin>(签名档见 README 的签名一节)

复位后 loader 会按记录自动启动负载;要回到控制台,在 1.5 s 窗口里发任意字符,
或者先 ``mode internal`` / ``once internal``(后者只影响下一次)再复位。

负载怎么跑
**********

只有一种形态:``install-slot``(或 direct-to-slot 上传)先把负载**写进片内槽位**
  ``0x8007c000``(200 KiB,``flash_agm`` 驱动),之后直接跳到槽位 XIP 运行 —— 负载用
  ``samples/spi_boot_app`` 构建(入口 ``0x8007c000``)。

> 历史:2026-09-17 起还有一条 ``mode external``(把 store 里的镜像拷进 SRAM
> ``0x20008000`` 再跳转)。它 2026-09-24 被删除 —— 实际用途只剩"给同一份记录两种
> 链接地址",而 A/B 槽才是这个 loader 的形态。

实测(2026-09-17,当时的槽位还是 ``0x80030000``):``install``(该命令 2026-09-24 删除) →
``install-slot``(22172 B,回读校验通过)→
``mode internal`` + 复位 → 槽位应用 XIP 运行;SWD 回读槽位前 22172 B 与构建产物
**逐字节相同**;比特流区全程未动。

实测结果(2026-09-17,agrv2k_407,canonical 200 MHz 比特流)
*******************************************************

=======================  =================================================
场景                     结果
=======================  =================================================
``install``(已删)       ✅ 写入 + 回读逐字节校验 + 记录更新(22808 B / 22048 B 两版)
``boot`` / ``auto``      ✅ 拷 22048 B 到 ``0x20008000``,CRC 通过,跳转后 RAM 镜像
                         (``mode external``,2026-09-24 删除)并每秒心跳
``mode internal`` + 复位  ✅ 不启动外部镜像,停在控制台
``once external`` + 复位  ✅ 这一次启动外部镜像;再复位回到持久模式 → 控制台(已删)
``erase`` + 复位          ✅ 拷贝后 ``CRC mismatch`` → 回退到控制台(不挂、不冻)
启动窗口内发字符          ✅ ``console input detected -- boot cancelled``
=======================  =================================================

**为什么 ``CONFIG_AGM_SOC_SKIP_CLOCK_INIT`` 是必须的**:负载若再跑一遍 ``soc.c`` 的
FCB 流程,会拆掉 CPU 脚下的 fabric —— 表现是核静默、SWD ``stalled AP operation``、
串口无输出,只能走 BOOT0 + 上电 + UART 重写固件恢复
(``docs/FLASH-AND-CAPTURE.md`` §10)。``spi_boot_app/prj.conf`` 里那一条不能省。

约束与已知限制
**************

* **比特流路由**(两 flash 变体才涉及):板上 NOR 挂在哪一路 SPI 是比特流属性。canonical 200 MHz
  (``<your canonical 比特流>``)把 flash 放在 **SPI0 的
  fabric 补丁**后面 —— 2026-09-17 用 ``samples/spi_flash_id`` 实测:
  ``spi@40012000`` 读到 RDID ``68 40 15``,SPI1 全 0。换比特流要同步改
  ``boards/agrv2k_407_ext_nor.overlay`` 里的控制器。默认(全片内)布局不碰 NOR,
  所以比特流没把 SPI0 接回引擎也不再是启动故障。
* **负载必须按槽位地址链接**(``0x8007c000``,200 KiB),并且**必须**打开
  ``CONFIG_AGM_SOC_SKIP_CLOCK_INIT``:loader 已经配好 fabric 并把 CPU 切到 PLL,
  链式加载的镜像再跑一遍 ``soc.c`` 的 FCB 流程会把 CPU 正在用的 fabric 拆掉
  (2026-09-17 实测:核静默 + SWD ``stalled AP operation``)。见
  ``samples/spi_boot_app/prj.conf`` 与 ``soc/agm/agrv2k/Kconfig.soc``。
* **读盘分块**(两 flash 变体):``spi_agm`` 单次 ``flash_read`` 受 RX bounce buffer
  限制,loader 用 256 B 分块读并增量算 CRC(§3.27 记过这个坑)。
* **上传**:三条路径都在(自带协议 / AN3155 / mcumgr,见上),``install`` 只是"把编进
  loader 的那份推进 store"的调试入口。
* **A/B 与回退**:在,而且是自动的 —— 试启动看门狗 + 应用侧 ``agm_boot_trial_confirm()``
  。仍然没有的是:anti-rollback 计数器、镜像加密、片内只读公钥区
  (``docs/SIGNED-IMAGES-PLAN.md`` §8 把这三条列为单独的计划)。
* 测试完板上状态:按仓库规矩刷回 ``hello_world`` + canonical 比特流(loader 就不在了,
  要用再按上面的步骤重刷)。想清空记录,进控制台执行 ``erase``。

相关文档
********

* ``开发记录（未随本仓库发布）``(为什么没有"外部 flash 启动"档)、
  §3.26.8(厂商 DFU 参考、Zephyr 侧现状、缺口表、Phase 划分)
* ``开发记录（未随本仓库发布）`` 的未来项
