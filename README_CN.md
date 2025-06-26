# NVMeVirt 架构


```mermaid
graph TD
    A["NVMeVirt Device"] -->|Manages| B["Namespaces"]
    A -->|Coordinates| C["I/O Processing"]
    B -->|Associated with| D["FTL Instances"]
    C -->|Dispatches to| B
    D -->|Handles| E["Storage Operations"]
    subgraph "FTL Types"
        D1["Simple FTL"]
        D2["Conventional FTL"]
        D3["ZNS FTL"]
        D4["KV FTL"]
        D --> D1
        D --> D2
        D --> D3
        D --> D4
    end
    D2 -->|Supports| F["FDP Features"]
    F -->|Manages| G["Reclaim Units"]
    F -->|Uses| H["Write Pointer Array"]
```


# Quick Start

## 配置

```bash
sudo insmod ./nvmev.ko \
  memmap_start=128G \       # e.g., 1M, 4G, 8T
  memmap_size=64G   \       # e.g., 1M, 4G, 8T
  cpus=7,8                  # List of CPU cores to process I/O requests (should have at least 2)
```

## 启动

```bash

```