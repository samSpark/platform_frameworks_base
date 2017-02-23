/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "JNIHelp.h"
#include "core_jni_helpers.h"
#include "JniConstants.h"
#include "utils/Log.h"
#include "utils/misc.h"

#if defined __arm__ || defined __aarch64__

#include <vector>

#include <sys/prctl.h>

#include <linux/unistd.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>

#include "seccomp_policy.h"

#define syscall_nr (offsetof(struct seccomp_data, nr))
#define arch_nr (offsetof(struct seccomp_data, arch))

typedef std::vector<sock_filter> filter;

// We want to keep the below inline functions for debugging and future
// development even though they are not all sed currently.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"

static inline void Kill(filter& f) {
    f.push_back(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_KILL));
}

static inline void Trap(filter& f) {
    f.push_back(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP));
}

static inline void Error(filter& f, __u16 retcode) {
    f.push_back(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ERRNO + retcode));
}

inline static void Trace(filter& f) {
    f.push_back(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRACE));
}

inline static void Allow(filter& f) {
    f.push_back(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW));
}

#pragma clang diagnostic pop

inline static void ExamineSyscall(filter& f) {
    f.push_back(BPF_STMT(BPF_LD|BPF_W|BPF_ABS, syscall_nr));
}

inline static int SetValidateArchitectureJumpTarget(size_t offset, filter& f) {
    size_t jump_length = f.size() - offset - 1;
    auto u8_jump_length = (__u8) jump_length;
    if (u8_jump_length != jump_length) {
        ALOGE("Can't set jump greater than 255 - actual jump is %zu",
              jump_length);
        return -1;
    }
    f[offset] = BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, AUDIT_ARCH_ARM, u8_jump_length, 0);
    return 0;
}

inline static size_t ValidateArchitectureAndJumpIfNeeded(filter& f) {
    f.push_back(BPF_STMT(BPF_LD|BPF_W|BPF_ABS, arch_nr));

    f.push_back(BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, AUDIT_ARCH_AARCH64, 2, 0));
    f.push_back(BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, AUDIT_ARCH_ARM, 1, 0));
    Trap(f);
    return f.size() - 2;
}

static bool install_filter(filter const& f) {
    struct sock_fprog prog = {
        (unsigned short) f.size(),
        (struct sock_filter*) &f[0],
    };

    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) < 0) {
        ALOGE("SECCOMP: Could not set seccomp filter of size %zu: %s", f.size(), strerror(errno));
        return false;
    }

    ALOGI("SECCOMP: Global filter of size %zu installed", f.size());
    return true;
}

bool set_seccomp_filter() {
    filter f;

    // Note that for mixed 64/32 bit architectures, ValidateArchitecture inserts a
    // jump that must be changed to point to the start of the 32-bit policy
    // 32 bit syscalls will not hit the policy between here and the call to SetJump
    auto offset_to_32bit_filter =
        ValidateArchitectureAndJumpIfNeeded(f);

    // 64-bit filter
    ExamineSyscall(f);

    // arm64-only filter - autogenerated from bionic syscall usage
    for (size_t i = 0; i < arm64_filter_size; ++i)
        f.push_back(arm64_filter[i]);
    Trap(f);

    if (SetValidateArchitectureJumpTarget(offset_to_32bit_filter, f) != 0)
        return -1;

    // 32-bit filter
    ExamineSyscall(f);

    // arm32 filter - autogenerated from bionic syscall usage
    for (size_t i = 0; i < arm_filter_size; ++i)
        f.push_back(arm_filter[i]);
    Trap(f);

    return install_filter(f);
}

static void Seccomp_setPolicy(JNIEnv* /*env*/) {
    if (!set_seccomp_filter()) {
        ALOGE("Failed to set seccomp policy - killing");
        exit(1);
    }
}

#else // #if defined __arm__ || defined __aarch64__

static void Seccomp_setPolicy(JNIEnv* /*env*/) {
}

#endif

static const JNINativeMethod method_table[] = {
    NATIVE_METHOD(Seccomp, setPolicy, "()V"),
};

namespace android {

int register_android_os_seccomp(JNIEnv* env) {
    return android::RegisterMethodsOrDie(env, "android/os/Seccomp",
                                         method_table, NELEM(method_table));
}

}
