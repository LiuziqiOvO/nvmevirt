
 如何测试FDPvirt是否work?

# 安装测试工具

> Ubuntu22.04，需要手动安装nvme-cli验证:
>
> ubuntu22.04默认的nvme-cli 1.16不支持FDP，手动编译安装最新的2.14，
> nvme-cli又依赖于libnvme，默认的版本是1.3，至少要1.6


### 1. 安装libnvme v1.10
```bash
#手动安装libnvme
git clone https://github.com/linux-nvme/libnvme.git
cd libnvme
git checkout v1.10  # 
meson setup build
ninja -C build
sudo ninja -C build install

# 更新动态链接库
sudo ldconfig
# 验证版本:是否1.3->1.10
pkg-config --modversion libnvme
```

### 2. 卸载掉旧的nvme-cli,手动安装v2.10(匹配libnvme v1.10)

```bash
rm -rf build
meson setup build
ninja -C build
sudo ninja -C build install

# 验证：
nvme --version                           
nvme version 2.10 (git 2.10)
libnvme version 1.10 (git 1.10)

```
>遇到了一个问题: 明明版本已经过来了,但是还没有fdp命令, 需要额外安装一个依赖,才能正常编译nvme-cli的FDP插件


### 3. 详解:

#### libnvme
libnvme 是用于 Linux 上 NVMe 开发的 C 语言库，它为 NVMe 规范中的结构、枚举提供了类型定义，并提供了用于管理 NVMe 设备的辅助函数 。   

nvme-cli 是该库的主要使用者 。   
libnvme 不仅仅是一个依赖项，它更是 NVMe 规范在软件层面的权威实现。它包含了 FDP 日志页（如 nvme_fdp_config_desc ）、FDP 事件（如    

nvme_fdp_event ）的 C 结构体定义，以及用于构建原始命令操作码的逻辑。如果    

nvme-cli 链接了一个在这些 FDP 结构标准化之前发布的 libnvme 版本，那么 FDP 插件将因缺少必要的符号而编译失败，从而导致它被静默地从最终的二进制文件中排除。用户的系统中安装了 libnvme 1.10 版本，而他尝试编译 nvme-cli 2.10 版本，这很可能就是问题的根源。

#### 关键 I/O 路径：块层与 I/O Passthru
在近期的 Linux 内核中，FDP 支持主要通过“I/O Passthru”路径实现，该路径在通用的 NVMe 字符设备（如 /dev/ng* 或 /dev/nvme*）上使用 io_uring_cmd 接口 。这种方式绕过了传统的块层设备（如    

/dev/nvme0n1）。FDP 命令的 man-page 也明确指出，设备参数必须是一个字符设备 。   

这是一个根本性的要点。为稳定性和抽象性而设计的 Linux 块层，其发展速度通常慢于 NVMe 规范的演进。为了加速采纳像 FDP 这样的新特性，开发者们创建了一条更直接、抽象程度更低的路径。这条路径允许像 nvme-cli 这样的用户空间应用程序将原始的 NVMe 命令直接发送给驱动程序。


# 下发命令

```
nvme fdp                           
nvme-2.10
usage: nvme fdp <command> [<device>] [<args>]

The '<device>' may be either an NVMe character device (ex: /dev/nvme0), an
nvme block device (ex: /dev/nvme0n1), or a mctp address in the form
mctp:<net>,<eid>[:ctrl-id]

Manage Flexible Data Placement enabled devices

The following are all implemented sub-commands:
  configs         List configurations
  usage           Show reclaim unit handle usage
  stats           Show statistics
  events          List events affecting reclaim units and media usage
  status          Show reclaim unit handle status
  update          Update a reclaim unit handle
  set-events      Enabled or disable events
  version         Shows the program version
  help            Display this help

```


检查 Identify Controller：
```bash
sudo nvme id-ctrl /dev/nvme8 -H
```
用户应该寻找与 FDP 相关的能力。虽然具体字段未在资料中提及，但检查 id-ctrl 的原则是确立的 。   

检查 FDP 特性：
```bash
sudo nvme get-feature /dev/nvme8 -f 0x1d -H
```

此命令直接查询 FDP 特性。一个正常工作的模拟器应该返回一个有效的结构，而不是错误 。   

检查 FDP 日志页：

```bash
sudo nvme get-log /dev/nvme8 --log-id=0x1c --log-len=512
```
这会尝试检索 FDP 配置日志页。成功则确认模拟器已实现了 FDP 规范的这一关键部分 。   


首先要能使用nvme-cli识别FDP SSD盘
使用 xNVMe 或 fio（3.36+）验证 FDP 功能。xnvme.io



# 识别FDP SSD
NVMe 驱动：Linux 内核的 NVMe 驱动（drivers/nvme/host/）需要支持 FDP 特性。您需要确保模拟器正确设置 NVMe 控制器寄存器（如 OACS 中的 FDP 位，功能 ID 0x85）。

表 1：核心 FDP 命令与日志页
为了提供一个权威的参考，下表将用户可见的 nvme-cli 命令与其底层的 NVMe 规范动作关联起来。

nvme-cli 命令	NVMe 规范动作	特性/日志 ID (十六进制)	简要描述
nvme fdp-feature	Set/Get Feature	$0x1D$	
为一个耐用性组（Endurance Group）启用、禁用或查询 FDP 模式 。   

nvme fdp-configs	Get Log Page	$0x1C$	
检索一个耐用性组可用的 FDP 配置 。   

nvme fdp-stats	Get Log Page	$0x1E$	
检索生命周期内的 FDP 统计数据（例如，主机/介质写入字节数） 。   

nvme fdp-events	Get Log Page	$0x1F$	
检索 FDP 相关事件的日志，如介质重分配 。   

nvme fdp-status	I/O Management Receive	不适用	
检索一个命名空间（Namespace）的回收单元句柄（RUH）的状态 。   

nvme fdp-update	I/O Management Send	不适用	
更新一个回收单元句柄（RUH） 。   

nvme fdp-usage	Get Log Page	$0x1D$	
检索回收单元句柄（RUH）的使用统计信息 



# test-Local(My server)
```bash
# 使用 8GB 作为起始地址，分配 1GB 内存，使用 CPU 核心 0
sudo insmod nvmev.ko memmap_start=0x200000000 memmap_size=0x40000000 cpu=0
```

# debug 
项目中打印出的调试信息都在dmesg里：
dmesg | tail -20
dmesg | grep -i error | tail -10
