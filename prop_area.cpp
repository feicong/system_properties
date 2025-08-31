/*
 * Copyright (C) 2008 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "system_properties/prop_area.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/cdefs.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <new>

#include <async_safe/log.h>

constexpr size_t PA_SIZE = 128 * 1024;  // 属性区域大小：128KB
constexpr uint32_t PROP_AREA_MAGIC = 0x504f5250;  // 属性区域魔数
constexpr uint32_t PROP_AREA_VERSION = 0xfc6ed0ab;  // 属性区域版本号

size_t prop_area::pa_size_ = 0;  // 属性区域总大小
size_t prop_area::pa_data_size_ = 0;  // 属性数据区大小

// 创建读写属性区域映射
prop_area* prop_area::map_prop_area_rw(const char* filename, const char* context,
                                       bool* fsetxattr_failed) {
  /* dev是一个tmpfs，我们可以用它来划分一个共享的工作空间，
   * 让我们开始吧...
   */
  const int fd = open(filename, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_EXCL, 0444);

  if (fd < 0) {
    if (errno == EACCES) {
      /* 为了与进程已经映射页面并在尝试写入时发生段错误的情况保持一致 */
      abort();
    }
    return nullptr;
  }

  if (context) {  // 如果指定了SELinux上下文
    if (fsetxattr(fd, XATTR_NAME_SELINUX, context, strlen(context) + 1, 0) != 0) {
      async_safe_format_log(ANDROID_LOG_ERROR, "libc",
                            "fsetxattr failed to set context (%s) for \"%s\"", context, filename);
      /*
       * 由于selinux策略，fsetxattr()在系统属性测试期间会失败。
       * 我们不想为测试器创建自定义策略，所以我们将继续在
       * 此函数中但设置一个标志表示发生了错误。
       * Init是唯一应该调用此函数的守护进程，当发生此错误时会中止。
       * 否则，测试器将忽略它并继续，尽管没有任何selinux
       * 属性分离。
       */
      if (fsetxattr_failed) {
        *fsetxattr_failed = true;
      }
    }
  }

  if (ftruncate(fd, PA_SIZE) < 0) {  // 设置文件大小
    close(fd);
    return nullptr;
  }

  pa_size_ = PA_SIZE;  // 设置属性区域大小
  pa_data_size_ = pa_size_ - sizeof(prop_area);  // 计算数据区大小

  void* const memory_area = mmap(nullptr, pa_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (memory_area == MAP_FAILED) {  // 内存映射失败
    close(fd);
    return nullptr;
  }

  prop_area* pa = new (memory_area) prop_area(PROP_AREA_MAGIC, PROP_AREA_VERSION);

  close(fd);  // 关闭文件描述符
  return pa;  // 返回属性区域指针
}

// 映射文件描述符为只读属性区域
prop_area* prop_area::map_fd_ro(const int fd, bool rw) {
  struct stat fd_stat;
  if (fstat(fd, &fd_stat) < 0) {  // 获取文件状态
    return nullptr;
  }

  // 检查文件权限和大小
  if ((fd_stat.st_uid != 0) || (fd_stat.st_gid != 0) ||
      ((fd_stat.st_mode & (S_IWGRP | S_IWOTH)) != 0) ||
      (fd_stat.st_size < static_cast<off_t>(sizeof(prop_area)))) {
    return nullptr;
  }

  pa_size_ = fd_stat.st_size;  // 设置属性区域大小
  pa_data_size_ = pa_size_ - sizeof(prop_area);  // 计算数据区大小

  int prot = rw ? PROT_READ | PROT_WRITE : PROT_READ;  // 设置内存保护标志
  void* const map_result = mmap(nullptr, pa_size_, prot, MAP_SHARED, fd, 0);
  if (map_result == MAP_FAILED) {  // 内存映射失败
    return nullptr;
  }

  prop_area* pa = reinterpret_cast<prop_area*>(map_result);
  if ((pa->magic() != PROP_AREA_MAGIC) || (pa->version() != PROP_AREA_VERSION)) {
    munmap(pa, pa_size_);  // 验证失败，取消映射
    return nullptr;
  }

  return pa;  // 返回属性区域指针
}

// 映射属性区域文件
prop_area* prop_area::map_prop_area(const char* filename, bool *is_rw) {
  bool rw = false;
  int fd = open(filename, O_CLOEXEC | O_NOFOLLOW | O_RDWR);  // 尝试以读写方式打开
  if (fd == -1) {
    fd = open(filename, O_CLOEXEC | O_NOFOLLOW | O_RDONLY);  // 以只读方式打开
    if (fd == -1) {
      return nullptr;
    }
  } else {
    rw = true;  // 成功以读写方式打开
  }

  prop_area* map_result = map_fd_ro(fd, rw);  // 映射文件描述符
  close(fd);  // 关闭文件描述符

  if (is_rw) *is_rw = rw;  // 返回是否为读写模式
  return map_result;
}

// 分配对象内存
void* prop_area::allocate_obj(const size_t size, uint_least32_t* const off) {
  const size_t aligned = __BIONIC_ALIGN(size, sizeof(uint_least32_t));  // 对齐到32位边界
  if (bytes_used_ + aligned > pa_data_size_) {  // 检查空间是否足够
    return nullptr;
  }

  *off = bytes_used_;  // 返回偏移量
  bytes_used_ += aligned;  // 更新已使用字节数
  return data_ + *off;  // 返回分配的内存地址
}

// 创建新的属性二叉树节点
prop_bt* prop_area::new_prop_bt(const char* name, uint32_t namelen, uint_least32_t* const off) {
  uint_least32_t new_offset;
  void* const p = allocate_obj(sizeof(prop_bt) + namelen + 1, &new_offset);  // 分配内存
  if (p != nullptr) {
    prop_bt* bt = new (p) prop_bt(name, namelen);  // 在分配的内存中构造对象
    *off = new_offset;  // 返回偏移量
    return bt;
  }

  return nullptr;
}

// 创建新的属性信息对象
prop_info* prop_area::new_prop_info(const char* name, uint32_t namelen, const char* value,
                                    uint32_t valuelen, uint_least32_t* const off) {
  uint_least32_t new_offset;
  void* const p = allocate_obj(sizeof(prop_info) + namelen + 1, &new_offset);  // 分配内存
  if (p == nullptr) return nullptr;

  prop_info* info;
  if (valuelen >= PROP_VALUE_MAX) {  // 如果值长度超过最大值，创建长属性
    uint32_t long_value_offset = 0;
    char* long_location = reinterpret_cast<char*>(allocate_obj(valuelen + 1, &long_value_offset));
    if (!long_location) return nullptr;

    memcpy(long_location, value, valuelen);  // 复制长值
    long_location[valuelen] = '\0';  // 添加null终止符

    // new_offset和long_value_offset都是基于data_的偏移量，但是prop_info
    // 不知道data_是什么，所以我们将此偏移量更改为从包含它的prop_info指针的偏移量
    long_value_offset -= new_offset;

    info = new (p) prop_info(name, namelen, long_value_offset);  // 构造长属性对象
  } else {
    info = new (p) prop_info(name, namelen, value, valuelen);  // 构造普通属性对象
  }
  *off = new_offset;  // 返回偏移量
  return info;
}

// 偏移量转换为对象指针
void* prop_area::to_prop_obj(uint_least32_t off) {
  if (off > pa_data_size_) return nullptr;  // 检查偏移量是否有效

  return (data_ + off);  // 返回对象指针
}

// 偏移量转换为属性二叉树节点指针
inline prop_bt* prop_area::to_prop_bt(atomic_uint_least32_t* off_p) {
  uint_least32_t off = atomic_load_explicit(off_p, memory_order_consume);  // 原子加载偏移量
  return reinterpret_cast<prop_bt*>(to_prop_obj(off));
}

// 偏移量转换为属性信息指针
inline prop_info* prop_area::to_prop_info(atomic_uint_least32_t* off_p) {
  uint_least32_t off = atomic_load_explicit(off_p, memory_order_consume);  // 原子加载偏移量
  return reinterpret_cast<prop_info*>(to_prop_obj(off));
}

// 获取根节点
inline prop_bt* prop_area::root_node() {
  return reinterpret_cast<prop_bt*>(to_prop_obj(0));  // 根节点在偏移量0处
}

// 比较属性名称
static int cmp_prop_name(const char* one, uint32_t one_len, const char* two, uint32_t two_len) {
  if (one_len < two_len)  // 长度比较
    return -1;
  else if (one_len > two_len)
    return 1;
  else
    return strncmp(one, two, one_len);  // 字符串比较
}

// 在二叉树中查找或创建属性节点
prop_bt* prop_area::find_prop_bt(prop_bt* const bt, const char* name, uint32_t namelen,
                                 bool alloc_if_needed) {
  prop_bt* current = bt;
  while (true) {
    if (!current) {  // 当前节点为空
      return nullptr;
    }

    const int ret = cmp_prop_name(name, namelen, current->name, current->namelen);  // 比较属性名
    if (ret == 0) {  // 找到匹配的节点
      return current;
    }

    if (ret < 0) {  // 当前名称小于节点名称，查找左子树
      uint_least32_t left_offset = atomic_load_explicit(&current->left, memory_order_relaxed);
      if (left_offset != 0) {
        current = to_prop_bt(&current->left);  // 移动到左子节点
      } else {
        if (!alloc_if_needed) {  // 如果不需要分配新节点
          return nullptr;
        }

        uint_least32_t new_offset;
        prop_bt* new_bt = new_prop_bt(name, namelen, &new_offset);  // 创建新节点
        if (new_bt) {
          atomic_store_explicit(&current->left, new_offset, memory_order_release);  // 链接新节点
        }
        return new_bt;
      }
    } else {  // 当前名称大于节点名称，查找右子树
      uint_least32_t right_offset = atomic_load_explicit(&current->right, memory_order_relaxed);
      if (right_offset != 0) {
        current = to_prop_bt(&current->right);  // 移动到右子节点
      } else {
        if (!alloc_if_needed) {  // 如果不需要分配新节点
          return nullptr;
        }

        uint_least32_t new_offset;
        prop_bt* new_bt = new_prop_bt(name, namelen, &new_offset);  // 创建新节点
        if (new_bt) {
          atomic_store_explicit(&current->right, new_offset, memory_order_release);  // 链接新节点
        }
        return new_bt;
      }
    }
  }
}

// 遍历属性树路径
prop_bt* prop_area::traverse_trie(prop_bt* const trie, const char* name, bool alloc_if_needed) {
  if (!trie) return nullptr;  // 树为空

  const char* remaining_name = name;  // 剩余待解析的名称
  prop_bt* current = trie;
  while (true) {
    const char* sep = strchr(remaining_name, '.');  // 查找点分隔符
    const bool want_subtree = (sep != nullptr);
    const uint32_t substr_size = (want_subtree) ? sep - remaining_name : strlen(remaining_name);

    if (!substr_size) {  // 子字符串为空
      return nullptr;
    }

    prop_bt* root = nullptr;
    uint_least32_t children_offset = atomic_load_explicit(&current->children, memory_order_relaxed);
    if (children_offset != 0) {  // 如果有子节点
      root = to_prop_bt(&current->children);
    } else if (alloc_if_needed) {  // 如果需要分配新节点
      uint_least32_t new_offset;
      root = new_prop_bt(remaining_name, substr_size, &new_offset);  // 创建新子节点
      if (root) {
        atomic_store_explicit(&current->children, new_offset, memory_order_release);  // 链接子节点
      }
    }

    if (!root) {  // 无法获取或创建根节点
      return nullptr;
    }

    current = find_prop_bt(root, remaining_name, substr_size, alloc_if_needed);  // 在子树中查找
    if (!current) {
      return nullptr;
    }

    if (!want_subtree) break;  // 如果不需要子树，结束遍历

    remaining_name = sep + 1;  // 移动到下一个部分
  }

  return current;  // 返回找到的节点
}

// 查找或创建属性
const prop_info* prop_area::find_property(prop_bt* const trie, const char* name, uint32_t namelen,
                                          const char* value, uint32_t valuelen,
                                          bool alloc_if_needed) {
  prop_bt* current = traverse_trie(trie, name, alloc_if_needed);  // 遍历到目标节点
  if (!current) return nullptr;

  uint_least32_t prop_offset = atomic_load_explicit(&current->prop, memory_order_relaxed);
  if (prop_offset != 0) {  // 如果节点已有属性
    return to_prop_info(&current->prop);
  } else if (alloc_if_needed) {  // 如果需要创建新属性
    uint_least32_t new_offset;
    prop_info* new_info = new_prop_info(name, namelen, value, valuelen, &new_offset);
    if (new_info) {
      atomic_store_explicit(&current->prop, new_offset, memory_order_release);  // 链接新属性
    }

    return new_info;
  } else {
    return nullptr;
  }
}

// 遍历所有属性（递归实现）
bool prop_area::foreach_property(prop_bt* const trie,
                                 void (*propfn)(const prop_info* pi, void* cookie), void* cookie) {
  if (!trie) return false;  // 树为空

  // 遍历左子树
  uint_least32_t left_offset = atomic_load_explicit(&trie->left, memory_order_relaxed);
  if (left_offset != 0) {
    const int err = foreach_property(to_prop_bt(&trie->left), propfn, cookie);
    if (err < 0) return false;
  }
  // 处理当前节点的属性
  uint_least32_t prop_offset = atomic_load_explicit(&trie->prop, memory_order_relaxed);
  if (prop_offset != 0) {
    prop_info* info = to_prop_info(&trie->prop);
    if (!info) return false;
    propfn(info, cookie);  // 调用回调函数
  }
  // 遍历子节点
  uint_least32_t children_offset = atomic_load_explicit(&trie->children, memory_order_relaxed);
  if (children_offset != 0) {
    const int err = foreach_property(to_prop_bt(&trie->children), propfn, cookie);
    if (err < 0) return false;
  }
  // 遍历右子树
  uint_least32_t right_offset = atomic_load_explicit(&trie->right, memory_order_relaxed);
  if (right_offset != 0) {
    const int err = foreach_property(to_prop_bt(&trie->right), propfn, cookie);
    if (err < 0) return false;
  }

  return true;
}

// 查找属性（公共接口）
const prop_info* prop_area::find(const char* name) {
  return find_property(root_node(), name, strlen(name), nullptr, 0, false);  // 不分配新节点
}

// 添加属性（公共接口）
bool prop_area::add(const char* name, unsigned int namelen, const char* value,
                    unsigned int valuelen) {
  return find_property(root_node(), name, namelen, value, valuelen, true);  // 允许分配新节点
}

// 遍历所有属性（公共接口）
bool prop_area::foreach (void (*propfn)(const prop_info* pi, void* cookie), void* cookie) {
  return foreach_property(root_node(), propfn, cookie);  // 从根节点开始遍历
}

#define get_offset(ptr)        atomic_load_explicit(ptr, memory_order_relaxed)  // 获取偏移量宏
#define set_offset(ptr, val)   atomic_store_explicit(ptr, val, memory_order_release)  // 设置偏移量宏

// 删除属性后，可能会在trie中出现冗余节点
// 通过数据结构进行DFS，删除不包含属性的叶节点，
// 将它们从trie中删除，然后递归回溯并删除冗余的父节点
// 当此方法返回true时，从父节点分离该节点
bool prop_area::prune_trie(prop_bt *const node) {
  bool is_leaf = true;  // 是否为叶节点
  if (get_offset(&node->children) != 0) {
    if (prune_trie(to_prop_bt(&node->children))) {
      set_offset(&node->children, 0u);
    } else {
      is_leaf = false;
    }
  }
  if (get_offset(&node->left) != 0) {
    if (prune_trie(to_prop_bt(&node->left))) {
      set_offset(&node->left, 0u);
    } else {
      is_leaf = false;
    }
  }
  if (get_offset(&node->right) != 0) {
    if (prune_trie(to_prop_bt(&node->right))) {
      set_offset(&node->right, 0u);
    } else {
      is_leaf = false;
    }
  }

  if (is_leaf && get_offset(&node->prop) == 0) {
    // Wipe the node
    memset(node->name, 0, node->namelen);
    memset(node, 0, sizeof(*node));
    // Then return true to detach the node from parent
    return true;
  }
  return false;
}

// 删除属性
bool prop_area::remove(const char *name, bool prune) {
  prop_bt *node = traverse_trie(root_node(), name, false);  // 查找目标节点
  if (!node) return false;

  uint_least32_t prop_offset = get_offset(&node->prop);  // 获取属性偏移量
  if (prop_offset == 0) return false;  // 节点没有属性

  prop_info *prop = to_prop_info(&node->prop);  // 获取属性信息

  // 尽快从trie中分离属性
  set_offset(&node->prop, 0u);

  // 然后从内存中清除属性
  if (prop->is_long()) {  // 如果是长属性
    char *value = const_cast<char*>(prop->long_value());
    memset(value, 0, strlen(value));  // 清除长值
  }
  memset(prop->name, 0, strlen(prop->name));  // 清除属性名
  memset(prop, 0, sizeof(*prop));  // 清除属性对象

  if (prune) {  // 如果需要修剪
    prune_trie(root_node());  // 修剪trie
  }

  return true;
}
