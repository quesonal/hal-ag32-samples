.. _spi_boot_app:

spi_boot_app -- spi_boot_loader 的负载
######################################

这个样例本身只有一段 banner + 每秒一行心跳,它的意义在于**怎么被链接**:

* ``CONFIG_XIP=y`` + ``boards/agrv2k_407.overlay`` 把它链进 loader 的**片内应用槽**
  (``0x8007c000``,默认布局 200 KiB):``zephyr,flash = &slot_internal`` 才是移动链接
  地址的那一句,入口即 ``0x8007c000``(用 ``readelf -l`` 可复核);
* 因此 ``zephyr.bin`` 可以被 ``samples/spi_boot_loader`` 原样写进槽位、原样 XIP 执行;
* ``CONFIG_AGM_SOC_SKIP_CLOCK_INIT=y`` 是必须的:loader 已经把 fabric 配好、把 CPU
  切到 PLL,这个镜像再跑一遍 ``soc.c`` 的 FCB/时钟流程会把 CPU 脚下的 fabric 拆掉
  (2026-09-17 实测:核静默、SWD ``stalled AP operation``,只能走 BOOT0+UART 救回)。

构建、烧录与运行都在 ``samples/spi_boot_loader/README.rst``(先编这个样例,再把它
传给 loader)。

实测数字(2026-09-19,`agrv2k_407`,canonical 200 MHz 比特流)

- 片内槽位变体:`zephyr.bin` **22488 B**,入口 `0x8007c000`(默认布局的应用槽,200 KiB);
- 签名档再加一个 MCUboot header(``CONFIG_ROM_START_OFFSET=0x20``,见 ``sample.yaml``);
- 真机上被 loader 装载并跳转运行(`spi_boot_app: running`,见
  `samples/spi_boot_loader/README.rst`)。

> 历史:2026-09-17 还有一个 RAM 变体(``CONFIG_XIP=n`` + SRAM overlay,链在
> ``0x20008000``,由 loader 的 ``mode external`` 拷进去跑)。它随 ``mode external``
> 在 2026-09-24 一起删除 —— 同一个负载维护两套链接地址没有实际用途。

试启动确认
**********

banner 之后这个镜像会调一次 ``agm_boot_trial_confirm()``
(``include/zephyr/drivers/misc/boot_agm.h`` 里的 inline,只写备份域寄存器,不需要链接
loader)。loader 把它当 TRIAL 启动时,会先武装备份域看门狗并把当前记录序号当作 ticket 写进
备份寄存器:这个调用回写 ticket + 关掉看门狗,于是**下一次启动**槽从 TRIAL 升成 CONFIRMED;
不调用(或者卡死)就会被看门狗复位,三次之后槽判 BAD、自动回退另一侧 —— 不需要有人按复位。
细节与真机实录见 ``samples/spi_boot_loader/README.rst``` 与
开发记录（未随本仓库发布）。

链接地址的两个坑(overlay 注释里也写了):```CONFIG_USE_DT_CODE_PARTITION`` 必须打开,
而且真正移动链接地址的是 chosen 里的 ``zephyr,flash = &slot_internal`` —— 只设
``zephyr,code-partition`` 时实测仍旧链在 ``0x80000000``。
