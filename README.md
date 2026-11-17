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
- [ ] Bootloader：元数据读写 + CRC 校验 + 跳转
- [ ] App：升级模块（写另一分区 + 切标记）
- [ ] 试启动 + 三次失败回滚
- [ ] 升级包传输（串口 / ESP8266）

---

## 当前进度

| 阶段 | 状态 |
|---|---|
| 目录搭建 | ✅ 2026-11-04 |
| 阶段一 基础（CubeMX、时钟、串口） | ✅ App 点灯，Bootloader 串口输出正常 |
| 阶段二 外设 | 🔄 OLED、4×4 键盘、AS608 指纹已完成；ESP8266、W25Q128 日志未开始 |
| 阶段三 RTOS | 🔄 FreeRTOS 已移植，指纹 / 键盘 / 锁体 / 显示四个任务在跑；队列和互斥量待加 |
| 阶段四 OTA | 🔄 Bootloader 的跳转、CRC 校验、元数据读写已完成；App 侧升级流程未开始 |

---

## 功能分支

每个功能一步一个分支，切过去能编译运行。

| 分支 | 内容 |
|---|---|
| `feat/11-oled` | SSD1306 OLED 显示任务（I2C1，影子帧缓冲，分页刷新） |
| `feat/12-keypad` | 4×4 矩阵键盘（行扫描 + 30ms 消抖，只报按下沿） |
| `feat/13-lock` | 继电器电磁锁驱动（PC0 高电平触发，开锁 3 秒自动关） |
| `feat/14-fingerprint` | AS608 指纹（UART4 57600，1:N 搜索，匹配成功联动开锁） |

---

## 测试记录

每个测试都在独立分支上，切过去能编译运行，复现当时的串口输出。`main` 上只留功能代码，不含测试代码。

| 分支 | 测什么 | 结论 |
|---|---|---|
| `test/crc-vector` | 用标准向量 `123456789` 算 CRC32 | 输出 `0xCBF43926`，与 CRC-32/ISO-HDLC 标准值一致 |
| `test/meta-magic` | 把 magic 改成 `0x12345678` 再写回 Flash | 上电打印 `meta invalid`，第一道检查生效 |
| `test/meta-crc` | magic 正确，但改 `boot_count` 不重算 CRC | 上电打印 `meta invalid`，第二道检查生效 |
| `test/flash-selftest` | 擦写 8KB 到 B 分区并逐字比对 | 2048 个字 0 错误，A 分区 CRC 前后一致 |
| `test/w25q-id` | 通过 SPI2 读 W25Q128 的 JEDEC ID | 返回 `0xEF4018`（Winbond / SPI Flash / 16MB），链路正常 |
| `test/w25q-rw` | 擦一个 4KB 扇区、写 256 字节、读回逐字节比对 | `bad 0`，擦写读链路正常 |
| `test/ota-prepare` | 把 A 区固件 16KB 复制到 W25Q128 暂存区 | `bad 0`，升级包准备就绪 |

---

## 设计说明

分区表、升级流程和每一步的取舍见 `Docs/分区表.md`。

每一步实际遇到的问题、拿到的证据和最终改法见 `Docs/调试记录.md`。
