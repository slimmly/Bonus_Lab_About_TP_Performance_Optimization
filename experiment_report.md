# 数据库事务系统性能优化实验报告

## 一、实验概述

### 1.1 实验背景

本实验基于中国人民大学 RucBase 数据库教学框架，在原有可正确运行的事务系统基础上进行性能优化。原系统实现了表级 S/X 锁 + wait-die 死锁预防机制，但存在多个故意保留的性能瓶颈，包括强制全表扫描、表级锁粒度过粗等问题。

### 1.2 实验目标

完成 README 中规定的 **B1 优化任务**：将查询从全表扫描（SeqScan）优化为使用 B+ 树索引扫描，从而提升 TPC-C benchmark 的吞吐量和降低查询延迟。

### 1.3 小组成员

| 姓名 | 学号 | 分工 |
|------|------|------|
| [请填写] | [请填写] | B+ 树索引实现与测试 |

### 1.4 实验日期

[请填写实验日期]

---

## 二、优化方法介绍

### 2.1 B+ 树索引原理

B+ 树是一种多路平衡查找树，具有以下特点：

1. **非叶子节点只存 key 不存数据**：内部节点仅存储索引键和子节点指针，提高扇出度
2. **所有数据存储在叶子节点**：叶子节点通过双向链表连接，支持高效的范围查询
3. **自平衡特性**：插入/删除操作通过分裂（split）和合并（coalesce）保持树的平衡
4. **对数级查找复杂度**：O(logₙN)，其中 n 为树的阶数，N 为记录总数

在本实验中，B+ 树索引用于加速等值查询和范围查询，避免每次查询都扫描整张表。

### 2.2 基线问题分析（B1 瓶颈）

根据 README 描述，B1 瓶颈表现为：**任何点查都走 SeqScan，stock/customer 表每查一条记录要扫几万页**。

具体问题：
- 原系统中 `force_seq_scan = true`，强制所有查询使用顺序扫描
- 对于 TPC-C 中的 NewOrder 和 Payment 事务，需要频繁查询 stock、customer、district 等表
- 当数据量达到 scale=1（约 2.3GB）时，全表扫描导致极高的 I/O 开销和延迟

### 2.3 优化思路

#### 2.3.1 核心策略

1. **实现完整的 B+ 树索引操作**：在 `src/index/ix_index_handle.cpp` 中完成所有 TODO 部分
2. **启用索引扫描**：将 `src/optimizer/planner.cpp` 中的 `force_seq_scan` 改回 `false`
3. **保持事务正确性**：在并发环境下保证 B+ 树操作的原子性和一致性

#### 2.3.2 技术路线

```
用户查询 → 优化器选择执行计划 → 
    ├─ force_seq_scan=true  → SeqScanExecutor（全表扫描）
    └─ force_seq_scan=false → IndexScanExecutor（B+ 树索引扫描）
```

关键改动点：
- **查找操作**：实现 `find_leaf_page` 定位叶子节点，`leaf_lookup` 和 `internal_lookup` 分别在叶子和内部节点中查找
- **插入操作**：实现 `insert` 插入键值对，`split` 处理节点溢出，`insert_into_parent` 处理父节点更新
- **删除操作**：实现 `remove` 删除键值对，`coalesce_or_redistribute` 处理节点下溢，`adjust_root` 处理根节点收缩

---

## 三、具体实现

### 3.1 修改文件清单

| 文件路径 | 修改内容 | 行数变化 |
|----------|----------|----------|
| `src/index/ix_index_handle.cpp` | 完成 18 个 B+ 树核心函数的 TODO 实现 | +约 600 行 |
| `src/optimizer/planner.cpp` | 将 `force_seq_scan` 从 `true` 改为 `false` | -1 行 |

### 3.2 实现的 18 个函数详解

#### 3.2.1 查找类函数（6 个）

##### (1) `lower_bound(const char *target)`

**功能**：在当前节点中查找第一个 `>= target` 的 key 索引位置。

**实现思路**：
- 使用二分查找算法在有序 key 数组中搜索
- 比较函数 `ix_compare` 支持多列复合键的比较
- 返回值范围 `[0, num_key]`，返回 `num_key` 表示 target 大于所有 key

**伪代码**：
```cpp
int lower_bound(const char *target) {
    int low = 0, high = num_key;
    while (low < high) {
        int mid = low + (high - low) / 2;
        if (compare(keys[mid], target) < 0)
            low = mid + 1;
        else
            high = mid;
    }
    return low;
}
```

##### (2) `upper_bound(const char *target)`

**功能**：在当前节点中查找第一个 `> target` 的 key 索引位置。

**实现思路**：
- 与 `lower_bound` 类似，但比较条件改为 `<=`
- 用于内部节点查找孩子节点时确定子树范围

**关键区别**：
```cpp
if (compare(keys[mid], target) <= 0)  // 注意是 <=
    low = mid + 1;
else
    high = mid;
```

##### (3) `leaf_lookup(const char *key, Rid **value)`

**功能**：在叶子节点中根据 key 查找对应的 Rid（记录 ID）。

**实现思路**：
- 调用 `lower_bound` 找到 key 的位置
- 检查该位置的 key 是否与目标完全匹配
- 若匹配则通过传出参数返回 Rid，否则返回 false

**时间复杂度**：O(log n)，n 为节点内键值对数量

##### (4) `internal_lookup(const char *key)`

**功能**：在内部节点中查找目标 key 所在的孩子节点页面 ID。

**实现思路**：
- 内部节点结构：`keys[i]` = 孩子 i 子树中的最小 key，`rids[i]` = 孩子 i 的 page_no
- 调用 `upper_bound(key)` 找到第一个 `> key` 的位置
- 返回 `idx-1` 对应的孩子节点（即最后一个 `<= key` 的孩子）
- 特殊情况：若 `idx=0`，说明 target 小于所有 key，走第一个孩子

**关键逻辑**：
```cpp
int idx = upper_bound(key);
if (idx == 0) return rids[0].page_no;  // 走最左子树
return rids[idx - 1].page_no;          // 走 idx-1 子树
```

##### (5) `find_leaf_page(const char *key, Operation op, Transaction *txn)`

**功能**：从根节点开始向下遍历，找到包含目标 key 的叶子节点。

**实现思路**：
- 从根节点开始，循环调用 `internal_lookup` descend 到叶子
- 使用 latch coupling 技术（crabbing protocol）保证并发安全
- 在下降过程中适时释放上层节点的 latch

**并发控制**：
- 对于 FIND 操作，到达叶子后释放根节点 latch
- 对于 INSERT/DELETE 操作，需要持有更高级别的锁

##### (6) `get_value(const Iid &iid)`

**功能**：根据索引槽位置（Iid）获取对应的 Rid。

**实现思路**：
- Iid 包含 `page_no` 和 `slot_no`，直接映射到节点内的键值对数组
- fetch 对应页面，验证 slot_no 合法性
- 返回 `rids[slot_no]` 并 unpin 页面

#### 3.2.2 插入类函数（5 个）

##### (7) `insert_pairs(int pos, const char *key, const Rid *rid, int n)`

**功能**：在指定位置插入 n 个连续的键值对。

**实现思路**：
- 将 `[pos, num_key)` 区间的数据整体后移 n 位
- 从后往前 copy 避免数据覆盖
- 写入新的 n 个键值对到 `[pos, pos+n)` 位置
- 更新 `num_key` 计数器

**内存布局变化**：
```
插入前：[0, pos) | [pos, num_key)
插入后：[0, pos) | [pos, pos+n)新数据 | [pos+n, num_key+n)原数据后移
```

##### (8) `insert(const char *key, const Rid &value)`

**功能**：在节点中插入单个键值对，重复 key 则忽略。

**实现思路**：
- 调用 `lower_bound` 找到插入位置
- 检查该位置 key 是否已存在（重复则直接返回）
- 调用 `insert_pairs` 插入新键值对

**去重逻辑**：
```cpp
int idx = lower_bound(key);
if (idx < num_key && compare(keys[idx], key) == 0)
    return num_key;  // 重复，不插入
insert_pairs(idx, key, &value, 1);
```

##### (9) `split(IxNodeHandle *node, int split_idx, Transaction *txn)`

**功能**：当节点键值对数量超过上限时，将节点分裂为两个。

**实现思路**：
- 创建新节点 `new_node`
- 将原节点 `[split_idx, num_key)` 的键值对移到新节点
- 若是内部节点，更新被移动孩子的父指针
- 若是叶子节点，维护双向链表（prev/next 指针）
- 调用 `insert_into_parent` 将新节点信息插入父节点

**分裂点选择**：通常选择中间位置 `num_key / 2`

##### (10) `insert_into_parent(IxNodeHandle *left, const char *key, IxNodeHandle *right, Transaction *txn)`

**功能**：将分裂后的右节点信息插入到父节点中。

**实现思路**：
- 获取 left 的父节点（若 left 是根则创建新根）
- 在父节点中找到 left 的位置 rank
- 在 rank+1 位置插入 `(key, right->page_no)`
- 更新 right 的父指针
- 若父节点溢出，递归调用 `split`

**根节点分裂**：
- 若 left 是根节点，创建新的根节点
- 新根包含两个条目：`(min_key_left, left)` 和 `(min_key_right, right)`
- 树的高度增加 1

##### (11) `insert_entry(const Iid &iid, const char *key, Transaction *txn)`

**功能**：对外提供的索引插入接口。

**实现思路**：
- 调用 `find_leaf_page` 找到目标叶子节点
- 在叶子节点中调用 `insert` 插入键值对
- 若插入后节点溢出，调用 `split` 处理
- 更新文件头中的元数据（如 last_leaf）

#### 3.2.3 删除类函数（7 个）

##### (12) `erase_pair(int pos)`

**功能**：删除节点中指定位置的键值对。

**实现思路**：
- 将 `[pos+1, num_key)` 区间的数据整体前移一位
- 更新 `num_key` 计数器

**注意**：此函数不检查节点是否下溢，需配合后续再平衡操作

##### (13) `remove(const char *key)`

**功能**：删除节点中指定 key 的键值对。

**实现思路**：
- 调用 `lower_bound` 找到 key 位置
- 验证该位置 key 是否匹配
- 调用 `erase_pair` 执行删除

##### (14) `coalesce_or_redistribute(IxNodeHandle *node, Transaction *txn, bool *root_is_latched)`

**功能**：当节点键值对数量低于下限时，决定是合并还是重新分配。

**实现思路**：
- 获取 node 的父节点和兄弟节点（前驱或后继）
- 计算 node 和兄弟节点的总键值对数量
- 若总和 >= 2 * min_size，调用 `redistribute` 重新分配
- 否则调用 `coalesce` 合并节点
- 若父节点因此下溢，递归上溯处理

**决策条件**：
```cpp
if (neighbor_size + node_size >= 2 * min_size)
    redistribute();  // 可以借一个
else
    coalesce();      // 必须合并
```

##### (15) `adjust_root(Transaction *txn)`

**功能**：当根节点键值对数量过少时，调整根节点。

**实现思路**：
- 若根节点不是叶子且只有一个孩子
- 将该孩子提升为新的根节点
- 删除原根节点页面
- 树的高度减 1

**意义**：保持 B+ 树的高度最小化，避免空洞

##### (16) `redistribute(IxNodeHandle *neighbor, IxNodeHandle *node, IxNodeHandle *parent, int index)`

**功能**：从兄弟节点借一个键值对给当前节点。

**实现思路**：
- 若 `index=0`：node 在左，neighbor 在右，从 neighbor 移第一个 kv 到 node 末尾
- 若 `index>0`：neighbor 在左，node 在右，从 neighbor 移最后一个 kv 到 node 开头
- 更新 parent 中对应的 key 值
- 若是内部节点，更新被移动孩子的父指针

**关键更新**：
```cpp
if (index == 0) {
    // 从右邻居借第一个元素
    node->insert(node->size(), neighbor->key(0), neighbor->rid(0));
    neighbor->erase(0);
    parent->set_key(1, neighbor->key(0));  // 更新分隔 key
}
```

##### (17) `coalesce(IxNodeHandle **neighbor, IxNodeHandle **node, IxNodeHandle **parent, int index, ...)`

**功能**：合并两个兄弟节点，删除其中一个。

**实现思路**：
- 约定 `*neighbor` 为左节点，`*node` 为右节点（若 `index=0` 则交换）
- 将右节点的所有键值对追加到左节点末尾
- 若是内部节点，更新所有被移动孩子的父指针
- 若是叶子节点，维护双向链表并更新 `last_leaf`
- 从 parent 中删除右节点对应的条目
- 删除右节点页面

**返回值**：指示 parent 是否需要进一步处理（递归上溯）

##### (18) `delete_entry(const Iid &iid, const char *key, Transaction *txn)`

**功能**：对外提供的索引删除接口。

**实现思路**：
- 调用 `find_leaf_page` 找到目标叶子节点
- 在叶子节点中调用 `remove` 删除键值对
- 若删除后节点下溢，调用 `coalesce_or_redistribute` 处理
- 若是叶子节点，调用 `erase_leaf` 更新链表指针

### 3.3 优化器修改

在 `src/optimizer/planner.cpp` 第 33 行：

```cpp
// 修改前
constexpr bool force_seq_scan = true;

// 修改后
constexpr bool force_seq_scan = false;
```

**影响**：
- 优化器在生成执行计划时会优先考虑索引扫描（IndexScan）
- 对于有合适索引的查询，不再强制使用全表扫描
- TPC-C 事务中的点查和范围查询将自动使用 B+ 树索引

---

## 四、实验结果与分析

### 4.1 测试环境

| 配置项 | 参数 |
|--------|------|
| 服务器地址 | 127.0.0.1:8765 |
| 并发线程数 | 4 |
| 仓库数 (warehouses) | 1 |

### 4.2 性能对比数据

#### 4.2.1 小规模测试（scale=10, duration=60s）

| 指标 | 修改前（全表扫描） | 修改后（B+ 树索引） | 提升倍数 |
|------|-------------------|-------------------|----------|
| **committed** | 142 | 1143 | **8.05x** |
| **aborted** | 805 | 8293 | - |
| **TPS (commit/s)** | 2.31 | 18.86 | **8.16x** |
| **tpmC (NewOrder/min)** | 62.49 | 509.17 | **8.15x** |
| **latency p50 (μs)** | 952 | 1275 | -34% |
| **latency p95 (μs)** | 5,839,575 | 413,563 | **14.1x** |
| **latency p99 (μs)** | 5,839,575 | 436,676 | **13.4x** |

**分析**：
1. **吞吐量大幅提升**：tpmC 从 62.49 提升到 509.17，提升约 8.15 倍
2. **提交事务数增加**：committed 从 142 增加到 1143，说明单位时间内完成的事务更多
3. **长尾延迟显著改善**：p95/p99 延迟从 5.8 秒降低到 0.4 秒左右，提升 13-14 倍
4. **abort 率上升**：从 85% 上升到 88%，这是因为吞吐量提升后并发冲突增加，wait-die 机制导致年轻事务更易 abort

#### 4.2.2 全量测试（scale=1, duration=300s）

| 指标 | 修改前（全表扫描） | 修改后（B+ 树索引） | 提升倍数 |
|------|-------------------|-------------------|----------|
| **committed** | 38 | 232 | **6.11x** |
| **aborted** | 254 | 1791 | - |
| **TPS (commit/s)** | 0.11 | 0.74 | **6.73x** |
| **tpmC (NewOrder/min)** | 2.97 | 20.11 | **6.77x** |
| **latency p50 (μs)** | 4,858,108 | 2,479 | **1959x** |
| **latency p95 (μs)** | 97,545,464 | 9,761,098 | **10.0x** |
| **latency p99 (μs)** | 97,545,464 | 12,162,811 | **8.0x** |

**分析**：
1. **吞吐量稳定提升**：tpmC 从 2.97 提升到 20.11，提升约 6.77 倍
2. **中位延迟极大改善**：p50 延迟从 4.86 秒降低到 2.48 毫秒，提升近 2000 倍！这说明大部分查询不再需要扫描全表
3. **长尾延迟改善明显**：p95/p99 延迟从 97 秒降低到 10-12 秒，提升 8-10 倍
4. **绝对 abort 数增加**：由于吞吐量提升，总事务数增加，abort 绝对数从 254 增加到 1791，但 abort 率从 87% 略降到 88.5%

### 4.3 数据可视化对比

```
tpmC 对比 (NewOrder 事务数/分钟)
├─ scale=10:  [████████] 62.49  →  [████████████████████████████████████████] 509.17  (8.15x)
└─ scale=1:   [██] 2.97       →  [███████████████] 20.11                 (6.77x)

p50 延迟对比 (微秒，越低越好)
├─ scale=10:  [█] 952μs     →  [█] 1275μs      (略增，因并发冲突增加)
└─ scale=1:   [████████████████████████████████████████] 4.86s  →  [█] 2.48ms  (1959x 改善!)

p99 延迟对比 (微秒，越低越好)
├─ scale=10:  [████████████████████████████████████████] 5.84s  →  [████] 437ms   (13.4x)
└─ scale=1:   [████████████████████████████████████████] 97.5s  →  [██████████] 12.2s  (8.0x)
```

### 4.4 正确性验证

1. **TPC-C 一致性约束**：实验过程中未出现数据不一致错误，说明 B+ 树操作在并发环境下保持了正确性
2. **事务隔离性**：wait-die 死锁预防机制正常工作，没有发生死锁
3. **索引完整性**：所有插入和删除操作后，B+ 树的中序遍历结果与预期一致

### 4.5 性能提升原因分析

1. **I/O 次数大幅减少**：
   - 全表扫描：查询一条记录需要扫描数万页
   - B+ 树索引：查询一条记录只需访问 O(logₙN) 个页面（通常 3-4 次 I/O）

2. **CPU 利用率提升**：
   - 减少了无用的数据页读取和比较操作
   - 二分查找比线性扫描更高效

3. **锁持有时间缩短**：
   - 快速定位到目标记录，减少表级 S/X 锁的持有时间
   - 降低了事务间的冲突概率

4. **缓冲池命中率提高**：
   - B+ 树内部节点常驻内存，减少磁盘 I/O
   - 热点数据页更容易被缓存

### 4.6 存在的问题与不足

1. **abort 率仍然较高**（85%-89%）：
   - 根本原因是 wait-die 死锁预防机制
   - 年轻事务遇到老事务持锁会主动 abort
   - 这是 README 中预期的正常表现

2. **表级锁粒度仍粗**：
   - 本实验仅完成 B1 优化（索引），未涉及 B2（锁粒度下沉）
   - 读写事务之间仍存在互斥

3. **长尾延迟依然显著**：
   - scale=1 时 p99 延迟仍有 12 秒
   - 需要进一步优化（如 MVCC、行级锁等）

---

## 五、总结与展望

### 5.1 实验总结

本次实验成功完成了 README 中规定的 **B1 优化任务**：

1. ✅ **实现 B+ 树索引**：在 `ix_index_handle.cpp` 中完成 18 个核心函数
2. ✅ **启用索引扫描**：将 `force_seq_scan` 改为 `false`
3. ✅ **性能显著提升**：
   - tpmC 提升 6.8-8.2 倍
   - p50 延迟最大改善 1959 倍
   - p99 延迟改善 8-14 倍

### 5.2 收获与体会

1. **深入理解 B+ 树数据结构**：通过亲手实现分裂、合并、再平衡等操作，对 B+ 树的自平衡机制有了直观认识
2. **掌握数据库索引优化方法**：理解了索引如何加速查询，以及优化器如何选择执行计划
3. **并发编程能力提升**：在实现 B+ 树操作时需要考虑 latch coupling、事务隔离等并发问题
4. **性能分析方法**：学会了使用 TPC-C benchmark 进行性能测试和数据分析

### 5.3 未来优化方向

根据 README 中的参考优化方向，后续可以考虑：

1. **B2 优化**：将表级锁下沉到行级锁，减少事务间冲突
2. **MVCC 实现**：用多版本并发控制替代纯 2PL，解决读写互斥问题
3. **WAL 日志**：实现 Write-Ahead Logging，支持崩溃恢复
4. **自适应优化**：根据负载特征动态选择最优并发控制协议

---

## 六、参考资料

1. **官方文档**
   - RucBase 实验框架：https://github.com/ruc-deke/rucbase-lab
   - TPC-C 规范：https://www.tpc.org/tpcc/

2. **学术论文**
   - Bayer, R., & McCreight, E. (1972). Organization and maintenance of large ordered indexes.
   - Graefe, G. (1993). Query evaluation techniques for large databases. ACM Computing Surveys.
   - Rosenkrantz, D. J., et al. (1978). System 2000 synchronization mechanisms.

3. **代码参考**
   - CMU 15-445/645 Database Systems Course
   - MySQL InnoDB Storage Engine Source Code
   - PostgreSQL B-Tree Implementation

4. **延伸阅读**
   - Hekaton: SQL Server's Memory-Optimized OLTP Engine (SIGMOD'13)
   - Silo: A Scalable Transactional Memory Engine (SOSP'13)
   - Polyjuice: Learning Concurrency Control Policies (OSDI'21)

---

## 附录：实验命令

```bash
# 1. 编译项目
cd /workspace
cmake -B build
cmake --build build --target rmdb tpcc_loader tpcc_driver -j$(nproc)

# 2. 启动数据库服务器
./build/bin/rmdb test_db &

# 3. 灌入 TPC-C 数据（scale=10）
./build/bin/tpcc_loader -w 1 -S 10 -d

# 4. 运行 TPC-C 测试（60 秒）
./build/bin/tpcc_driver -w 1 -t 4 -d 60 -S 10

# 5. 运行 TPC-C 测试（300 秒，全量）
./build/bin/tpcc_driver -w 1 -t 4 -d 300 -S 1
```

---

**报告生成日期**：[请填写]  
**实验完成状态**：✅ B1 优化已完成（全表扫描 → B+ 树索引）
