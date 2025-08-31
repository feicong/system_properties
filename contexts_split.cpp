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

#include "system_properties/contexts_split.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <async_safe/log.h>

#include "system_properties/context_node.h"
#include "system_properties/system_properties.h"

// 上下文列表节点类，继承自ContextNode，用于管理属性的SELinux上下文
class ContextListNode : public ContextNode {
 public:
  // 构造函数：创建一个新的上下文列表节点
  // next: 链表中的下一个节点
  // context: SELinux上下文字符串
  // filename: 属性文件名
  ContextListNode(ContextListNode* next, const char* context, const char* filename)
      : ContextNode(strdup(context), filename), next(next) {
  }

  // 析构函数：释放复制的上下文字符串内存
  ~ContextListNode() {
    free(const_cast<char*>(context()));
  }

  ContextListNode* next; // 指向链表中下一个节点的指针
};

// 前缀节点结构，用于管理属性名前缀到上下文的映射
struct PrefixNode {
  // 构造函数：创建一个新的前缀节点
  // next: 链表中的下一个节点
  // prefix: 属性名前缀字符串
  // context: 关联的上下文列表节点
  PrefixNode(struct PrefixNode* next, const char* prefix, ContextListNode* context)
      : prefix(strdup(prefix)), prefix_len(strlen(prefix)), context(context), next(next) {
  }
  // 析构函数：释放前缀字符串内存
  ~PrefixNode() {
    free(prefix);
  }
  char* prefix;              // 属性名前缀字符串
  const size_t prefix_len;   // 前缀长度，用于快速比较
  ContextListNode* context;  // 关联的上下文节点
  PrefixNode* next;          // 指向链表中下一个节点
};

// 模板函数：向链表头部添加新节点
// List: 链表类型
// Args: 构造函数参数类型
template <typename List, typename... Args>
static inline void ListAdd(List** list, Args... args) {
  *list = new List(*list, args...);  // 创建新节点并插入到链表头部
}

// 按长度顺序添加前缀节点到链表中
// 确保较长的前缀排在前面，通配符(*)排在最后
static void ListAddAfterLen(PrefixNode** list, const char* prefix, ContextListNode* context) {
  size_t prefix_len = strlen(prefix);  // 计算前缀长度

  auto next_list = list;

  // 遍历链表找到合适的插入位置
  while (*next_list) {
    // 如果当前节点前缀更短或是通配符，在此处插入
    if ((*next_list)->prefix_len < prefix_len || (*next_list)->prefix[0] == '*') {
      ListAdd(next_list, prefix, context);
      return;
    }
    next_list = &(*next_list)->next;
  }
  // 插入到链表末尾
  ListAdd(next_list, prefix, context);
}

// 模板函数：遍历链表并对每个节点执行指定操作
// List: 链表类型
// Func: 函数对象类型
template <typename List, typename Func>
static void ListForEach(List* list, Func func) {
  while (list) {
    func(list);         // 对当前节点执行操作
    list = list->next;  // 移动到下一个节点
  }
}

// 模板函数：在链表中查找满足条件的节点
// List: 链表类型
// Func: 谓词函数类型
template <typename List, typename Func>
static List* ListFind(List* list, Func func) {
  while (list) {
    if (func(list)) {   // 如果找到满足条件的节点
      return list;
    }
    list = list->next;
  }
  return nullptr;       // 没找到返回空指针
}

// 模板函数：释放链表并删除所有节点
template <typename List>
static void ListFree(List** list) {
  while (*list) {
    auto old_list = *list;     // 保存当前节点
    *list = old_list->next;    // 移动到下一个节点
    delete old_list;           // 删除当前节点
  }
}

// 以下两个函数复制自libselinux中的label_support.c
// read_spec_entries和read_spec_entry函数可用于替代sscanf来读取spec文件中的条目
// 文件和属性服务现在使用这些函数

// 从spec文件中读取一个条目（例如file_contexts）
static inline int read_spec_entry(char** entry, char** ptr, int* len) {
  *entry = nullptr;
  char* tmp_buf = nullptr;

  // 跳过开头的空白字符
  while (isspace(**ptr) && **ptr != '\0') (*ptr)++;

  tmp_buf = *ptr;
  *len = 0;

  // 读取非空白字符组成的条目
  while (!isspace(**ptr) && **ptr != '\0') {
    (*ptr)++;
    (*len)++;
  }

  if (*len) {
    *entry = strndup(tmp_buf, *len);  // 复制条目字符串
    if (!*entry) return -1;          // 内存分配失败
  }

  return 0;
}

// 读取spec文件中的多个条目
// line_buf - 包含spec条目的缓冲区
// num_args - 要处理的spec参数条目数量
// ...      - 每个参数对应的'char **spec_entry'
// 返回值   - 处理的条目数量
//
// 此函数调用read_spec_entry()来进行实际的字符串处理
static int read_spec_entries(char* line_buf, int num_args, ...) {
  char **spec_entry, *buf_p;
  int len, rc, items, entry_len = 0;
  va_list ap;

  len = strlen(line_buf);
  if (line_buf[len - 1] == '\n')
    line_buf[len - 1] = '\0';  // 移除行尾换行符
  else
    // 处理行不以\n结尾的情况，通过增加len来处理下面的检查
    // （因为getline(3)会以NUL终止行）
    len++;

  buf_p = line_buf;
  while (isspace(*buf_p)) buf_p++;  // 跳过开头空白字符

  // 跳过注释行和空行
  if (*buf_p == '#' || *buf_p == '\0') return 0;

  // 处理spec文件条目
  va_start(ap, num_args);

  items = 0;
  while (items < num_args) {
    spec_entry = va_arg(ap, char**);

    if (len - 1 == buf_p - line_buf) {  // 已到达行尾
      va_end(ap);
      return items;
    }

    rc = read_spec_entry(spec_entry, &buf_p, &entry_len);
    if (rc < 0) {
      va_end(ap);
      return rc;
    }
    if (entry_len) items++;  // 如果读取到条目则计数增加
  }
  va_end(ap);
  return items;
}

// 映射序列化属性区域
// access_rw: 是否以读写方式访问
// fsetxattr_failed: 返回设置xattr是否失败
bool ContextsSplit::MapSerialPropertyArea(bool access_rw, bool* fsetxattr_failed) {
  char filename[PROP_FILENAME_MAX];
  // 构造序列化属性文件路径
  int len = async_safe_format_buffer(filename, sizeof(filename), "%s/properties_serial", filename_);
  if (len < 0 || len >= PROP_FILENAME_MAX) {
    serial_prop_area_ = nullptr;
    return false;
  }

  if (access_rw) {
    // 以读写方式映射属性区域，设置SELinux上下文
    serial_prop_area_ =
        prop_area::map_prop_area_rw(filename, "u:object_r:properties_serial:s0", fsetxattr_failed);
  } else {
    // 以只读方式映射属性区域
    serial_prop_area_ = prop_area::map_prop_area(filename, &rw_);
  }
  return serial_prop_area_;
}

// 从指定文件初始化属性上下文映射
// filename: 属性上下文配置文件路径
bool ContextsSplit::InitializePropertiesFromFile(const char* filename) {
  FILE* file = fopen(filename, "re");
  if (!file) {
    return false;
  }

  char* buffer = nullptr;
  size_t line_len;
  char* prop_prefix = nullptr;
  char* context = nullptr;

  // 逐行读取配置文件
  while (getline(&buffer, &line_len, file) > 0) {
    // 解析每行的前缀和上下文条目
    int items = read_spec_entries(buffer, 2, &prop_prefix, &context);
    if (items <= 0) {
      continue;  // 跳过空行或解析失败的行
    }
    if (items == 1) {
      free(prop_prefix);  // 只有前缀没有上下文，释放内存并继续
      continue;
    }

    // init使用ctl.*属性作为IPC机制，不会将它们写入属性文件，
    // 因此我们不需要创建属性文件来存储它们
    if (!strncmp(prop_prefix, "ctl.", 4)) {
      free(prop_prefix);
      free(context);
      continue;
    }

    // 查找是否已存在相同的上下文节点
    auto old_context = ListFind(
        contexts_, [context](ContextListNode* l) { return !strcmp(l->context(), context); });
    if (old_context) {
      // 重用已存在的上下文节点
      ListAddAfterLen(&prefixes_, prop_prefix, old_context);
    } else {
      // 创建新的上下文节点
      ListAdd(&contexts_, context, filename_);
      ListAddAfterLen(&prefixes_, prop_prefix, contexts_);
    }
    free(prop_prefix);
    free(context);
  }

  free(buffer);
  fclose(file);

  return true;
}

// 初始化属性上下文映射，从多个可能的配置文件中读取
bool ContextsSplit::InitializeProperties() {
  // 如果找到/property_contexts，说明这是在旧版本上运行的OTA更新程序，
  // 该版本有/property_contexts - b/34370523
  if (InitializePropertiesFromFile("/property_contexts")) {
    return true;
  }

  // 使用来自/system和/vendor的property_contexts，回退到根目录下的文件
  if (access("/system/etc/selinux/plat_property_contexts", R_OK) != -1) {
    // 加载平台属性上下文配置
    if (!InitializePropertiesFromFile("/system/etc/selinux/plat_property_contexts")) {
      return false;
    }
    // 不检查失败，因为我们并不总是有所有这些分区
    // 例如在recovery模式下，vendor分区不会挂载，我们仍然需要
    // system/platform属性来正常工作
    if (access("/vendor/etc/selinux/vendor_property_contexts", R_OK) != -1) {
      InitializePropertiesFromFile("/vendor/etc/selinux/vendor_property_contexts");
    } else {
      // 如果vendor_*不存在，回退到nonplat_*
      InitializePropertiesFromFile("/vendor/etc/selinux/nonplat_property_contexts");
    }
  } else {
    // 回退到旧的文件路径结构
    if (!InitializePropertiesFromFile("/plat_property_contexts")) {
      return false;
    }
    if (access("/vendor_property_contexts", R_OK) != -1) {
      InitializePropertiesFromFile("/vendor_property_contexts");
    } else {
      // 如果vendor_*不存在，回退到nonplat_*
      InitializePropertiesFromFile("/nonplat_property_contexts");
    }
  }

  return true;
}

// 初始化ContextsSplit实例
// writable: 是否以可写模式初始化
// filename: 属性文件目录路径
// fsetxattr_failed: 返回设置xattr是否失败
bool ContextsSplit::Initialize(bool writable, const char* filename, bool* fsetxattr_failed) {
  filename_ = filename;
  // 首先初始化属性上下文映射
  if (!InitializeProperties()) {
    return false;
  }

  if (writable) {
    // 创建属性目录，设置适当的权限
    mkdir(filename_, S_IRWXU | S_IXGRP | S_IXOTH);
    bool open_failed = false;
    if (fsetxattr_failed) {
      *fsetxattr_failed = false;
    }

    // 以读写模式打开所有上下文节点
    ListForEach(contexts_, [&fsetxattr_failed, &open_failed](ContextListNode* l) {
      if (!l->Open(true, fsetxattr_failed)) {
        open_failed = true;
      }
    });
    // 如果有节点打开失败或映射序列化区域失败，清理并返回失败
    if (open_failed || !MapSerialPropertyArea(true, fsetxattr_failed)) {
      FreeAndUnmap();
      return false;
    }
  } else {
    // 只读模式，只需要映射序列化属性区域
    if (!MapSerialPropertyArea(false, nullptr)) {
      FreeAndUnmap();
      return false;
    }
  }
  return true;
}

// 根据属性名获取对应的前缀节点
// name: 属性名
PrefixNode* ContextsSplit::GetPrefixNodeForName(const char* name) {
  // 查找匹配的前缀节点，支持通配符(*)或前缀匹配
  auto entry = ListFind(prefixes_, [name](PrefixNode* l) {
    return l->prefix[0] == '*' || !strncmp(l->prefix, name, l->prefix_len);
  });

  return entry;
}

// 根据属性名获取对应的属性区域
// name: 属性名
prop_area* ContextsSplit::GetPropAreaForName(const char* name) {
  auto entry = GetPrefixNodeForName(name);
  if (!entry) {
    return nullptr;
  }

  auto cnode = entry->context;
  if (!cnode->pa()) {
    // 我们在这种情况下明确不检查no_access_，因为与foreach()的情况不同，
    // 我们希望为此函数中每个未被允许的属性访问生成selinux审计
    cnode->Open(false, nullptr);
  }
  return cnode->pa();
}

// 根据属性名获取对应的SELinux上下文
// name: 属性名
const char* ContextsSplit::GetContextForName(const char* name) {
  auto entry = GetPrefixNodeForName(name);
  if (!entry) {
    return nullptr;
  }
  return entry->context->context();
}

// 遍历所有属性，对每个属性执行指定的函数
// propfn: 对每个属性信息执行的函数
// cookie: 传递给propfn的用户数据
void ContextsSplit::ForEach(void (*propfn)(const prop_info* pi, void* cookie), void* cookie) {
  // 遍历所有上下文节点
  ListForEach(contexts_, [propfn, cookie](ContextListNode* l) {
    // 检查访问权限并打开节点，然后遍历该节点中的所有属性
    if (l->CheckAccessAndOpen()) {
      l->pa()->foreach (propfn, cookie);
    }
  });
}

// 重置所有上下文节点的访问状态
void ContextsSplit::ResetAccess() {
  ListForEach(contexts_, [](ContextListNode* l) { l->ResetAccess(); });
}

// 释放内存并取消映射所有资源
void ContextsSplit::FreeAndUnmap() {
  ListFree(&prefixes_);                                // 释放前缀节点链表
  ListFree(&contexts_);                                // 释放上下文节点链表
  prop_area::unmap_prop_area(&serial_prop_area_);      // 取消映射序列化属性区域
}
