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

#include <errno.h>
#include <poll.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#define _REALLY_INCLUDE_SYS__SYSTEM_PROPERTIES_H_
#include <api/_system_properties.h>
#include <unistd.h>

#include <async_safe/log.h>
#include <async_safe/CHECK.h>

#include "private/bionic_defs.h"
#include "platform/bionic/macros.h"
#include "private/ScopedFd.h"

// 属性服务套接字路径
static const char property_service_socket[] = "/dev/socket/" PROP_SERVICE_NAME;
// 服务版本属性名称
static const char* kServiceVersionPropertyName = "ro.property_service.version";

// 属性服务连接类，管理与属性服务的socket连接
class PropertyServiceConnection {
 public:
  // 构造函数：创建并连接到属性服务
  PropertyServiceConnection() : last_error_(0) {
    // 创建本地socket，设置CLOEXEC标志
    socket_.reset(::socket(AF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (socket_.get() == -1) {
      last_error_ = errno;
      return;
    }

    // 设置socket地址结构
    const size_t namelen = strlen(property_service_socket);
    sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    strlcpy(addr.sun_path, property_service_socket, sizeof(addr.sun_path));
    addr.sun_family = AF_LOCAL;
    socklen_t alen = namelen + offsetof(sockaddr_un, sun_path) + 1;

    // 连接到属性服务
    if (TEMP_FAILURE_RETRY(connect(socket_.get(),
                                   reinterpret_cast<sockaddr*>(&addr), alen)) == -1) {
      last_error_ = errno;
      socket_.reset();  // 连接失败，重置socket
    }
  }

  // 检查连接是否有效
  bool IsValid() {
    return socket_.get() != -1;
  }

  // 获取最后的错误码
  int GetLastError() {
    return last_error_;
  }

  // 接收32位整数
  bool RecvInt32(int32_t* value) {
    int result = TEMP_FAILURE_RETRY(recv(socket_.get(), value, sizeof(*value), MSG_WAITALL));
    return CheckSendRecvResult(result, sizeof(*value));
  }

  // 获取socket文件描述符
  int socket() {
    return socket_.get();
  }

 private:
  // 检查发送/接收结果
  bool CheckSendRecvResult(int result, int expected_len) {
    if (result == -1) {
      last_error_ = errno;
    } else if (result != expected_len) {
      last_error_ = -1;  // 数据长度不匹配
    } else {
      last_error_ = 0;  // 成功
    }

    return last_error_ == 0;
  }

  ScopedFd socket_;    // socket文件描述符的智能指针
  int last_error_;     // 最后的错误码

  friend class SocketWriter;  // 允许SocketWriter访问私有成员
};

// Socket写入器类，用于构建和发送属性消息
class SocketWriter {
 public:
  // 构造函数，关联到属性服务连接
  explicit SocketWriter(PropertyServiceConnection* connection)
      : connection_(connection), iov_index_(0), uint_buf_index_(0) {
  }

  // 写入32位无符号整数
  SocketWriter& WriteUint32(uint32_t value) {
    CHECK(uint_buf_index_ < kUintBufSize);
    CHECK(iov_index_ < kIovSize);
    uint32_t* ptr = uint_buf_ + uint_buf_index_;
    uint_buf_[uint_buf_index_++] = value;     // 存储值到缓冲区
    iov_[iov_index_].iov_base = ptr;          // 设置IO向量基地址
    iov_[iov_index_].iov_len = sizeof(*ptr);  // 设置IO向量长度
    ++iov_index_;
    return *this;  // 返回自身以支持链式调用
  }

  // 写入字符串（先写长度，再写内容）
  SocketWriter& WriteString(const char* value) {
    uint32_t valuelen = strlen(value);
    WriteUint32(valuelen);  // 先写入字符串长度
    if (valuelen == 0) {
      return *this;  // 空字符串直接返回
    }

    CHECK(iov_index_ < kIovSize);
    iov_[iov_index_].iov_base = const_cast<char*>(value);  // 设置字符串地址
    iov_[iov_index_].iov_len = valuelen;                   // 设置字符串长度
    ++iov_index_;

    return *this;
  }

  // 发送所有缓冲的数据
  bool Send() {
    if (!connection_->IsValid()) {
      return false;
    }

    // 使用writev一次性发送所有IO向量
    if (writev(connection_->socket(), iov_, iov_index_) == -1) {
      connection_->last_error_ = errno;
      return false;
    }

    iov_index_ = uint_buf_index_ = 0;  // 重置索引，准备下次使用
    return true;
  }

 private:
  static constexpr size_t kUintBufSize = 8;  // 整数缓冲区大小
  static constexpr size_t kIovSize = 8;      // IO向量数组大小

  PropertyServiceConnection* connection_;  // 属性服务连接
  iovec iov_[kIovSize];                   // IO向量数组
  size_t iov_index_;                      // 当前IO向量索引
  uint32_t uint_buf_[kUintBufSize];       // 整数缓冲区
  size_t uint_buf_index_;                 // 当前整数缓冲区索引

  BIONIC_DISALLOW_IMPLICIT_CONSTRUCTORS(SocketWriter);  // 禁止隐式构造函数
};

// 属性消息结构，用于旧协议版本
struct prop_msg {
  unsigned cmd;                   // 命令类型
  char name[PROP_NAME_MAX];      // 属性名
  char value[PROP_VALUE_MAX];    // 属性值
};

// 发送属性消息到属性服务（使用旧协议）
static int send_prop_msg(const prop_msg* msg) {
  PropertyServiceConnection connection;
  if (!connection.IsValid()) {
    return connection.GetLastError();
  }

  int result = -1;
  int s = connection.socket();

  // 发送属性消息
  const int num_bytes = TEMP_FAILURE_RETRY(send(s, msg, sizeof(prop_msg), 0));
  if (num_bytes == sizeof(prop_msg)) {
    // 我们成功写入到属性服务器，但现在我们等待属性服务器完成其工作。
    // 它通过关闭socket来确认完成，所以我们在这里轮询（什么都不做），
    // 等待socket关闭。如果你执行'adb shell setprop foo bar'，
    // 一旦socket关闭，你会看到POLLHUP。出于谨慎，我们将轮询时间限制在250毫秒。
    pollfd pollfds[1];
    pollfds[0].fd = s;
    pollfds[0].events = 0;
    const int poll_result = TEMP_FAILURE_RETRY(poll(pollfds, 1, 250 /* ms */));
    if (poll_result == 1 && (pollfds[0].revents & POLLHUP) != 0) {
      result = 0;
    } else {
      // 忽略超时并将其视为成功。
      // init进程是单线程的，其属性服务有时响应缓慢
      // （也许它正在启动子进程或其他操作），因此这会超时，
      // 调用者认为它失败了，即使它仍在进行中。
      // 所以我们在这里伪造它，主要是为了ctl.*属性，
      // 但我们确实尝试等待250毫秒，这样执行读取后写入的调用者
      // 可以可靠地看到他们写入的内容。大多数时候。
      async_safe_format_log(ANDROID_LOG_WARN, "libc",
                            "Property service has timed out while trying to set \"%s\" to \"%s\"",
                            msg->name, msg->value);
      result = 0;
    }
  }

  return result;
}

// 协议版本常量
static constexpr uint32_t kProtocolVersion1 = 1;      // 旧版本协议
static constexpr uint32_t kProtocolVersion2 = 2;      // 当前版本协议

// 全局属性服务协议版本（原子变量）
static atomic_uint_least32_t g_propservice_protocol_version = 0;

// 检测属性服务协议版本
static void detect_protocol_version() {
  char value[PROP_VALUE_MAX];
  // 尝试获取服务版本属性
  if (__system_property_get(kServiceVersionPropertyName, value) == 0) {
    // 没有版本属性，使用旧协议
    g_propservice_protocol_version = kProtocolVersion1;
    async_safe_format_log(ANDROID_LOG_WARN, "libc",
                          "Using old property service protocol (\"%s\" is not set)",
                          kServiceVersionPropertyName);
  } else {
    // 解析版本号
    uint32_t version = static_cast<uint32_t>(atoll(value));
    if (version >= kProtocolVersion2) {
      g_propservice_protocol_version = kProtocolVersion2;
    } else {
      // 版本号太低，使用旧协议
      async_safe_format_log(ANDROID_LOG_WARN, "libc",
                            "Using old property service protocol (\"%s\"=\"%s\")",
                            kServiceVersionPropertyName, value);
      g_propservice_protocol_version = kProtocolVersion1;
    }
  }
}

// 系统属性设置函数（bionic弱符号，用于native bridge）
__BIONIC_WEAK_FOR_NATIVE_BRIDGE
int __system_property_set(const char* key, const char* value) {
  if (key == nullptr) return -1;   // 键不能为空
  if (value == nullptr) value = "";  // 值为空时设为空字符串

  // 如果协议版本未检测，先检测版本
  if (g_propservice_protocol_version == 0) {
    detect_protocol_version();
  }

  if (g_propservice_protocol_version == kProtocolVersion1) {
    // 旧协议不支持长名称或长值
    if (strlen(key) >= PROP_NAME_MAX) return -1;
    if (strlen(value) >= PROP_VALUE_MAX) return -1;

    // 构造并发送属性消息
    prop_msg msg;
    memset(&msg, 0, sizeof msg);
    msg.cmd = PROP_MSG_SETPROP;
    strlcpy(msg.name, key, sizeof msg.name);
    strlcpy(msg.value, value, sizeof msg.value);

    return send_prop_msg(&msg);
  } else {
    // 新协议只允许ro.属性使用长值
    if (strlen(value) >= PROP_VALUE_MAX && strncmp(key, "ro.", 3) != 0) return -1;
    
    // 使用新协议
    PropertyServiceConnection connection;
    if (!connection.IsValid()) {
      errno = connection.GetLastError();
      async_safe_format_log(
          ANDROID_LOG_WARN, "libc",
          "Unable to set property \"%s\" to \"%s\": connection failed; errno=%d (%s)", key, value,
          errno, strerror(errno));
      return -1;
    }

    // 使用SocketWriter发送属性设置请求
    SocketWriter writer(&connection);
    if (!writer.WriteUint32(PROP_MSG_SETPROP2).WriteString(key).WriteString(value).Send()) {
      errno = connection.GetLastError();
      async_safe_format_log(ANDROID_LOG_WARN, "libc",
                            "Unable to set property \"%s\" to \"%s\": write failed; errno=%d (%s)",
                            key, value, errno, strerror(errno));
      return -1;
    }

    // 接收服务器响应
    int result = -1;
    if (!connection.RecvInt32(&result)) {
      errno = connection.GetLastError();
      async_safe_format_log(ANDROID_LOG_WARN, "libc",
                            "Unable to set property \"%s\" to \"%s\": recv failed; errno=%d (%s)",
                            key, value, errno, strerror(errno));
      return -1;
    }

    // 检查服务器返回的结果
    if (result != PROP_SUCCESS) {
      async_safe_format_log(ANDROID_LOG_WARN, "libc",
                            "Unable to set property \"%s\" to \"%s\": error code: 0x%x", key, value,
                            result);
      return -1;
    }

    return 0;  // 设置成功
  }
}
