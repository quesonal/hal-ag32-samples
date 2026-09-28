.. _verify_flow:

verify_flow -- 端到端验证流程(应用侧)
#####################################

这个样例是 ``../../tools/verify_flow.py`` 的**应用侧那一半**:loader 负责验签与策略,
它负责**把验签结果读回来并打印成一份 ledger**,让"整条链成立"这件事在一屏输出里
可核对。完整流程(每芯片绑定 + provisioning、签名应用 + 签名比特流槽 + 防回滚 +
R2b 授权命令 + 六条失败路径)由 host 脚本驱动,见 ``../../tools/verify_flow.py``。

它做的事
********

* 从**片内槽位** XIP 运行(和 ``samples/spi_boot_app`` 同一套链接方式:
  ``CONFIG_XIP=y`` + ``chosen/zephyr,flash = &slot_internal``);
* 用驱动的公开 API ``agm_boot_info_get()`` 读回启动记录 —— 打印的是 **loader 自己的
  视图**(槽状态、镜像长度/CRC、active 槽、防回滚 floor),不是本镜像自带的副本;
* 读**槽基址上的 MCUboot 头**(就是 loader 验过的那个容器),打印它的 magic、版本、
  header/image 长度;头布局与驱动解析的是同一份
  (``drivers/misc/boot_agm_verify.c``);
* 回应试验启动握手 ``agm_boot_trial_confirm()`` —— 下一次启动这个槽就是 CONFIRMED;
* 最后一行固定为 ``verify_flow: VERIFY-FLOW: PASS``(或 ``FAIL``),host 脚本用它断言。

**它只给正向结论**。所有"必须被拒绝"的路径(篡改容器、旧版本、错密钥签的比特流、
未授权发布)都由 host 脚本对着 **loader 的输出**断言 —— 那些判决发生在 loader 里。

构建与运行
**********

``../../tools/verify_flow.py`` 会自己构建这一份(带 ``CONFIG_ROM_START_OFFSET=0x20``,
即 MCUboot 头占掉的那 32 字节),并用 ``../../tools/sign_image.py`` 签成容器。手工构建:

.. code-block:: sh

   west build -d /tmp/b_verify_flow -b agrv2k_407 --pristine=auto \
       modules/hal_ag32/samples/verify_flow -- -DCONFIG_ROM_START_OFFSET=0x20

依赖:loader 必须用 production profile 构建(``CONFIG_BOOT_AGM_PRODUCTION_PROFILE=y``
+ ``-DSPI_BOOT_PUBKEY=<key.pub>```),否则它不会验签,这份 ledger 里的状态也就没有意义。

实测与文档
**********

* 端到端流程的步骤、断言与一次完整的板级实录:
  维护者本地开发记录(未随本仓库发布)(整链)与(每芯片绑定那两步);
* 现状/待办入口:```docs/BOOT-DFU-STATUS.md``;
* 设计取舍(签名档位、格式、密钥、验签时机):``docs/SIGNED-IMAGES-PLAN.md``;
* 生产锁(R2b 授权命令):``docs/SIGNED-IMAGES-PLAN.md`` §11(原 ``R2-PRODUCTION-LOCK-PLAN.md``)。
* 每芯片绑定(盐、``BIND`` 标签、provisioning):``docs/SIGNED-IMAGES-PLAN.md`` §12。
