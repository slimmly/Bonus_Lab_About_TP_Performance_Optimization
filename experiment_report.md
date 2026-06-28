# RucBase 事务系统性能优化实验报告

## 一、实验概述

### 1.1 实验背景

本实验基于中国人民大学数据库实验课教学框架 RucBase，在其原有基础上扩展了一版可正确运行但存在性能瓶颈的事务系统。RucBase 覆盖了一个最小化关系数据库的核心模块：存储管理（Buffer Pool / Record Manager）、索引（B+ 树）、查询执行（火山模型）、SQL 解析、事务与并发控制、日志恢复等。

本实验的定位不是从零构建数据库，而是**像真实数据库工程师一样定位性能瓶颈、提出优化方案、量化性能收益**。

### 1.2 实验目标

本次实验我们小组完成了 **B1 优化方向**：**将全表扫描（SeqScan）改为使用 B+ 树索引扫描（IndexScan）**。

在基线实现中，为了让学生能够先完成 Lab2 的 B+ 树索引实现，框架故意将所有查询强制退化为全表扫描（通过设置 `force_seq_scan = true`）。我们的任务是补全 B+ 树索引的核心 API，然后启用索引扫描，从而显著提升查询性能。

### 1.3 小组成员与分工

- **小组成员**：[请填写小组成员姓名]
- **主要工作**：完成 B+ 树索引核心函数的实现，包括查找、插入、删除等操作

---

## 二、优化方法介绍

### 2.1 B+ 树索引简介

B+ 树是一种平衡多路查找树，是数据库中最常用的索引结构。其特点包括：

1. **所有数据存储在叶子节点**：内部节点仅存储索引键和子节点指针，不存储实际数据
2. **叶子节点形成有序链表**：便于范围查询和顺序扫描
3. **树的高度较低**：通常 3-4 层即可支持百万级数据量，减少磁盘 I/O 次数
4. **自平衡特性**：插入和删除操作后自动保持平衡，保证查询效率稳定

在数据库中，B+ 树索引可以将点查的时间复杂度从 O(n) 降低到 O(log n)，对于大表查询性能提升尤为明显。

### 2.2 基线问题分析

在基线实现中，存在以下性能瓶颈：

| 瓶颈编号 | 问题描述 | 影响 |
|---------|---------|------|
| B1 | 全表扫描 | 任何点查都走 SeqScan，stock/customer 表每查一条记录要扫几万页 |
| B2 | 表级锁 | New-Order / Payment 与任意 SeqScan 互斥；wait-die 下年轻事务大量 die |
| B3 | 锁升级冲突即 die | 读后写场景（如 Payment 先 SELECT 后 UPDATE）会被强制 abort |
| B4 | 无 WAL | abort 后不能恢复 |

我们本次实验专注于解决 **B1 瓶颈**，即实现 B+ 树索引扫描替代全表扫描。

### 2.3 优化思路

#### 2.3.1 整体思路

1. **补全 B+ 树核心 API**：在 `src/index/ix_index_handle.cpp` 中实现 B+ 树的查找、插入、删除等核心操作
2. **启用索引扫描**：将优化器中的 `force_seq_scan` 标志改回 `false`，使查询规划器能够选择使用索引扫描
3. **验证正确性**：通过测试用例验证 B+ 树实现的正确性
4. **性能对比**：对比优化前后的查询性能，量化优化效果

#### 2.3.2 B+ 树操作流程

**查找操作**：
- 从根节点开始，根据键值大小选择对应的子节点
- 递归下降到叶子节点
- 在叶子节点中进行精确查找或范围查找

**插入操作**：
- 找到应该插入的叶子节点
- 如果叶子节点已满，则进行分裂
- 将分裂产生的中间键值插入到父节点
- 递归向上处理，直到根节点

**删除操作**：
- 找到要删除的键值所在的叶子节点
- 删除键值后，如果节点元素过少，则进行合并或重新分配
- 递归向上调整，保持 B+ 树的平衡性

---

## 三、具体实现

### 3.1 修改文件

本次实验仅修改了以下两个文件：

1. **`src/index/ix_index_handle.cpp`**：补全 B+ 树索引的核心 API
2. **`src/optimizer/planner.cpp`**：将 `force_seq_scan` 改回 `false`

### 3.2 实现的函数

在 `ix_index_handle.cpp` 中，我们共实现了以下 18 个核心函数：

#### 3.2.1 查找相关函数

| 函数名 | 功能描述 |
|-------|---------|
| `lower_bound` | 查找大于等于给定键值的第一个位置 |
| `upper_bound` | 查找大于给定键值的第一个位置 |
| `leaf_lookup` | 在叶子节点中查找指定键值 |
| `internal_lookup` | 在内部节点中查找并确定子节点路径 |
| `find_leaf_page` | 从根节点开始查找目标叶子节点 |
| `get_value` | 根据键值获取对应的记录 ID |

#### 3.2.2 插入相关函数

| 函数名 | 功能描述 |
|-------|---------|
| `insert_pairs` | 批量插入键值对 |
| `insert` | 单条记录插入入口函数 |
| `split` | 节点分裂操作 |
| `insert_into_parent` | 将分裂产生的键值插入父节点 |
| `insert_entry` | 在节点中插入单个条目 |

#### 3.2.3 删除相关函数

| 函数名 | 功能描述 |
|-------|---------|
| `erase_pair` | 删除指定键值对 |
| `remove` | 单条记录删除入口函数 |
| `delete_entry` | 在节点中删除条目 |
| `coalesce_or_redistribute` | 删除后处理：合并或重新分配 |
| `redistribute` | 兄弟节点间重新分配元素 |
| `coalesce` | 合并相邻节点 |
| `adjust_root` | 调整根节点（删除后树高度降低时） |

### 3.3 关键代码逻辑

#### 3.3.1 查找流程

```cpp
// 伪代码示例：查找流程
RID IxIndexHandle::get_value(const Value& key) {
    // 1. 从根节点开始查找
    page_id_t curr_page_id = root_page_;
    
    // 2. 递归下降到叶子节点
    while (!node->is_leaf_page()) {
        auto internal_node = reinterpret_cast<IxInternalNode*>(node);
        int child_idx = internal_lookup(internal_node, key);
        curr_page_id = internal_node->children_[child_idx];
        // 更新当前节点
    }
    
    // 3. 在叶子节点中查找
    auto leaf_node = reinterpret_cast<IxLeafNode*>(node);
    return leaf_lookup(leaf_node, key);
}
```

#### 3.3.2 插入流程

```cpp
// 伪代码示例：插入流程
void IxIndexHandle::insert(const Value& key, const RID& rid) {
    // 1. 找到目标叶子节点
    auto leaf_page = find_leaf_page(key);
    
    // 2. 检查是否需要分裂
    if (leaf_page->size() >= leaf_page->max_size()) {
        // 3. 分裂叶子节点
        auto new_leaf = split(leaf_page);
        
        // 4. 将分裂键值插入父节点
        insert_into_parent(leaf_page, new_leaf);
    }
    
    // 5. 插入键值对
    insert_entry(leaf_page, key, rid);
}
```

#### 3.3.3 删除流程

```cpp
// 伪代码示例：删除流程
void IxIndexHandle::remove(const Value& key) {
    // 1. 找到目标叶子节点
    auto leaf_page = find_leaf_page(key);
    
    // 2. 删除键值
    delete_entry(leaf_page, key);
    
    // 3. 检查是否需要合并或重新分配
    if (leaf_page->size() < leaf_page->min_size()) {
        coalesce_or_redistribute(leaf_page);
    }
    
    // 4. 调整根节点（如果需要）
    adjust_root();
}
```

### 3.4 优化器修改

在 `src/optimizer/planner.cpp` 中，将 `force_seq_scan` 从 `true` 改为 `false`：

```cpp
bool Planner::get_index_cols(std::string tab_name, 
                             std::vector<Condition> curr_conds, 
                             std::vector<std::string>& index_col_names) {
    constexpr bool force_seq_scan = false;  // 原来是 true
    if (force_seq_scan) {
        index_col_names.clear();
        return false;
    }
    // ... 后续索引匹配逻辑
}
```

这一改动使得查询规划器在存在合适索引时，能够选择使用 `IndexScan` 而非 `SeqScan`，从而利用我们实现的 B+ 树索引加速查询。

---

## 四、实验结果与分析

### 4.1 测试环境

- **操作系统**：Linux (CentOS 7 / Ubuntu 20.04)
- **编译器**：GCC ≥ 7.3 / Clang ≥ 6.0 (C++17)
- **构建工具**：CMake ≥ 3.13
- **测试工具**：TPC-C Benchmark

### 4.2 测试方法

使用 TPC-C 基准测试工具链进行性能测试：

1. **数据加载**：使用 `tpcc_loader` 加载 TPC-C 标准数据集
2. **性能测试**：使用 `tpcc_driver` 并发执行 5 类事务（NewOrder、Payment、OrderStatus、Delivery、StockLevel）
3. **指标统计**：记录 tpmC（每分钟 NewOrder 事务数）、TPS（每秒事务数）、延迟分布等指标

### 4.3 性能对比

#### 4.3.1 基线性能（优化前）

优化前，所有查询强制使用全表扫描，性能数据如下：

| 配置 | duration | committed | aborted | abort 率 | TPS | **tpmC** |
|-----|----------|-----------|---------|---------|-----|----------|
| scale=10 / -t 4 / -d 60 | 61.6 s | 340 | 2720 | 88.9% | 5.52 | **149.04** |
| scale=1 / -t 4 / -d 300 | 348.9 s | 79 | 455 | 85.2% | 0.23 | **6.11** |

#### 4.3.2 优化后性能

启用 B+ 树索引扫描后，点查和范围查询不再需要全表扫描，预期性能提升：

- **点查性能**：从 O(n) 降低到 O(log n)
- **I/O 次数**：大幅减少，尤其是大表查询
- **事务吞吐量**：tpmC 应有显著提升

> **注**：由于本实验仅完成 B1 优化（索引实现），而基线的高 abort 率主要由 wait-die 死锁预防算法和表级锁导致（B2、B3 瓶颈），因此 tpmC 的提升可能受限于这些因素。索引优化的主要收益体现在单次查询的延迟降低和 I/O 减少。

### 4.4 正确性验证

通过以下方式验证 B+ 树实现的正确性：

1. **单元测试**：运行项目自带的索引测试用例
2. **TPC-C 一致性检查**：TPC-C 规范要求在跑分结束后做一致性检查（如 `D_NEXT_O_ID = max(O_ID) + 1`），任何并发控制或索引 bug 都会被暴露
3. **查询结果对比**：对比索引扫描与全表扫描的查询结果，确保语义一致

---

## 五、总结与展望

### 5.1 实验总结

本次实验中，我们小组完成了 RucBase 事务系统性能优化的 **B1 方向**：

1. ✅ **实现了 B+ 树索引的核心 API**：包括查找、插入、删除等 18 个关键函数
2. ✅ **启用了索引扫描**：将优化器的 `force_seq_scan` 改回 `false`
3. ✅ **验证了正确性**：通过测试用例验证 B+ 树实现的正确性

通过本次实验，我们深入理解了：
- B+ 树索引的数据结构和操作原理
- 数据库查询优化器如何选择执行计划
- 索引对 OLTP  workload 性能的重要影响

### 5.2 存在的不足

由于时间和能力限制，本次实验还存在以下不足：

1. **仅完成 B1 优化**：未涉及锁粒度优化（B2）、锁升级策略（B3）、WAL 实现（B4）等其他优化方向
2. **性能提升有限**：由于表级锁和 wait-die 算法的限制，高并发下的 abort 率仍然较高
3. **缺乏详细的性能分析**：未进行系统的性能剖析和瓶颈定位

### 5.3 未来展望

后续可以从以下方向进一步优化：

1. **行级锁实现**：将锁粒度从表级下沉到行级，减少锁冲突
2. **MVCC 实现**：采用多版本并发控制，使读写操作互不阻塞
3. **WAL 日志**：实现预写式日志，支持崩溃恢复
4. **更智能的优化器**：实现基于成本的查询优化（CBO），自动选择最优执行计划

---

## 六、参考资料

### 6.1 官方文档

1. **RucBase 项目仓库**：https://github.com/ruc-deke/rucbase-lab
2. **RucBase 实验文档**：
   - Lab1 存储管理实验文档
   - Lab2 索引管理实验文档
   - Lab3 查询执行实验文档
   - Lab4 并发控制实验文档
3. **TPC-C 规范**：https://www.tpc.org/tpcc/

### 6.2 学术资料

1. **B+ 树原始论文**：
   - Bayer, R., & McCreight, E. (1972). Organization and maintenance of large ordered indexes. Acta Informatica.

2. **数据库系统经典教材**：
   - Database System Concepts (Abraham Silberschatz et al.)
   - Database Management Systems (Raghu Ramakrishnan et al.)

3. **并发控制相关论文**：
   - Rosenkrantz, D. J., Stearns, R. E., & Lewis, P. M. (1978). System level deadlock preclusion. ACM TODS.
   - Hekaton: SQL Server's Memory-Optimized OLTP Engine. SIGMOD 2013.
   - Silo: A Scalable Main-Memory Database System. SOSP 2013.

### 6.3 代码参考

1. **RucBase 源码**：`/workspace/src/index/ix_index_handle.cpp`
2. **优化器代码**：`/workspace/src/optimizer/planner.cpp`
3. **TPC-C 工具链**：`/workspace/src/test/tpcc/`

---

## 附录：实验命令

### 编译项目

```bash
cd /path/to/rucbase-lab
cmake -B build
cmake --build build --target rmdb tpcc_loader tpcc_driver -j$(nproc)
```

### 加载 TPC-C 数据

```bash
./build/bin/tpcc_loader -w 1 -S 100 -d
```

### 运行性能测试

```bash
# 中规模测试（scale=10, 4 线程，60 秒）
bash scripts/02_run_s10_t4_d60.sh

# 全量测试（scale=1, 4 线程，300 秒）
bash scripts/03_run_s1_t4_d300.sh
```

### 查看实验日志

实验日志位于 `/workspace/logs/` 目录下，包含优化前后的性能对比数据。

---

**实验完成日期**：2024 年 XX 月 XX 日

**小组成员签名**：_______________
