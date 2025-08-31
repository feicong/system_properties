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

#include "system_properties/prop_info.h"

#include <string.h>

// 长属性的错误消息（用于遗留libc）
constexpr static const char kLongLegacyError[] =
    "Must use __system_property_read_callback() to read";
static_assert(sizeof(kLongLegacyError) < prop_info::kLongLegacyErrorBufferSize,
              "Error message for long properties read by legacy libc must fit within 56 chars");

// 构造函数：用于普通属性
prop_info::prop_info(const char* name, uint32_t namelen, const char* value, uint32_t valuelen) {
  memcpy(this->name, name, namelen);  // 复制属性名
  this->name[namelen] = '\0';  // 添加null终止符
  atomic_init(&this->serial, valuelen << 24);  // 在序列号中编码值长度
  memcpy(this->value, value, valuelen);  // 复制属性值
  this->value[valuelen] = '\0';  // 添加null终止符
}

// 构造函数：用于长属性
prop_info::prop_info(const char* name, uint32_t namelen, uint32_t long_offset) {
  memcpy(this->name, name, namelen);  // 复制属性名
  this->name[namelen] = '\0';  // 添加null终止符

  auto error_value_len = sizeof(kLongLegacyError) - 1;  // 错误消息长度
  atomic_init(&this->serial, error_value_len << 24 | kLongFlag);  // 设置长属性标志
  memcpy(this->long_property.error_message, kLongLegacyError, sizeof(kLongLegacyError));  // 复制错误消息

  this->long_property.offset = long_offset;  // 设置长属性偏移量
}
