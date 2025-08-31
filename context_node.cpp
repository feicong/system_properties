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

#include "system_properties/context_node.h"

#include <limits.h>
#include <unistd.h>

#include <async_safe/log.h>

#include "system_properties/system_properties.h"

// pthread_mutex_lock()在争用情况下调用system_properties
// 如果任何system_properties函数在system_property初始化后使用pthread锁，
// 这会造成死锁风险
//
// 因此，下面三个函数使用bionic Lock和静态内存分配

// 打开上下文节点
bool ContextNode::Open(bool access_rw, bool* fsetxattr_failed) {
  lock_.lock();  // 获取锁
  if (pa_) {  // 如果属性区域已存在
    lock_.unlock();
    return true;
  }

  char filename[PROP_FILENAME_MAX];
  int len = async_safe_format_buffer(filename, sizeof(filename), "%s/%s", filename_, context_);
  if (len < 0 || len >= PROP_FILENAME_MAX) {  // 检查文件名长度
    lock_.unlock();
    return false;
  }

  if (access_rw) {  // 如果需要读写访问
    pa_ = prop_area::map_prop_area_rw(filename, context_, fsetxattr_failed);
  } else {  // 只读访问
    pa_ = prop_area::map_prop_area(filename, nullptr);
  }
  lock_.unlock();  // 释放锁
  return pa_;
}

// 检查访问权限并打开
bool ContextNode::CheckAccessAndOpen() {
  if (!pa_ && !no_access_) {  // 如果属性区域不存在且没有访问限制
    if (!CheckAccess() || !Open(false, nullptr)) {  // 检查访问权限并尝试打开
      no_access_ = true;  // 标记为无访问权限
    }
  }
  return pa_;  // 返回属性区域是否存在
}

// 重置访问权限
void ContextNode::ResetAccess() {
  if (!CheckAccess()) {  // 如果没有访问权限
    Unmap();  // 取消映射
    no_access_ = true;  // 标记为无访问权限
  } else {
    no_access_ = false;  // 重置访问权限标志
  }
}

// 检查访问权限
bool ContextNode::CheckAccess() {
  char filename[PROP_FILENAME_MAX];
  int len = async_safe_format_buffer(filename, sizeof(filename), "%s/%s", filename_, context_);
  if (len < 0 || len >= PROP_FILENAME_MAX) {  // 检查文件名长度
    return false;
  }

  return access(filename, R_OK) == 0;  // 检查文件是否可读
}

// 取消映射属性区域
void ContextNode::Unmap() {
  prop_area::unmap_prop_area(&pa_);  // 取消映射并置空指针
}
