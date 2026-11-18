> 作品集：[tidegale.github.io](https://tidegale.github.io/) — 两个项目的实现、实测数据和没做完的部分都在这一页

> 开发日志：[Docs/开发日志.md](Docs/开发日志.md) — 按天记的推进和卡点


# 智能门锁 · OTA 版（从零重建）

> 从零重建的智能门锁项目，**A/B 双分区 OTA 是核心能力**，不是附加功能。
> 硬件：嘉立创 STM32F103 项目开发板（STM32F103RCT6，256KB Flash / 48KB RAM）
> 目标：把「能升级」和「升级失败不变砖」这两件事做扎实，重点在可靠性设计

---

## 构建环境

版本从工程文件里读的，不是凭记忆写的（`App.uvprojx`、`App.ioc`、`App/FreeRTOS/Source/include/FreeRTOS.h`）。

| 项 | 内容 |
|---|---|
| MCU | STM32F103RCT6，Cortex-M3，256KB Flash / 48KB RAM |
| IDE | Keil MDK，编译器 ARMCC V5.06 update 6 (build 750)，未启用 ARMCLANG（AC6） |
| 器件支持包 | `Keil.STM32F1xx_DFP` 2.2.0，CMSIS 4.5.0 |
| 工程生成 | STM32CubeMX 6.16.0（MxDb DB.6.0.160） |
| 固件包 | STM32Cube FW_F1 V1.8.7（HAL 驱动来自这里） |
| RTOS | FreeRTOS Kernel V10.3.1，端口 `portable/RVDS/ARM_CM3`，内存管理 `heap_4.c` |
| 外部存储 | W25Q128（SPI2），JEDEC ID `0xEF4018` |
| 烧录 | ST-Link。Bootloader 烧 `0x08000000`，App 烧 `0x08004000` |

两个工程的编译器、器件包、CubeMX 版本一致，只有链接地址和源文件列表不同。

**烧录顺序**：先烧 App，再烧 Bootloader。反过来会因为 A 区 CRC 和元数据不一致被判失败、回退到 B，看起来像"新烧的代码没生效"。原因和排查过程在 [调试记录 第 11 步](Docs/调试记录.md)。

## 目录结构

```
智能门锁_OTA/
├── README.md              本文件
├── Bootloader/            独立工程，烧在 0x08000000
│   ├── Src/
│   │   ├── main.c         上电主流程：读元数据 → 校验 → 跳转
│   │   ├── ota_meta.c     元数据读写（含 magic 和 CRC 自校验）
│   │   └── crc32.c        CRC32 计算（内存 + Flash 两个版本）
│   └── Inc/               对应头文件
├── App/                   主程序工程，烧在 0x08004000
│   ├── Src/
│   │   └── ota.c          升级模块：收固件 → 写另一分区 → 切标记 → 复位
│   └── Inc/
│       └── ota.h          升级接口
└── Docs/
    ├── 分区表.md          Flash 分区详细说明
    └── 调试记录.md        每一步踩过的坑和排查过程
```

---

## Flash 分区（256KB = 0x40000）

| 区间 | 地址范围 | 大小 | 用途 |
|---|---|---|---|
| Bootloader | `0x08000000` - `0x08003FFF` | 16KB | 上电判断、校验、跳转 |
| App-A | `0x08004000` - `0x0801BFFF` | 96KB | 主程序槽 A |
| App-B | `0x0801C000` - `0x08033FFF` | 96KB | 主程序槽 B |
| 元数据 | `0x08034000` - `0x08034FFF` | 4KB | 当前活动槽、试启动标记、CRC |
| 预留 | `0x08035000` - `0x0803FFFF` | 44KB | 参数、日志、后续扩展 |

**外部 W25Q128（16MB SPI Flash）**

| 区间 | 地址范围 | 用途 |
|---|---|---|
| 0x000000 - 0x00FFFF | 64KB | **升级包暂存区**（下载的新固件先放这里） |
| 0x010000 - ... | 剩余 | 开锁日志（已有功能） |

---

## 模块清单

### 阶段一：基础（先跑通再说）
- [ ] CubeMX 建工程，点灯
- [ ] 串口 printf 重定向
- [ ] 系统时钟、滴答定时器

### 阶段二：外设
- [ ] OLED（SSD1306，硬件 I2C）
- [ ] 4×4 矩阵键盘（行扫描 + 消抖状态机）
- [ ] AS608 指纹（UART + 协议帧解析）
- [ ] ESP8266（MQTT）
- [ ] W25Q128（SPI + 扇区管理 + 日志）

### 阶段三：RTOS
- [ ] FreeRTOS 移植
- [ ] 任务划分：指纹 / 键盘 / 网络 / 显示 / 存储
- [ ] 队列、信号量、互斥量
- [ ] 三次失败锁定状态机

### 阶段四：OTA（核心）
- [x] Bootloader：元数据读写 + CRC 校验 + 跳转
- [x] App：升级模块（读暂存区 → 写另一分区 → 校验 → 切标记）
- [x] 试启动 + 三次失败回滚（坏包实验实测）
- [x] 升级包传输：串口收包 → W25Q（ESP8266 下载留给 2.0）

---

## 怎么升级（当前阶段）

两个 Keil 工程共用同一份源码，只是链接地址不同：

| 工程 | IROM1 | 产物 | 烧到 |
|---|---|---|---|
| `App/MDK-ARM/App.uvprojx` | 0x08004000 | `AppA.bin` | A 槽 |
| `App/MDK-ARM/AppB.uvprojx` | 0x0801C000 | `AppB.bin` | B 槽 |

```bash
# 编译 + 导出 bin（fromelf 在 Keil 的 ARMCC 目录下）
UV4.exe -b App/MDK-ARM/App.uvprojx  -j0 -o App/MDK-ARM/App.log
UV4.exe -b App/MDK-ARM/AppB.uvprojx -j0 -o App/MDK-ARM/AppB.log
fromelf.exe --bin -o 门锁OTA升级包/AppA.bin App/MDK-ARM/App/App.axf
fromelf.exe --bin -o 门锁OTA升级包/AppB.bin App/MDK-ARM/AppB/AppB.axf

# 发升级包（apply = 发完自动升级并复位）
python Tools/send_fw.py COM4 门锁OTA升级包/AppB.bin apply
```

板子侧的串口命令：`D` 收包、`A` 应用并复位、`P` 查询当前槽、`R`/`B` 强制切槽（调试用）。

---

## 当前进度

| 阶段 | 状态 |
|---|---|
| 目录搭建 | ✅ 2026-11-04 |
| 阶段一 基础（CubeMX、时钟、串口） | ✅ App 点灯，Bootloader 串口输出正常 |
| 阶段二 外设 | ✅ OLED、4×4 键盘、AS608 指纹、ESP8266 远程开锁已完成；W25Q128 日志未开始 |
| 阶段三 RTOS | ✅ 五个任务在跑（显示 / 键盘 / 锁体 / 指纹 / 网络），键盘用队列、锁体用互斥量 |
| 阶段四 OTA | ✅ 分区管理、整槽 CRC、串口收包、A→B→A 双镜像闭环、试启动回滚全部实测通过 |

---

## 步骤与验证标签

功能开发直接在 `main` 上按步骤提交，每完成一步打一个 `test-*` 标签。切到标签能编译运行，复现当时的串口输出。

| 标签 | 验什么 | 结论 |
|---|---|---|
| `test-clock-tick` | `HAL_GetTick()` 是否真按毫秒走 | 1 秒计到 1000 附近，时钟基准正确 |
| `test-freertos-blink` | FreeRTOS 起来后两个任务能否独立点灯 | 两灯不同频闪烁，调度正常 |
| `test-oled` | SSD1306 5 行显示 + I2C 出错恢复 | 长时间运行不冻结，`i2c err` 不再增长 |
| `test-keypad` | 4×4 键盘 16 个键位 | 逐个按下，`[KEY] 字符 r= c=` 行列号全对 |
| `test-lock` | 继电器锁体状态机 | 请求后开 3 秒自动回锁，电平与日志一致 |
| `test-fp-search` | AS608 57600 波特率 1:N 搜索 | 匹配成功开锁，`score<100` 拒绝 |
| `test-pass-lock` | 密码 `1234` + 连错 3 次锁 5 分钟 | 倒计时实测 298s，锁定期正确密码也拒绝 |
| `test-mqtt` | ESP8266 连热点 + 手写 MQTT 3.1.1 | 手机发 `open` 开锁，`lock/state` 回执可见 |
| `test-crc-vector` | 用标准向量 `123456789` 算 CRC32 | 输出 `0xCBF43926`，与 CRC-32/ISO-HDLC 标准值一致 |
| `test-meta-magic` | 把 magic 改成 `0x12345678` 再写回 Flash | 上电打印 `meta invalid`，第一道检查生效 |
| `test-meta-crc` | magic 正确但改 `boot_count` 不重算 CRC | 上电打印 `meta invalid`，第二道检查生效 |
| `test-flash-selftest` | 擦写 8KB 到 B 分区并逐字比对 | 2048 个字 0 错误，A 分区 CRC 前后一致 |
| `test-w25q-id` | 通过 SPI2 读 W25Q128 的 JEDEC ID | 返回 `0xEF4018`，链路正常 |
| `test-w25q-rw` | 擦一个 4KB 扇区、写 256 字节、读回比对 | `bad 0`，擦写读链路正常 |
| `test-ota-dual` | B 槽装按 B 地址链接的镜像 | `[App] main=0x0801F84D VTOR=0x0801C000`，确实从 B 区执行 |
| `test-ota-loop` | 串口发 30400 字节升级包，A→B→A 走两遍 | 两次 `dropped=0`、CRC 与本地文件一致、回程不卡死 |
| `test-rollback` | 坏包（复位向量指到 0xFF 区）连续三次启动失败 | `boot_count` 1→2→3 后打出 `rollback 1 -> 0`，旧槽正常跑起来 |
| `test-ota-prepare` | 把 A 区固件复制到 W25Q128 暂存区 | `bad 0`，升级包准备就绪 |
| `test-ota-corrupt` | 故意改坏升级包的 CRC | Bootloader 拒绝启动坏槽并切回 |

```bash
git tag                      # 看全部标签
git checkout test-mqtt       # 切到某一步的验证状态
```

---

## 设计说明

分区表、升级流程和每一步的取舍见 `Docs/分区表.md`。

每一步实际遇到的问题、拿到的证据和最终改法见 `Docs/调试记录.md`。
