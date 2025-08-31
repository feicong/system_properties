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

#include "system_properties/system_properties.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <new>

#include <async_safe/log.h>

#include "private/ErrnoRestorer.h"
#include "private/bionic_futex.h"

#include "system_properties/context_node.h"
#include "system_properties/prop_area.h"
#include "system_properties/prop_info.h"

// 检查序列号是否脏（用于同步）
// 检查序列号是否脏（用于同步）
#define SERIAL_DIRTY(serial) ((serial)&1)
// 从序列号中获取值长度
#define SERIAL_VALUE_LEN(serial) ((serial) >> 24)

// 检查路径是否为目录
static bool is_dir(const char* pathname) {
  struct stat info;
  if (stat(pathname, &info) == -1) {  // 获取文件状态失败
    return false;
  }
  return S_ISDIR(info.st_mode);  // 检查是否为目录类型
}

// 初始化系统属性
bool SystemProperties::Init(const char* filename) {
  // 这个函数从__libc_init_common调用，应该保持errno为0 (http://b/37248982)
  ErrnoRestorer errno_restorer;

  if (initialized_) {  // 如果已经初始化，重置访问权限
    contexts_->ResetAccess();
    return true;
  }

  if (strlen(filename) >= PROP_FILENAME_MAX) {  // 检查文件名长度
    return false;
  }
  strcpy(property_filename_, filename);  // 保存属性文件名

  if (is_dir(property_filename_)) {  // 如果是目录
    // 根据property_info文件的存在选择不同的上下文实现
    if (access("/dev/__properties__/property_info", R_OK) == 0) {
      contexts_ = new (contexts_data_) ContextsSerialized();  // 使用序列化上下文
      if (!contexts_->Initialize(false, property_filename_, nullptr)) {
        return false;
      }
    } else {
      contexts_ = new (contexts_data_) ContextsSplit();  // 使用分割上下文
      if (!contexts_->Initialize(false, property_filename_, nullptr)) {
        return false;
      }
    }
  } else {
    // 使用预分割的上下文
    contexts_ = new (contexts_data_) ContextsPreSplit();
    if (!contexts_->Initialize(false, property_filename_, nullptr)) {
      return false;
    }
  }
  initialized_ = true;  // 标记为已初始化
  return true;
}

// 初始化属性区域
bool SystemProperties::AreaInit(const char* filename, bool* fsetxattr_failed) {
  if (strlen(filename) >= PROP_FILENAME_MAX) {  // 检查文件名长度
    return false;
  }
  strcpy(property_filename_, filename);  // 保存属性文件名

  contexts_ = new (contexts_data_) ContextsSerialized();  // 创建序列化上下文
  if (!contexts_->Initialize(true, property_filename_, fsetxattr_failed)) {  // 初始化为读写模式
    return false;
  }
  initialized_ = true;  // 标记为已初始化
  return true;
}

// 获取属性区域的序列号
uint32_t SystemProperties::AreaSerial() {
  if (!initialized_) {  // 检查是否已初始化
    return -1;
  }

  prop_area* pa = contexts_->GetSerialPropArea();  // 获取序列属性区域
  if (!pa) {
    return -1;
  }

  // 确保在__system_property_serial之前完成此读取
  return atomic_load_explicit(pa->serial(), memory_order_acquire);
}

// 查找属性信息
const prop_info* SystemProperties::Find(const char* name) {
  if (!initialized_) {  // 检查是否已初始化
    return nullptr;
  }

  prop_area* pa = contexts_->GetPropAreaForName(name);  // 根据属性名获取属性区域
  if (!pa) {
    async_safe_format_log(ANDROID_LOG_WARN, "libc", "Access denied finding property \"%s\"", name);
    return nullptr;
  }

  return pa->find(name);  // 在属性区域中查找属性
}

// 检查属性是否为只读
static bool is_read_only(const char* name) {
  return strncmp(name, "ro.", 3) == 0;  // 以"ro."开头的属性为只读
}

// 读取可变属性值
uint32_t SystemProperties::ReadMutablePropertyValue(const prop_info* pi, char* value) {
  // 我们假设下面的memcpy通过获取栅栏进行序列化
  uint32_t new_serial = load_const_atomic(&pi->serial, memory_order_acquire);
  uint32_t serial;
  unsigned int len;
  for (;;) {  // 循环直到读取到一致的值
    serial = new_serial;
    len = SERIAL_VALUE_LEN(serial);  // 从序列号中提取值长度
    if (__predict_false(SERIAL_DIRTY(serial))) {  // 如果序列号标记为脏
      // 参见prop_area构造函数中的注释
      prop_area* pa = contexts_->GetPropAreaForName(pi->name);
      memcpy(value, pa->dirty_backup_area(), len + 1);  // 从备份区域复制
    } else {
      memcpy(value, pi->value, len + 1);  // 从主区域复制
    }
    atomic_thread_fence(memory_order_acquire);  // 内存栅栏
    new_serial = load_const_atomic(&pi->serial, memory_order_relaxed);
    if (__predict_true(serial == new_serial)) {  // 如果序列号没有变化
      break;
    }
    // 我们在这里需要另一个栅栏，因为我们想确保循环下一次迭代中的memcpy
    // 发生在上面new_serial的加载之后。我们可以通过让new_serial的load_const_atomic
    // 使用memory_order_acquire而不是memory_order_relaxed来获得这个保证，
    // 但即使在序列号没有变化的绝大多数情况下，我们也会付出memory_order_acquire的代价
    atomic_thread_fence(memory_order_acquire);
  }
  return serial;
}

// 读取属性信息
int SystemProperties::Read(const prop_info* pi, char* name, char* value) {
  uint32_t serial = ReadMutablePropertyValue(pi, value);  // 读取属性值
  if (name != nullptr) {  // 如果需要返回属性名
    size_t namelen = strlcpy(name, pi->name, PROP_NAME_MAX);
    if (namelen >= PROP_NAME_MAX) {  // 检查属性名是否被截断
      async_safe_format_log(ANDROID_LOG_ERROR, "libc",
                            "The property name length for \"%s\" is >= %d;"
                            " please use __system_property_read_callback"
                            " to read this property. (the name is truncated to \"%s\")",
                            pi->name, PROP_NAME_MAX - 1, name);
    }
  }
  if (is_read_only(pi->name) && pi->is_long()) {  // 检查只读长属性
    async_safe_format_log(
        ANDROID_LOG_ERROR, "libc",
        "The property \"%s\" has a value with length %zu that is too large for"
        " __system_property_get()/__system_property_read(); use"
        " __system_property_read_callback() instead.",
        pi->name, strlen(pi->long_value()));
  }
  return SERIAL_VALUE_LEN(serial);  // 返回值长度
}

// 通过回调读取属性
void SystemProperties::ReadCallback(const prop_info* pi,
                                    void (*callback)(void* cookie, const char* name,
                                                     const char* value, uint32_t serial),
                                    void* cookie) {
  // 只读属性不需要将值复制到临时缓冲区，因为它永远不会改变
  // 出于同样的原因，我们在序列加载上使用relaxed内存顺序
  if (is_read_only(pi->name)) {
    uint32_t serial = load_const_atomic(&pi->serial, memory_order_relaxed);
    if (pi->is_long()) {  // 如果是长属性
      callback(cookie, pi->name, pi->long_value(), serial);
    } else {
      callback(cookie, pi->name, pi->value, serial);
    }
    return;
  }

  char value_buf[PROP_VALUE_MAX];  // 为可变属性创建缓冲区
  uint32_t serial = ReadMutablePropertyValue(pi, value_buf);
  callback(cookie, pi->name, value_buf, serial);
}

// 获取属性值
int SystemProperties::Get(const char* name, char* value) {
  const prop_info* pi = Find(name);  // 查找属性信息

  if (pi != nullptr) {  // 如果找到属性
    return Read(pi, nullptr, value);  // 读取属性值
  } else {
    value[0] = 0;  // 属性不存在，设置为空字符串
    return 0;
  }
}

// 更新属性值
int SystemProperties::Update(prop_info* pi, const char* value, unsigned int len) {
  if (len >= PROP_VALUE_MAX) {  // 检查值长度
    return -1;
  }

  if (!initialized_) {  // 检查是否已初始化
    return -1;
  }

  if (!contexts_->rw_) {  // 检查是否有写权限
    return -1;
  }

  prop_area* serial_pa = contexts_->GetSerialPropArea();  // 获取序列属性区域
  if (!serial_pa) {
    return -1;
  }
  prop_area* pa = contexts_->GetPropAreaForName(pi->name);  // 获取属性所在区域
  if (__predict_false(!pa)) {
    async_safe_format_log(ANDROID_LOG_ERROR, "libc", "Could not find area for \"%s\"", pi->name);
    return -1;
  }

  uint32_t serial = atomic_load_explicit(&pi->serial, memory_order_relaxed);
  unsigned int old_len = SERIAL_VALUE_LEN(serial);  // 获取旧值长度

  // 与读取器的约定是，每当设置脏位时，预脏值的未损坏副本
  // 在脏备份区域中可用。栅栏确保我们在允许读取器看到
  // 脏序列之前发布我们的脏区域更新
  memcpy(pa->dirty_backup_area(), pi->value, old_len + 1);  // 备份旧值
  atomic_thread_fence(memory_order_release);
  serial |= 1;  // 设置脏位
  atomic_store_explicit(&pi->serial, serial, memory_order_relaxed);
  strlcpy(pi->value, value, len + 1);  // 复制新值
  // 现在主值属性区域是最新的。让读取器知道他们应该
  // 查看属性值而不是备份区域
  atomic_thread_fence(memory_order_release);
  atomic_store_explicit(&pi->serial, (len << 24) | ((serial + 1) & 0xffffff), memory_order_relaxed);
  __futex_wake(&pi->serial, INT32_MAX);  // 通过副作用进行栅栏
  atomic_store_explicit(serial_pa->serial(),
                        atomic_load_explicit(serial_pa->serial(), memory_order_relaxed) + 1,
                        memory_order_release);
  __futex_wake(serial_pa->serial(), INT32_MAX);  // 唤醒等待者

  return 0;
}

// 添加新属性
int SystemProperties::Add(const char* name, unsigned int namelen, const char* value,
                          unsigned int valuelen) {
  if (valuelen >= PROP_VALUE_MAX && !is_read_only(name)) {  // 检查非只读属性的值长度
    return -1;
  }

  if (namelen < 1) {  // 检查属性名长度
    return -1;
  }

  if (!initialized_) {  // 检查是否已初始化
    return -1;
  }

  if (!contexts_->rw_) {  // 检查是否有写权限
    return -1;
  }

  prop_area* serial_pa = contexts_->GetSerialPropArea();  // 获取序列属性区域
  if (serial_pa == nullptr) {
    return -1;
  }

  prop_area* pa = contexts_->GetPropAreaForName(name);  // 获取属性所在区域
  if (!pa) {
    async_safe_format_log(ANDROID_LOG_ERROR, "libc", "Access denied adding property \"%s\"", name);
    return -1;
  }

  bool ret = pa->add(name, namelen, value, valuelen);  // 添加属性到区域
  if (!ret) {
    return -1;
  }

  // 只有一个修改器，但我们想确保更新对等待更新的读取器可见
  atomic_store_explicit(serial_pa->serial(),
                        atomic_load_explicit(serial_pa->serial(), memory_order_relaxed) + 1,
                        memory_order_release);
  __futex_wake(serial_pa->serial(), INT32_MAX);  // 唤醒等待者
  return 0;
}

// 删除属性
int SystemProperties::Delete(const char *name, bool prune) {
  if (!initialized_) {  // 检查是否已初始化
    return -1;
  }

  if (!contexts_->rw_) {  // 检查是否有写权限
    return -1;
  }

  prop_area* serial_pa = contexts_->GetSerialPropArea();  // 获取序列属性区域
  if (serial_pa == nullptr) {
    return -1;
  }

  prop_area* pa = contexts_->GetPropAreaForName(name);  // 获取属性所在区域
  if (!pa) {
    async_safe_format_log(ANDROID_LOG_ERROR, "libc",
                          "Access denied deleting property \"%s\"", name);
    return -1;
  }

  bool ret = pa->remove(name, prune);  // 从区域中删除属性
  if (!ret) {
    return -1;
  }

  // 只有一个修改器，但我们想确保更新对等待更新的读取器可见
  atomic_store_explicit(serial_pa->serial(),
                        atomic_load_explicit(serial_pa->serial(), memory_order_relaxed) + 1,
                        memory_order_release);
  __futex_wake(serial_pa->serial(), INT32_MAX);  // 唤醒等待者
  return 0;
}

// 获取属性的SELinux上下文
const char* SystemProperties::GetContext(const char* name) {
  if (!initialized_) {  // 检查是否已初始化
    return nullptr;
  }

  return contexts_->GetContextForName(name);  // 根据属性名获取上下文
}

// 等待任意属性变化
uint32_t SystemProperties::WaitAny(uint32_t old_serial) {
  uint32_t new_serial;
  Wait(nullptr, old_serial, &new_serial, nullptr);  // 等待全局序列号变化
  return new_serial;
}

// 等待属性变化
bool SystemProperties::Wait(const prop_info* pi, uint32_t old_serial, uint32_t* new_serial_ptr,
                            const timespec* relative_timeout) {
  // 我们是在等待全局序列还是特定序列？
  atomic_uint_least32_t* serial_ptr;
  if (pi == nullptr) {  // 等待全局序列
    if (!initialized_) {
      return -1;
    }

    prop_area* serial_pa = contexts_->GetSerialPropArea();  // 获取序列属性区域
    if (serial_pa == nullptr) {
      return -1;
    }

    serial_ptr = serial_pa->serial();
  } else {  // 等待特定属性序列
    serial_ptr = const_cast<atomic_uint_least32_t*>(&pi->serial);
  }

  uint32_t new_serial;
  do {
    int rc;
    if ((rc = __futex_wait(serial_ptr, old_serial, relative_timeout)) != 0 && rc == -ETIMEDOUT) {
      return false;  // 超时
    }
    new_serial = load_const_atomic(serial_ptr, memory_order_acquire);
  } while (new_serial == old_serial);  // 继续等待直到序列号变化

  *new_serial_ptr = new_serial;
  return true;
}

// 查找第n个属性
const prop_info* SystemProperties::FindNth(unsigned n) {
  struct find_nth {  // 用于查找第n个属性的辅助结构
    const uint32_t sought;  // 要查找的索引
    uint32_t current;       // 当前索引
    const prop_info* result; // 结果属性信息

    explicit find_nth(uint32_t n) : sought(n), current(0), result(nullptr) {
    }
    static void fn(const prop_info* pi, void* ptr) {  // 遍历回调函数
      find_nth* self = reinterpret_cast<find_nth*>(ptr);
      if (self->current++ == self->sought) self->result = pi;  // 找到目标属性
    }
  } state(n);
  Foreach(find_nth::fn, &state);  // 遍历所有属性
  return state.result;
}

// 遍历所有属性
int SystemProperties::Foreach(void (*propfn)(const prop_info* pi, void* cookie), void* cookie) {
  if (!initialized_) {  // 检查是否已初始化
    return -1;
  }

  contexts_->ForEach(propfn, cookie);  // 调用上下文的遍历方法

  return 0;
}
