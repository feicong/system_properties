/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "system_properties/contexts_serialized.h"

#include <fcntl.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <new>

#include <async_safe/log.h>

#include "system_properties/system_properties.h"

// 初始化上下文节点数组
bool ContextsSerialized::InitializeContextNodes() {
  auto num_context_nodes = property_info_area_file_->num_contexts();
  auto context_nodes_mmap_size = sizeof(ContextNode) * num_context_nodes;
  // 我们希望在系统属性中避免malloc，所以我们使用匿名映射代替 (b/31659220)
  void* const map_result = mmap(nullptr, context_nodes_mmap_size, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (map_result == MAP_FAILED) {
    return false;
  }

  // 设置VMA名称用于调试
  prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, map_result, context_nodes_mmap_size,
        "System property context nodes");

  context_nodes_ = reinterpret_cast<ContextNode*>(map_result);
  num_context_nodes_ = num_context_nodes;
  context_nodes_mmap_size_ = context_nodes_mmap_size;

  // 为每个上下文创建ContextNode对象
  for (size_t i = 0; i < num_context_nodes; ++i) {
    new (&context_nodes_[i]) ContextNode(property_info_area_file_->context(i), filename_);
  }

  return true;
}

// 映射序列化属性区域
// access_rw: 是否以读写方式访问
// fsetxattr_failed: 返回设置xattr是否失败
bool ContextsSerialized::MapSerialPropertyArea(bool access_rw, bool* fsetxattr_failed) {
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

// 初始化属性信息和上下文节点
bool ContextsSerialized::InitializeProperties() {
  // 加载默认路径的属性信息文件
  if (!property_info_area_file_.LoadDefaultPath()) {
    return false;
  }

  // 初始化上下文节点数组
  if (!InitializeContextNodes()) {
    FreeAndUnmap();
    return false;
  }

  return true;
}

// 初始化ContextsSerialized实例
// writable: 是否以可写模式初始化
// filename: 属性文件目录路径
// fsetxattr_failed: 返回设置xattr是否失败
bool ContextsSerialized::Initialize(bool writable, const char* filename, bool* fsetxattr_failed) {
  filename_ = filename;
  // 首先初始化属性信息和上下文节点
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
    for (size_t i = 0; i < num_context_nodes_; ++i) {
      if (!context_nodes_[i].Open(true, fsetxattr_failed)) {
        open_failed = true;
      }
    }
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

// 根据属性名获取对应的属性区域
// name: 属性名
prop_area* ContextsSerialized::GetPropAreaForName(const char* name) {
  uint32_t index;
  // 从属性信息文件中获取上下文索引
  property_info_area_file_->GetPropertyInfoIndexes(name, &index, nullptr);
  if (index == ~0u || index >= num_context_nodes_) {
    async_safe_format_log(ANDROID_LOG_ERROR, "libc", "Could not find context for property \"%s\"",
                          name);
    return nullptr;
  }
  auto* context_node = &context_nodes_[index];
  if (!context_node->pa()) {
    // 我们在这种情况下明确不检查no_access_，因为与foreach()的情况不同，
    // 我们希望为此函数中每个未被允许的属性访问生成selinux审计
    context_node->Open(false, nullptr);
  }
  return context_node->pa();
}

// 根据属性名获取对应的SELinux上下文
// name: 属性名
const char* ContextsSerialized::GetContextForName(const char* name) {
  const char* context;
  // 从属性信息文件中获取上下文字符串
  property_info_area_file_->GetPropertyInfo(name, &context, nullptr);
  return context;
}

// 遍历所有属性，对每个属性执行指定的函数
// propfn: 对每个属性信息执行的函数
// cookie: 传递给propfn的用户数据
void ContextsSerialized::ForEach(void (*propfn)(const prop_info* pi, void* cookie), void* cookie) {
  // 遍历所有上下文节点
  for (size_t i = 0; i < num_context_nodes_; ++i) {
    // 检查访问权限并打开节点，然后遍历该节点中的所有属性
    if (context_nodes_[i].CheckAccessAndOpen()) {
      context_nodes_[i].pa()->foreach (propfn, cookie);
    }
  }
}

// 重置所有上下文节点的访问状态
void ContextsSerialized::ResetAccess() {
  for (size_t i = 0; i < num_context_nodes_; ++i) {
    context_nodes_[i].ResetAccess();
  }
}

// 释放内存并取消映射所有资源
void ContextsSerialized::FreeAndUnmap() {
  property_info_area_file_.Reset();  // 重置属性信息文件
  if (context_nodes_ != nullptr) {
    // 取消映射所有上下文节点
    for (size_t i = 0; i < num_context_nodes_; ++i) {
      context_nodes_[i].Unmap();
    }
    // 取消映射上下文节点数组
    munmap(context_nodes_, context_nodes_mmap_size_);
    context_nodes_ = nullptr;
  }
  // 取消映射序列化属性区域
  prop_area::unmap_prop_area(&serial_prop_area_);
  serial_prop_area_ = nullptr;
}
