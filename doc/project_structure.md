# NVMeVirt 项目结构文档

## 项目概述

NVMeVirt 是一个基于内核模块的 NVMe 虚拟化项目，支持模拟多种不同类型的 SSD 设备，包括 NVM、传统 SSD、ZNS、KV SSD 和 FDP SSD。

## 目录结构

```
nvmevirt/
├── doc/                    # 文档目录
├── include/                # 头文件目录
│   ├── pqueue/            # 优先级队列实现
│   ├── *.h                # 各种头文件
├── src/                   # 源代码目录
│   ├── *.c                # 各种源文件
│   └── *.o                # 编译生成的目标文件
├── Kbuild                 # 内核模块构建配置
├── Makefile               # 主构建文件
├── README.md              # 英文说明文档
├── README_CN.md           # 中文说明文档
└── LICENSE                # 许可证文件
```

## 核心组件

### 1. 主模块文件 (src/)

#### 基础组件
- **main.c**: 模块入口点，负责初始化和清理
- **pci.c**: PCI 设备模拟，处理设备注册和中断
- **admin.c**: NVMe 管理命令处理
- **io.c**: I/O 命令处理，读写操作的核心逻辑
- **dma.c**: DMA 传输处理

#### FTL (Flash Translation Layer) 实现
- **simple_ftl.c**: 简单 FTL，用于 NVM SSD
- **conv_ftl.c**: 传统 FTL，用于常规 SSD
- **zns_ftl.c**: ZNS FTL，用于 Zoned Namespace SSD
- **kv_ftl.c**: KV FTL，用于 Key-Value SSD
- **fdp_ftl.c**: FDP FTL，用于 Flexible Data Placement SSD

#### 辅助组件
- **ssd.c**: SSD 设备模拟，包含通道模型
- **channel_model.c**: 通道延迟模型
- **append_only.c**: 追加写入支持
- **bitmap.c**: 位图操作工具

#### ZNS 专用组件
- **zns_read_write.c**: ZNS 读写操作
- **zns_mgmt_send.c**: ZNS 管理发送命令
- **zns_mgmt_recv.c**: ZNS 管理接收命令

### 2. 头文件 (include/)

#### 核心头文件
- **nvmev.h**: 主要数据结构定义
- **nvme.h**: NVMe 协议定义
- **ssd.h**: SSD 设备结构定义
- **pci.h**: PCI 相关定义

#### FTL 头文件
- **simple_ftl.h**: 简单 FTL 接口
- **conv_ftl.h**: 传统 FTL 接口
- **zns_ftl.h**: ZNS FTL 接口
- **kv_ftl.h**: KV FTL 接口
- **fdp_ftl.h**: FDP FTL 接口

#### 专用协议头文件
- **nvme_zns.h**: ZNS 协议定义
- **nvme_kv.h**: KV 协议定义

#### 配置和工具
- **ssd_config.h**: SSD 配置参数
- **channel_model.h**: 通道模型定义
- **dma.h**: DMA 操作定义
- **append_only.h**: 追加写入接口
- **bitmap.h**: 位图操作接口

#### 优先级队列
- **pqueue/**: 优先级队列实现目录

## 构建配置

### Kbuild 配置选项

项目支持多种 SSD 类型，通过修改 `Kbuild` 文件中的配置选项来切换：

```bash
# 选择一种 SSD 类型进行编译
CONFIG_NVMEVIRT_NVM := y      # NVM SSD
CONFIG_NVMEVIRT_SSD := y      # 传统 SSD
CONFIG_NVMEVIRT_ZNS := y      # ZNS SSD
CONFIG_NVMEVIRT_KV := y       # KV SSD
CONFIG_NVMEVIRT_FDP := y      # FDP SSD
```

### 编译依赖

每种 SSD 类型会编译不同的源文件：
- **NVM**: `simple_ftl.o`
- **传统 SSD**: `ssd.o`, `conv_ftl.o`, `pqueue.o`, `channel_model.o`
- **ZNS**: `ssd.o`, `zns_ftl.o`, `zns_read_write.o`, `zns_mgmt_send.o`, `zns_mgmt_recv.o`, `channel_model.o`
- **KV**: `kv_ftl.o`, `append_only.o`, `bitmap.o`
- **FDP**: `ssd.o`, `fdp_ftl.o`, `pqueue.o`, `channel_model.o`

## 架构特点

### 1. 模块化设计
- 每种 SSD 类型有独立的 FTL 实现
- 通过条件编译确保只编译需要的组件
- 清晰的接口分离

### 2. 性能优化
- 支持多 CPU 核心处理
- 通道延迟模型模拟真实 SSD 行为
- DMA 传输优化

### 3. 协议支持
- 完整的 NVMe 协议支持
- ZNS 协议扩展
- KV 协议扩展
- FDP 协议扩展

## 使用场景

1. **存储系统研究**: 用于测试和研究不同的存储架构
2. **性能评估**: 评估不同 SSD 类型的性能特征
3. **协议验证**: 验证 NVMe 协议扩展的正确性
4. **开发测试**: 为存储相关开发提供测试环境
