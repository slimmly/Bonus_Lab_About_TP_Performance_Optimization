/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "ix_index_handle.h"

#include "ix_scan.h"

/**
 * @brief 在当前node中查找第一个>=target的key_idx
 *
 * @return key_idx，范围为[0,num_key)，如果返回的key_idx=num_key，则表示target大于最后一个key
 * @note 返回key index（同时也是rid index），作为slot no
 */
int IxNodeHandle::lower_bound(const char *target) const {
    // 在当前节点的有序key数组中，二分查找第一个 >= target 的key_idx
    // key_idx ∈ [0, num_key]，返回 num_key 表示 target 大于所有key
    int num_key = page_hdr->num_key;
    int low = 0, high = num_key;
    while (low < high) {
        int mid = low + (high - low) / 2;
        char *mid_key = keys + mid * file_hdr->col_tot_len_;
        // 使用多列复合key的比较函数，逐列比较
        int cmp = ix_compare(mid_key, target, file_hdr->col_types_, file_hdr->col_lens_);
        if (cmp < 0) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return low;
}

/**
 * @brief 在当前node中查找第一个>target的key_idx
 *
 * @return key_idx，范围为[1,num_key)，如果返回的key_idx=num_key，则表示target大于等于最后一个key
 * @note 注意此处的范围从1开始
 */
int IxNodeHandle::upper_bound(const char *target) const {
    // 在当前节点的有序key数组中，二分查找第一个 > target 的key_idx
    // key_idx ∈ [0, num_key]，返回 num_key 表示 target 大于等于所有key
    int num_key = page_hdr->num_key;
    int low = 0, high = num_key;
    while (low < high) {
        int mid = low + (high - low) / 2;
        char *mid_key = keys + mid * file_hdr->col_tot_len_;
        int cmp = ix_compare(mid_key, target, file_hdr->col_types_, file_hdr->col_lens_);
        if (cmp <= 0) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return low;
}

/**
 * @brief 用于叶子结点根据key来查找该结点中的键值对
 * 值value作为传出参数，函数返回是否查找成功
 *
 * @param key 目标key
 * @param[out] value 传出参数，目标key对应的Rid
 * @return 目标key是否存在
 */
bool IxNodeHandle::leaf_lookup(const char *key, Rid **value) {
    // 在叶子节点中二分查找目标key，若匹配则通过value返回对应的Rid
    int idx = lower_bound(key);
    int num_key = page_hdr->num_key;
    if (idx < num_key &&
        ix_compare(keys + idx * file_hdr->col_tot_len_, key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        *value = &rids[idx];
        return true;
    }
    return false;
}

/**
 * 用于内部结点（非叶子节点）查找目标key所在的孩子结点（子树）
 * @param key 目标key
 * @return page_id_t 目标key所在的孩子节点（子树）的存储页面编号
 */
page_id_t IxNodeHandle::internal_lookup(const char *key) {
    // 内部节点结构：keys[i] = 孩子i子树中的最小key，rids[i] = 孩子i的page_no
    // 查找满足 keys[i] <= target 的最大i：即 upper_bound(target) - 1
    // 注意：函数的返回值是记录所在页面的page_no，不是Rid
    int idx = upper_bound(key);  // 第一个 > key 的位置，范围[0, num_key]
    if (idx == 0) {
        // target 小于所有 key，走第一个孩子
        return rids[0].page_no;
    }
    // idx-1 是最后一个 <= key 的孩子下标
    return rids[idx - 1].page_no;
}

/**
 * @brief 在指定位置插入n个连续的键值对
 * 将key的前n位插入到原来keys中的pos位置；将rid的前n位插入到原来rids中的pos位置
 *
 * @param pos 要插入键值对的位置
 * @param (key, rid) 连续键值对的起始地址，也就是第一个键值对，可以通过(key, rid)来获取n个键值对
 * @param n 键值对数量
 * @note [0,pos)           [pos,num_key)
 *                            key_slot
 *                            /      \
 *                           /        \
 *       [0,pos)     [pos,pos+n)   [pos+n,num_key+n)
 *                      key           key_slot
 */
void IxNodeHandle::insert_pairs(int pos, const char *key, const Rid *rid, int n) {
    // 在pos位置插入n个连续的(key, rid)键值对
    // 原有[pos, num_key)的数据整体后移n位
    int num_key = page_hdr->num_key;
    int col_tot_len = file_hdr->col_tot_len_;
    // 1. 合法性检查：pos必须在[0, num_key]范围内
    assert(pos >= 0 && pos <= num_key);
    // 2. 将[pos, num_key)区间的keys和rids向后移动n个位置（从后往前copy，避免覆盖）
    for (int i = num_key - 1; i >= pos; i--) {
        memcpy(keys + (i + n) * col_tot_len, keys + i * col_tot_len, col_tot_len);
        rids[i + n] = rids[i];
    }
    // 3. 写入n个新键值对
    for (int i = 0; i < n; i++) {
        memcpy(keys + (pos + i) * col_tot_len, key + i * col_tot_len, col_tot_len);
        rids[pos + i] = rid[i];
    }
    // 4. 更新键值对数量
    page_hdr->num_key = num_key + n;
}

/**
 * @brief 用于在结点中插入单个键值对。
 * 函数返回插入后的键值对数量
 *
 * @param (key, value) 要插入的键值对
 * @return int 键值对数量
 */
int IxNodeHandle::insert(const char *key, const Rid &value) {
    // 在节点中插入单个键值对，key重复则忽略
    int idx = lower_bound(key);
    int num_key = page_hdr->num_key;
    int col_tot_len = file_hdr->col_tot_len_;
    // 检查key是否已存在（重复key不插入）
    if (idx < num_key && ix_compare(keys + idx * col_tot_len, key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        return num_key;
    }
    // 插入到正确位置
    insert_pairs(idx, key, &value, 1);
    return page_hdr->num_key;
}

/**
 * @brief 用于在结点中的指定位置删除单个键值对
 *
 * @param pos 要删除键值对的位置
 */
void IxNodeHandle::erase_pair(int pos) {
    // 删除pos位置的键值对，将[pos+1, num_key)整体前移一位
    int num_key = page_hdr->num_key;
    int col_tot_len = file_hdr->col_tot_len_;
    assert(pos >= 0 && pos < num_key);
    // 将[pos+1, num_key)的keys和rids向前移动一位
    for (int i = pos + 1; i < num_key; i++) {
        memcpy(keys + (i - 1) * col_tot_len, keys + i * col_tot_len, col_tot_len);
        rids[i - 1] = rids[i];
    }
    // 更新键值对数量
    page_hdr->num_key = num_key - 1;
}

/**
 * @brief 用于在结点中删除指定key的键值对。函数返回删除后的键值对数量
 *
 * @param key 要删除的键值对key值
 * @return 完成删除操作后的键值对数量
 */
int IxNodeHandle::remove(const char *key) {
    // 在节点中查找并删除指定key的键值对
    int idx = lower_bound(key);
    int num_key = page_hdr->num_key;
    int col_tot_len = file_hdr->col_tot_len_;
    // 检查key是否存在，若存在则删除
    if (idx < num_key && ix_compare(keys + idx * col_tot_len, key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        erase_pair(idx);
    }
    return page_hdr->num_key;
}

IxIndexHandle::IxIndexHandle(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, int fd)
    : disk_manager_(disk_manager), buffer_pool_manager_(buffer_pool_manager), fd_(fd) {
    // init file_hdr_
    disk_manager_->read_page(fd, IX_FILE_HDR_PAGE, (char *)&file_hdr_, sizeof(file_hdr_));
    char *buf = new char[PAGE_SIZE];
    memset(buf, 0, PAGE_SIZE);
    disk_manager_->read_page(fd, IX_FILE_HDR_PAGE, buf, PAGE_SIZE);
    file_hdr_ = new IxFileHdr();
    file_hdr_->deserialize(buf);

    // disk_manager管理的fd对应的文件中，设置从file_hdr_->num_pages开始分配page_no
    int now_page_no = disk_manager_->get_fd2pageno(fd);
    disk_manager_->set_fd2pageno(fd, now_page_no + 1);
}

/**
 * @brief 用于查找指定键所在的叶子结点
 * @param key 要查找的目标key值
 * @param operation 查找到目标键值对后要进行的操作类型
 * @param transaction 事务参数，如果不需要则默认传入nullptr
 * @return [leaf node] and [root_is_latched] 返回目标叶子结点以及根结点是否加锁
 * @note need to Unlatch and unpin the leaf node outside!
 * 注意：用了FindLeafPage之后一定要unlatch叶结点，否则下次latch该结点会堵塞！
 */
std::pair<IxNodeHandle *, bool> IxIndexHandle::find_leaf_page(const char *key, Operation operation,
                                                              Transaction *transaction, bool find_first) {
    // 从根节点开始，沿内部节点逐层向下，直到找到包含key的叶子节点
    // 内部节点通过 internal_lookup(key) 决定走哪个孩子
    page_id_t page_no = file_hdr_->root_page_;
    if (page_no == IX_NO_PAGE) {
        return std::make_pair(nullptr, false);
    }
    IxNodeHandle *node = nullptr;
    while (true) {
        node = fetch_node(page_no);
        if (node->is_leaf_page()) {
            // 找到叶子节点，返回；调用方负责 unpin + delete handle
            return std::make_pair(node, false);
        }
        // 内部节点：根据key定位下一层孩子
        page_id_t child_page_no = node->internal_lookup(key);
        buffer_pool_manager_->unpin_page(node->get_page_id(), false);
        delete node;
        page_no = child_page_no;
    }
}

/**
 * @brief 用于查找指定键在叶子结点中的对应的值result
 *
 * @param key 查找的目标key值
 * @param result 用于存放结果的容器
 * @param transaction 事务指针
 * @return bool 返回目标键值对是否存在
 */
bool IxIndexHandle::get_value(const char *key, std::vector<Rid> *result, Transaction *transaction) {
    // 通过索引查找key对应的所有Rid（支持非唯一索引）
    auto [leaf, _root_latched] = find_leaf_page(key, Operation::FIND, transaction);
    if (leaf == nullptr) return false;
    Rid *value = nullptr;
    bool found = leaf->leaf_lookup(key, &value);
    if (found && value != nullptr) {
        result->push_back(*value);
    }
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    delete leaf;
    return found;
}

/**
 * @brief  将传入的一个node拆分(Split)成两个结点，在node的右边生成一个新结点new node
 * @param node 需要拆分的结点
 * @return 拆分得到的new_node
 * @note need to unpin the new node outside
 * 注意：本函数执行完毕后，原node和new node都需要在函数外面进行unpin
 */
IxNodeHandle *IxIndexHandle::split(IxNodeHandle *node) {
    // 将node的键值对一分为二：左半保留在node，右半移入新建的右兄弟节点
    IxNodeHandle *new_node = create_node();
    // 初始化新节点的page_hdr
    new_node->page_hdr->is_leaf = node->is_leaf_page();
    new_node->page_hdr->parent = node->get_parent_page_no();
    new_node->page_hdr->num_key = 0;

    int num_key = node->get_size();
    int mid = num_key / 2;  // 左半数量，右半数量 = num_key - mid
    int move_n = num_key - mid;

    // 将node的[mid, num_key)键值对移动到new_node
    new_node->insert_pairs(0, node->get_key(mid), node->get_rid(mid), move_n);
    node->set_size(mid);

    if (node->is_leaf_page()) {
        // 叶子节点：维护prev_leaf / next_leaf双向链表
        new_node->set_prev_leaf(node->get_page_no());
        new_node->set_next_leaf(node->get_next_leaf());
        node->set_next_leaf(new_node->get_page_no());
        // 更新原后继节点的prev_leaf指向new_node
        if (new_node->get_next_leaf() != IX_NO_PAGE && new_node->get_next_leaf() != IX_LEAF_HEADER_PAGE) {
            IxNodeHandle *next = fetch_node(new_node->get_next_leaf());
            next->set_prev_leaf(new_node->get_page_no());
            buffer_pool_manager_->unpin_page(next->get_page_id(), true);
        }
    } else {
        // 内部节点：将移到new_node的所有孩子的父指针更新为new_node
        for (int i = 0; i < move_n; i++) {
            maintain_child(new_node, i);
        }
    }

    return new_node;  // 调用方负责unpin new_node和node
}

/**
 * @brief Insert key & value pair into internal page after split
 * 拆分(Split)后，向上找到old_node的父结点
 * 将new_node的第一个key插入到父结点，其位置在 父结点指向old_node的孩子指针 之后
 * 如果插入后>=maxsize，则必须继续拆分父结点，然后在其父结点的父结点再插入，即需要递归
 * 直到找到的old_node为根结点时，结束递归（此时将会新建一个根R，关键字为key，old_node和new_node为其孩子）
 *
 * @param (old_node, new_node) 原结点为old_node，old_node被分裂之后产生了新的右兄弟结点new_node
 * @param key 要插入parent的key
 * @note 一个结点插入了键值对之后需要分裂，分裂后左半部分的键值对保留在原结点，在参数中称为old_node，
 * 右半部分的键值对分裂为新的右兄弟节点，在参数中称为new_node（参考Split函数来理解old_node和new_node）
 * @note 本函数执行完毕后，new node和old node都需要在函数外面进行unpin
 */
void IxIndexHandle::insert_into_parent(IxNodeHandle *old_node, const char *key, IxNodeHandle *new_node,
                                       Transaction *transaction) {
    // 分裂后将new_node的信息插入父节点，若父节点溢出则递归分裂上溯
    // key = new_node->get_key(0)，即新节点的最小key

    if (old_node->is_root_page()) {
        // old_node原本是根：创建新根，old_node和new_node作为新根的两个孩子
        IxNodeHandle *new_root = create_node();
        new_root->page_hdr->is_leaf = false;
        new_root->page_hdr->parent = IX_NO_PAGE;
        new_root->page_hdr->num_key = 0;
        update_root_page_no(new_root->get_page_no());

        // 将old_node和new_node作为新根的前两个孩子
        Rid rid_old = {.page_no = old_node->get_page_no(), .slot_no = -1};
        Rid rid_new = {.page_no = new_node->get_page_no(), .slot_no = -1};
        new_root->insert_pairs(0, old_node->get_key(0), &rid_old, 1);
        new_root->insert_pairs(1, key, &rid_new, 1);

        // 更新两个孩子的父指针
        old_node->set_parent_page_no(new_root->get_page_no());
        new_node->set_parent_page_no(new_root->get_page_no());

        // 更新last_leaf：如果new_node是叶子且old_node原是last_leaf
        if (new_node->is_leaf_page() && file_hdr_->last_leaf_ == old_node->get_page_no()) {
            file_hdr_->last_leaf_ = new_node->get_page_no();
        }

        buffer_pool_manager_->unpin_page(new_root->get_page_id(), true);
        return;
    }

    // 非根节点：获取父节点，在old_node位置之后插入new_node条目
    IxNodeHandle *parent = fetch_node(old_node->get_parent_page_no());
    int child_idx = parent->find_child(old_node);

    Rid rid_new = {.page_no = new_node->get_page_no(), .slot_no = -1};

    parent->insert_pairs(child_idx, key, &rid_new, 1);

    // 检查父节点是否溢出
    if (parent->get_size() >= parent->get_max_size()) {
        IxNodeHandle *new_parent = split(parent);
        insert_into_parent(parent, new_parent->get_key(0), new_parent, transaction);
        // 递归返回后，parent和new_parent的page已在递归中标记dirty，需由本层unpin
        buffer_pool_manager_->unpin_page(new_parent->get_page_id(), true);
    }

    buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
}

/**
 * @brief 将指定键值对插入到B+树中
 * @param (key, value) 要插入的键值对
 * @param transaction 事务指针
 * @return page_id_t 插入到的叶结点的page_no
 */
page_id_t IxIndexHandle::insert_entry(const char *key, const Rid &value, Transaction *transaction) {
    // 将(key, value)插入B+树：找到叶子 -> 插入 -> 若溢出则分裂并上溯
    if (is_empty()) {
        // 空树：创建根叶子节点
        IxNodeHandle *root = create_node();
        root->page_hdr->is_leaf = true;
        root->page_hdr->parent = IX_NO_PAGE;
        update_root_page_no(root->get_page_no());
        file_hdr_->first_leaf_ = root->get_page_no();
        file_hdr_->last_leaf_ = root->get_page_no();
        root->insert(key, value);
        page_id_t leaf_page = root->get_page_no();
        buffer_pool_manager_->unpin_page(root->get_page_id(), true);
        return leaf_page;
    }

    auto [leaf, _root_latched] = find_leaf_page(key, Operation::INSERT, transaction);
    int old_size = leaf->get_size();
    leaf->insert(key, value);
    int new_size = leaf->get_size();
    page_id_t leaf_page = leaf->get_page_no();

    if (old_size == new_size) {
        // key重复，未实际插入
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
        delete leaf;
        return leaf_page;
    }

    if (new_size >= leaf->get_max_size()) {
        // 叶子节点溢出：分裂
        IxNodeHandle *new_leaf = split(leaf);
        insert_into_parent(leaf, new_leaf->get_key(0), new_leaf, transaction);

        // 更新last_leaf：若new_leaf在leaf右边，则new_leaf是新的last_leaf
        if (file_hdr_->last_leaf_ == leaf->get_page_no()) {
            file_hdr_->last_leaf_ = new_leaf->get_page_no();
        }

        buffer_pool_manager_->unpin_page(new_leaf->get_page_id(), true);
        leaf_page = new_leaf->get_page_no();
    }

    buffer_pool_manager_->unpin_page(leaf->get_page_id(), true);
    delete leaf;
    return leaf_page;
}

/**
 * @brief 用于删除B+树中含有指定key的键值对
 * @param key 要删除的key值
 * @param transaction 事务指针
 */
bool IxIndexHandle::delete_entry(const char *key, Transaction *transaction) {
    // 从B+树中删除key对应的键值对：找叶子 -> 删除 -> 若欠额则调整
    if (is_empty()) return false;

    auto [leaf, _root_latched] = find_leaf_page(key, Operation::DELETE, transaction);
    if (leaf == nullptr) return false;

    int before = leaf->get_size();
    leaf->remove(key);
    int after = leaf->get_size();

    if (before == after) {
        // key不存在
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
        delete leaf;
        return false;
    }

    // 删除成功，检查是否需要合并或重分配
    if (after < leaf->get_min_size()) {
        // coalesce_or_redistribute 内部会处理leaf的清理（包括可能的页面删除）
        // 如果leaf被合并删除，coalesce会delete_page；如果重分配，leaf存活
        // 函数统一负责sibling/parent/leaf的清理，此处不再手动unpin leaf
        bool root_is_latched = false;
        coalesce_or_redistribute(leaf, transaction, &root_is_latched);
        // leaf在coalesce_or_redistribute中已被处理
        return true;
    }

    buffer_pool_manager_->unpin_page(leaf->get_page_id(), true);
    delete leaf;
    return true;
}

/**
 * @brief 用于处理合并和重分配的逻辑，用于删除键值对后调用
 *
 * @param node 执行完删除操作的结点
 * @param transaction 事务指针
 * @param root_is_latched 传出参数：根节点是否上锁，用于并发操作
 * @return 是否需要删除结点
 * @note User needs to first find the sibling of input page.
 * If sibling's size + input page's size >= 2 * page's minsize, then redistribute.
 * Otherwise, merge(Coalesce).
 */
bool IxIndexHandle::coalesce_or_redistribute(IxNodeHandle* node, Transaction* transaction,bool* root_is_latched) {

    // 根节点直接调整
    if (node->is_root_page()) {
        return adjust_root(node);
    }

    // 未发生下溢无需处理
    if (node->get_size() >= node->get_min_size()) {
        return false; // 不需要处理
    }
   
    // 获取父节点和兄弟节点
    IxNodeHandle* parent = fetch_node(node->get_parent_page_no());
    int idx = parent->find_child(node);

    int sib_idx = (idx == 0) ? 1 : idx - 1;
    IxNodeHandle* sibling = fetch_node(parent->value_at(sib_idx));

    bool need_merge = (node->get_size() + sibling->get_size()) < node->get_min_size() * 2;

    // 重分配
    if (!need_merge) {
        redistribute(sibling, node, parent, idx);
        buffer_pool_manager_->unpin_page(sibling->get_page_id(), true);
        delete sibling;

        buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
        delete parent;
        return false;
    }

    // 合并
    bool parent_need;

    if (sib_idx < idx) {
        parent_need = coalesce(&sibling, &node, &parent, sib_idx, transaction, root_is_latched);
    } else {
        parent_need = coalesce(&node, &sibling, &parent, idx, transaction, root_is_latched);
    }

    // 清理内存
    buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
    delete parent;
    delete node;
    delete sibling;

    // 递归向上处理父节点
    if (parent_need) {
        coalesce_or_redistribute(parent, transaction, root_is_latched);
    }

    return parent_need;
}

/**
 * @brief 用于当根结点被删除了一个键值对之后的处理
 * @param old_root_node 原根节点
 * @return bool 根结点是否需要被删除
 * @note size of root page can be less than min size and this method is only called within coalesce_or_redistribute()
 */
bool IxIndexHandle::adjust_root(IxNodeHandle *old_root_node) {
    // 根节点在删除一个键值对后的调整
    if (!old_root_node->is_leaf_page() && old_root_node->get_size() == 1) {
        // 内部根节点只剩1个孩子：将孩子提升为新根
        page_id_t child_page = old_root_node->remove_and_return_only_child();
        update_root_page_no(child_page);
        IxNodeHandle *child = fetch_node(child_page);
        child->set_parent_page_no(IX_NO_PAGE);
        buffer_pool_manager_->unpin_page(child->get_page_id(), true);
        delete child;
        // 删除旧根页面
        buffer_pool_manager_->delete_page(old_root_node->get_page_id());
        release_node_handle(*old_root_node);
        return true;
    }

    if (old_root_node->is_leaf_page() && old_root_node->get_size() == 0) {
        // 叶子根节点变空：整棵树为空
        update_root_page_no(IX_NO_PAGE);
        file_hdr_->first_leaf_ = IX_NO_PAGE;
        file_hdr_->last_leaf_ = IX_NO_PAGE;
        buffer_pool_manager_->delete_page(old_root_node->get_page_id());
        release_node_handle(*old_root_node);
        return true;
    }

    return false;
}

/**
 * @brief 重新分配node和兄弟结点neighbor_node的键值对
 * Redistribute key & value pairs from one page to its sibling page. If index == 0, move sibling page's first key
 * & value pair into end of input "node", otherwise move sibling page's last key & value pair into head of input "node".
 *
 * @param neighbor_node sibling page of input "node"
 * @param node input from method coalesceOrRedistribute()
 * @param parent the parent of "node" and "neighbor_node"
 * @param index node在parent中的rid_idx
 * @note node是之前刚被删除过一个key的结点
 * index=0，则neighbor是node后继结点，表示：node(left)      neighbor(right)
 * index>0，则neighbor是node前驱结点，表示：neighbor(left)  node(right)
 * 注意更新parent结点的相关kv对
 */
void IxIndexHandle::redistribute(IxNodeHandle *neighbor_node, IxNodeHandle *node, IxNodeHandle *parent, int index) {
    // 从neighbor_node借一个键值对给node，使两个节点都 >= min_size
    // index是node在parent中的rid_idx
    if (index == 0) {
        // node在左（index=0），neighbor在右（index=1）：从neighbor移第一个kv到node末尾
        node->insert_pair(node->get_size(), neighbor_node->get_key(0), *neighbor_node->get_rid(0));
        neighbor_node->erase_pair(0);
        // 更新parent中指向neighbor的key
        parent->set_key(1, neighbor_node->get_key(0));
        // 如果node是内部节点，更新被移入孩子的父指针
        if (!node->is_leaf_page()) {
            maintain_child(node, node->get_size() - 1);
        }
    } else {
        // neighbor在左（index-1），node在右（index）：从neighbor移最后一个kv到node开头
        int last = neighbor_node->get_size() - 1;
        node->insert_pair(0, neighbor_node->get_key(last), *neighbor_node->get_rid(last));
        neighbor_node->erase_pair(last);
        // 更新parent中指向node的key（node的第一个key变了）
        parent->set_key(index, node->get_key(0));
        // 如果node是内部节点，更新被移入孩子的父指针
        if (!node->is_leaf_page()) {
            maintain_child(node, 0);
        }
    }
}

/**
 * @brief 合并(Coalesce)函数是将node和其直接前驱进行合并，也就是和它左边的neighbor_node进行合并；
 * 假设node一定在右边。如果上层传入的index=0，说明node在左边，那么交换node和neighbor_node，保证node在右边；合并到左结点，实际上就是删除了右结点；
 * Move all the key & value pairs from one page to its sibling page, and notify buffer pool manager to delete this page.
 * Parent page must be adjusted to take info of deletion into account. Remember to deal with coalesce or redistribute
 * recursively if necessary.
 *
 * @param neighbor_node sibling page of input "node" (neighbor_node是node的前结点)
 * @param node input from method coalesceOrRedistribute() (node结点是需要被删除的)
 * @param parent parent page of input "node"
 * @param index node在parent中的rid_idx
 * @return true means parent node should be deleted, false means no deletion happend
 * @note Assume that *neighbor_node is the left sibling of *node (neighbor -> node)
 */
bool IxIndexHandle::coalesce(IxNodeHandle **neighbor_node, IxNodeHandle **node, IxNodeHandle **parent, int index,
                             Transaction *transaction, bool *root_is_latched) {
    // 合并两个节点：将右节点(*node)的所有kv移入左节点(*neighbor_node)，删除右节点
    // 约定：*neighbor_node=左，*node=右；若index=0则先交换以确保此约定

    if (index == 0) {
        // node在parent的最左边（index=0），即node是左节点、neighbor是右节点
        // 交换使neighbor为左、node为右
        std::swap(*neighbor_node, *node);
        index = 1;  // node在parent中的新位置
    }

    int neighbor_n = (*neighbor_node)->get_size();
    int node_n = (*node)->get_size();

    // 将右节点的全部键值对追加到左节点末尾
    (*neighbor_node)->insert_pairs(neighbor_n, (*node)->get_key(0), (*node)->get_rid(0), node_n);

    // 如果是内部节点，更新所有移入孩子的父指针
    if (!(*node)->is_leaf_page()) {
        for (int i = 0; i < node_n; i++) {
            maintain_child(*neighbor_node, neighbor_n + i);
        }
    }

    // 如果是叶子节点，维护双向链表并更新last_leaf
    if ((*node)->is_leaf_page()) {
        (*neighbor_node)->set_next_leaf((*node)->get_next_leaf());
        if ((*node)->get_next_leaf() != IX_NO_PAGE && (*node)->get_next_leaf() != IX_LEAF_HEADER_PAGE) {
            IxNodeHandle *next = fetch_node((*node)->get_next_leaf());
            next->set_prev_leaf((*neighbor_node)->get_page_no());
            buffer_pool_manager_->unpin_page(next->get_page_id(), true);
        }
        // 如果被删除的是最后一个叶子，更新last_leaf为合并后的节点
        if (file_hdr_->last_leaf_ == (*node)->get_page_no()) {
            file_hdr_->last_leaf_ = (*neighbor_node)->get_page_no();
        }
    }

    // 从parent中删除右节点(*node)对应的条目
    (*parent)->erase_pair(index);
    // 更新parent中左节点(*neighbor_node)对应的key（合并后最小key可能不变，但安全起见刷新）
    (*parent)->set_key(index - 1, (*neighbor_node)->get_key(0));

    // 删除右节点(*node)的页面
    buffer_pool_manager_->delete_page((*node)->get_page_id());
    release_node_handle(*(*node));

    // 返回parent是否需要进一步处理（递归上溯）
    return (*parent)->get_size() < (*parent)->get_min_size();
}

/**
 * @brief 这里把iid转换成了rid，即iid的slot_no作为node的rid_idx(key_idx)
 * node其实就是把slot_no作为键值对数组的下标
 * 换而言之，每个iid对应的索引槽存了一对(key,rid)，指向了(要建立索引的属性首地址,插入/删除记录的位置)
 *
 * @param iid
 * @return Rid
 * @note iid和rid存的不是一个东西，rid是上层传过来的记录位置，iid是索引内部生成的索引槽位置
 */
Rid IxIndexHandle::get_rid(const Iid &iid) const {
    IxNodeHandle *node = fetch_node(iid.page_no);
    if (iid.slot_no >= node->get_size()) {
        throw IndexEntryNotFoundError();
    }
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);  // unpin it!
    return *node->get_rid(iid.slot_no);
}

/**
 * @brief FindLeafPage + lower_bound
 *
 * @param key
 * @return Iid
 * @note 上层传入的key本来是int类型，通过(const char *)&key进行了转换
 * 可用*(int *)key转换回去
 */
/**
 * @brief FindLeafPage + lower_bound
 *
 * @param key 全量索引列拼接后的复合 key（长度为 file_hdr_->col_tot_len_）
 * @return Iid 指向第一个 >= key 的叶子节点中的位置；若不存在则返回 leaf_end()
 */
Iid IxIndexHandle::lower_bound(const char *key) {
    if (is_empty()) return leaf_end();
    auto [leaf, _root_latched] = find_leaf_page(key, Operation::FIND, nullptr);
    int idx = leaf->lower_bound(key);
    Iid iid;
    if (idx >= leaf->get_size()) {
        // 当前叶子所有 key 都小于目标：跳到下一个叶子的首位
        page_id_t next = leaf->get_next_leaf();
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
        delete leaf;
        if (next == IX_NO_PAGE || next == IX_LEAF_HEADER_PAGE) {
            return leaf_end();
        }
        iid = {.page_no = next, .slot_no = 0};
        return iid;
    }
    iid = {.page_no = leaf->get_page_no(), .slot_no = idx};
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    delete leaf;
    return iid;
}

/**
 * @brief FindLeafPage + upper_bound
 *
 * @param key 全量复合 key
 * @return Iid 指向第一个 > key 的位置。
 */
Iid IxIndexHandle::upper_bound(const char *key) {
    if (is_empty()) return leaf_end();
    auto [leaf, _root_latched] = find_leaf_page(key, Operation::FIND, nullptr);
    int idx = leaf->upper_bound(key);
    Iid iid;
    if (idx >= leaf->get_size()) {
        page_id_t next = leaf->get_next_leaf();
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
        delete leaf;
        if (next == IX_NO_PAGE || next == IX_LEAF_HEADER_PAGE) {
            return leaf_end();
        }
        iid = {.page_no = next, .slot_no = 0};
        return iid;
    }
    iid = {.page_no = leaf->get_page_no(), .slot_no = idx};
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    delete leaf;
    return iid;
}

/**
 * @brief 指向最后一个叶子的最后一个结点的后一个
 * 用处在于可以作为IxScan的最后一个
 *
 * @return Iid
 */
Iid IxIndexHandle::leaf_end() const {
    IxNodeHandle *node = fetch_node(file_hdr_->last_leaf_);
    Iid iid = {.page_no = file_hdr_->last_leaf_, .slot_no = node->get_size()};
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);  // unpin it!
    return iid;
}

/**
 * @brief 指向第一个叶子的第一个结点
 * 用处在于可以作为IxScan的第一个
 *
 * @return Iid
 */
Iid IxIndexHandle::leaf_begin() const {
    Iid iid = {.page_no = file_hdr_->first_leaf_, .slot_no = 0};
    return iid;
}

/**
 * @brief 获取一个指定结点
 *
 * @param page_no
 * @return IxNodeHandle*
 * @note pin the page, remember to unpin it outside!
 */
IxNodeHandle *IxIndexHandle::fetch_node(int page_no) const {
    Page *page = buffer_pool_manager_->fetch_page(PageId{fd_, page_no});
    IxNodeHandle *node = new IxNodeHandle(file_hdr_, page);

    return node;
}

/**
 * @brief 创建一个新结点
 *
 * @return IxNodeHandle*
 * @note pin the page, remember to unpin it outside!
 * 注意：对于Index的处理是，删除某个页面后，认为该被删除的页面是free_page
 * 而first_free_page实际上就是最新被删除的页面，初始为IX_NO_PAGE
 * 在最开始插入时，一直是create node，那么first_page_no一直没变，一直是IX_NO_PAGE
 * 与Record的处理不同，Record将未插入满的记录页认为是free_page
 */
IxNodeHandle *IxIndexHandle::create_node() {
    IxNodeHandle *node;
    file_hdr_->num_pages_++;

    PageId new_page_id = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    // 从3开始分配page_no，第一次分配之后，new_page_id.page_no=3，file_hdr_.num_pages=4
    Page *page = buffer_pool_manager_->new_page(&new_page_id);
    node = new IxNodeHandle(file_hdr_, page);
    return node;
}

/**
 * @brief 从node开始更新其父节点的第一个key，一直向上更新直到根节点
 *
 * @param node
 */
void IxIndexHandle::maintain_parent(IxNodeHandle *node) {
    IxNodeHandle *curr = node;
    while (curr->get_parent_page_no() != IX_NO_PAGE) {
        // Load its parent
        IxNodeHandle *parent = fetch_node(curr->get_parent_page_no());
        int rank = parent->find_child(curr);
        char *parent_key = parent->get_key(rank);
        char *child_first_key = curr->get_key(0);
        if (memcmp(parent_key, child_first_key, file_hdr_->col_tot_len_) == 0) {
            assert(buffer_pool_manager_->unpin_page(parent->get_page_id(), true));
            break;
        }
        memcpy(parent_key, child_first_key, file_hdr_->col_tot_len_);  // 修改了parent node
        curr = parent;

        assert(buffer_pool_manager_->unpin_page(parent->get_page_id(), true));
    }
}

/**
 * @brief 要删除leaf之前调用此函数，更新leaf前驱结点的next指针和后继结点的prev指针
 *
 * @param leaf 要删除的leaf
 */
void IxIndexHandle::erase_leaf(IxNodeHandle *leaf) {
    assert(leaf->is_leaf_page());

    IxNodeHandle *prev = fetch_node(leaf->get_prev_leaf());
    prev->set_next_leaf(leaf->get_next_leaf());
    buffer_pool_manager_->unpin_page(prev->get_page_id(), true);

    IxNodeHandle *next = fetch_node(leaf->get_next_leaf());
    next->set_prev_leaf(leaf->get_prev_leaf());  // 注意此处是SetPrevLeaf()
    buffer_pool_manager_->unpin_page(next->get_page_id(), true);
}

/**
 * @brief 删除node时，更新file_hdr_.num_pages
 *
 * @param node
 */
void IxIndexHandle::release_node_handle(IxNodeHandle &node) { file_hdr_->num_pages_--; }

/**
 * @brief 将node的第child_idx个孩子结点的父节点置为node
 */
void IxIndexHandle::maintain_child(IxNodeHandle *node, int child_idx) {
    if (!node->is_leaf_page()) {
        //  Current node is inner node, load its child and set its parent to current node
        int child_page_no = node->value_at(child_idx);
        IxNodeHandle *child = fetch_node(child_page_no);
        child->set_parent_page_no(node->get_page_no());
        buffer_pool_manager_->unpin_page(child->get_page_id(), true);
    }
}
