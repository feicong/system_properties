//
// Copyright (C) 2017 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "property_info_parser/property_info_parser.h"

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace android {
namespace properties {

namespace {

// 二分查找：在数组中查找通过f(search)比较的元素索引
template <typename F>
int Find(uint32_t array_length, F&& f) {
  int bottom = 0;
  int top = array_length - 1;
  while (top >= bottom) {
    int search = (top + bottom) / 2;  // 计算中间位置

    auto cmp = f(search);  // 比较函数返回值

    if (cmp == 0) return search;      // 找到匹配项
    if (cmp < 0) bottom = search + 1; // 在右半部分继续查找
    if (cmp > 0) top = search - 1;    // 在左半部分继续查找
  }
  return -1;  // 未找到
}

}  // namespace

// 二分查找上下文列表以找到给定上下文字符串的索引
// 仅应由TrieSerializer用于构造Trie树
int PropertyInfoArea::FindContextIndex(const char* context) const {
  return Find(num_contexts(), [this, context](auto array_offset) {
    // 获取上下文字符串的偏移量并进行比较
    auto string_offset = uint32_array(contexts_array_offset())[array_offset];
    return strcmp(c_string(string_offset), context);
  });
}

// 二分查找类型列表以找到给定类型字符串的索引
// 仅应由TrieSerializer用于构造Trie树
int PropertyInfoArea::FindTypeIndex(const char* type) const {
  return Find(num_types(), [this, type](auto array_offset) {
    // 获取类型字符串的偏移量并进行比较
    auto string_offset = uint32_array(types_array_offset())[array_offset];
    return strcmp(c_string(string_offset), type);
  });
}

// 二分查找子节点列表以找到给定属性片段的TrieNode
// 用于在GetPropertyInfoIndexes()中遍历Trie树
bool TrieNode::FindChildForString(const char* name, uint32_t namelen, TrieNode* child) const {
  auto node_index = Find(trie_node_base_->num_child_nodes, [this, name, namelen](auto array_offset) {
    const char* child_name = child_node(array_offset).name();
    int cmp = strncmp(child_name, name, namelen);
    if (cmp == 0 && child_name[namelen] != '\0') {
      // 我们使用strncmp()因为name不是以null结尾的，但我们不想只匹配
      // 子节点名称的前缀，所以这里检查是否只匹配了前缀，
      // 返回1，指示二分查找在数组前面搜索真正的匹配项
      return 1;
    }
    return cmp;
  });

  if (node_index == -1) {
    return false;  // 未找到匹配的子节点
  }
  *child = child_node(node_index);  // 设置找到的子节点
  return true;
}

// 检查前缀匹配，更新上下文和类型索引
// remaining_name: 剩余的属性名部分
// trie_node: 当前Trie节点
// context_index: 上下文索引指针（输出参数）
// type_index: 类型索引指针（输出参数）
void PropertyInfoArea::CheckPrefixMatch(const char* remaining_name, const TrieNode& trie_node,
                                        uint32_t* context_index, uint32_t* type_index) const {
  const uint32_t remaining_name_size = strlen(remaining_name);
  // 遍历当前节点的所有前缀
  for (uint32_t i = 0; i < trie_node.num_prefixes(); ++i) {
    auto prefix_len = trie_node.prefix(i)->namelen;
    if (prefix_len > remaining_name_size) continue;  // 前缀长度超过剩余名称长度

    // 检查前缀是否匹配
    if (!strncmp(c_string(trie_node.prefix(i)->name_offset), remaining_name, prefix_len)) {
      // 更新上下文索引（如果有效）
      if (trie_node.prefix(i)->context_index != ~0u) {
        *context_index = trie_node.prefix(i)->context_index;
      }
      // 更新类型索引（如果有效）
      if (trie_node.prefix(i)->type_index != ~0u) {
        *type_index = trie_node.prefix(i)->type_index;
      }
      return;  // 找到匹配前缀后立即返回
    }
  }
}

// 获取属性信息索引（上下文索引和类型索引）
// name: 属性名
// context_index: 上下文索引指针（输出参数）
// type_index: 类型索引指针（输出参数）
void PropertyInfoArea::GetPropertyInfoIndexes(const char* name, uint32_t* context_index,
                                              uint32_t* type_index) const {
  uint32_t return_context_index = ~0u;  // 初始化为无效值
  uint32_t return_type_index = ~0u;     // 初始化为无效值
  const char* remaining_name = name;    // 剩余待处理的属性名
  auto trie_node = root_node();         // 从根节点开始遍历
  
  while (true) {
    const char* sep = strchr(remaining_name, '.');  // 查找下一个'.'分隔符

    // 应用以'.'分隔的前缀匹配
    if (trie_node.context_index() != ~0u) {
      return_context_index = trie_node.context_index();
    }
    if (trie_node.type_index() != ~0u) {
      return_type_index = trie_node.type_index();
    }

    // 检查此节点的前缀。这在节点检查之后进行，因为这些前缀
    // 根据定义比节点本身更长
    CheckPrefixMatch(remaining_name, trie_node, &return_context_index, &return_type_index);

    if (sep == nullptr) {
      break;  // 没有更多分隔符，到达叶节点
    }

    const uint32_t substr_size = sep - remaining_name;  // 计算子字符串长度
    TrieNode child_node;
    // 查找匹配的子节点
    if (!trie_node.FindChildForString(remaining_name, substr_size, &child_node)) {
      break;  // 没找到匹配的子节点，停止遍历
    }

    trie_node = child_node;     // 移动到子节点
    remaining_name = sep + 1;   // 更新剩余名称
  }

  // 我们已经到达叶节点，检查内容并适当返回
  // 检查精确匹配
  for (uint32_t i = 0; i < trie_node.num_exact_matches(); ++i) {
    if (!strcmp(c_string(trie_node.exact_match(i)->name_offset), remaining_name)) {
      // 找到精确匹配，设置返回值
      if (context_index != nullptr) {
        if (trie_node.exact_match(i)->context_index != ~0u) {
          *context_index = trie_node.exact_match(i)->context_index;
        } else {
          *context_index = return_context_index;  // 使用之前找到的上下文索引
        }
      }
      if (type_index != nullptr) {
        if (trie_node.exact_match(i)->type_index != ~0u) {
          *type_index = trie_node.exact_match(i)->type_index;
        } else {
          *type_index = return_type_index;  // 使用之前找到的类型索引
        }
      }
      return;
    }
  }
  // 检查不以'.'分隔的前缀匹配
  CheckPrefixMatch(remaining_name, trie_node, &return_context_index, &return_type_index);
  // 返回之前找到的前缀匹配结果
  if (context_index != nullptr) *context_index = return_context_index;
  if (type_index != nullptr) *type_index = return_type_index;
  return;
}

// 获取属性信息（上下文和类型字符串）
// property: 属性名
// context: 上下文字符串指针（输出参数）
// type: 类型字符串指针（输出参数）
void PropertyInfoArea::GetPropertyInfo(const char* property, const char** context,
                                       const char** type) const {
  uint32_t context_index;
  uint32_t type_index;
  // 先获取索引
  GetPropertyInfoIndexes(property, &context_index, &type_index);
  
  // 根据索引获取实际的字符串
  if (context != nullptr) {
    if (context_index == ~0u) {
      *context = nullptr;  // 无效索引，返回空指针
    } else {
      *context = this->context(context_index);  // 根据索引获取上下文字符串
    }
  }
  if (type != nullptr) {
    if (type_index == ~0u) {
      *type = nullptr;  // 无效索引，返回空指针
    } else {
      *type = this->type(type_index);  // 根据索引获取类型字符串
    }
  }
}

// 加载默认路径的属性信息文件
bool PropertyInfoAreaFile::LoadDefaultPath() {
  return LoadPath("/dev/__properties__/property_info");
}

// 从指定路径加载属性信息文件
// filename: 文件路径
bool PropertyInfoAreaFile::LoadPath(const char* filename) {
  // 以只读方式打开文件，设置CLOEXEC和NOFOLLOW标志
  int fd = open(filename, O_CLOEXEC | O_NOFOLLOW | O_RDONLY);
  // 获取文件状态信息
  struct stat fd_stat;
  if (fstat(fd, &fd_stat) < 0) {
    close(fd);
    return false;
  }

  // 安全检查：验证文件所有者、权限和大小
  if ((fd_stat.st_uid != 0) || (fd_stat.st_gid != 0) ||
      ((fd_stat.st_mode & (S_IWGRP | S_IWOTH)) != 0) ||
      (fd_stat.st_size < static_cast<off_t>(sizeof(PropertyInfoArea)))) {
    close(fd);
    return false;  // 文件不符合安全要求
  }

  auto mmap_size = fd_stat.st_size;

  // 将文件映射到内存
  void* map_result = mmap(nullptr, mmap_size, PROT_READ, MAP_SHARED, fd, 0);
  if (map_result == MAP_FAILED) {
    close(fd);
    return false;
  }

  // 验证属性信息区域的有效性
  auto property_info_area = reinterpret_cast<PropertyInfoArea*>(map_result);
  if (property_info_area->minimum_supported_version() > 1 ||
      property_info_area->size() != mmap_size) {
    munmap(map_result, mmap_size);  // 验证失败，取消映射
    close(fd);
    return false;
  }

  close(fd);
  mmap_base_ = map_result;  // 保存映射基地址
  mmap_size_ = mmap_size;   // 保存映射大小
  return true;
}

// 重置PropertyInfoAreaFile，释放映射的内存
void PropertyInfoAreaFile::Reset() {
  if (mmap_size_ > 0) {
    munmap(mmap_base_, mmap_size_);  // 取消内存映射
  }
  mmap_base_ = nullptr;  // 重置基地址
  mmap_size_ = 0;        // 重置大小
}

}  // namespace properties
}  // namespace android
