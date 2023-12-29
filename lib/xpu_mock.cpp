#include "xpu_mock.h"

#include <Python.h>
#include <dlfcn.h>  // dladdr
#include <execinfo.h>
#include <frameobject.h>

#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

#include "backtrace.h"
#include "hook.h"
#include "logger/logger.h"
#include "support.h"

namespace {

class XpuRuntimeApiHook;

static constexpr int kMaxXpuDeviceNum = 8;

class XpuRuntimeWrapApi {
   public:
    static XpuRuntimeWrapApi& instance();
    XpuRuntimeWrapApi();
    static int xpuMalloc(void** pDevPtr, uint64_t size, int kind);
    static int xpuFree(void* devPtr);
    static int xpuWait(void* devStream);
    static int xpuMemcpy(void* dst, const void* src, uint64_t size, int kind);
    static int xpuSetDevice(int devId);

   private:
    int (*raw_xpu_malloc_)(void**, uint64_t, int){nullptr};
    int (*raw_xpu_free_)(void*){nullptr};
    int (*raw_xpu_current_device_)(int*){nullptr};
    int (*raw_xpu_wait_)(void*){nullptr};
    int (*raw_xpu_memcpy_)(void*, const void*, uint64_t, int){nullptr};
    decltype(&xpuSetDevice) raw_xpu_set_device_id_{nullptr};

    enum class XpuMemKind { GLOBAL_MEMORY = 0, L3_MEMORY };

    struct XpuDataPtr {
        void* data_ptr;
        uint64_t size;
        XpuMemKind kind;
    };

    std::mutex memory_api_mutex_;
    std::vector<std::map<void*, XpuDataPtr>> allocated_ptr_map_;
    std::vector<uint64_t> allocated_gm_size_;
    std::vector<uint64_t> allocated_l3_size_;
    std::vector<uint64_t> peak_gm_size_;
    std::vector<uint64_t> peak_l3_size_;

    friend class XpuRuntimeApiHook;
};

XpuRuntimeWrapApi& XpuRuntimeWrapApi::instance() {
    static XpuRuntimeWrapApi instance;
    return instance;
}

XpuRuntimeWrapApi::XpuRuntimeWrapApi()
    : allocated_ptr_map_(kMaxXpuDeviceNum),
      allocated_gm_size_(kMaxXpuDeviceNum, 0),
      allocated_l3_size_(kMaxXpuDeviceNum, 0),
      peak_gm_size_(kMaxXpuDeviceNum, 0),
      peak_l3_size_(kMaxXpuDeviceNum, 0) {}

int XpuRuntimeWrapApi::xpuMalloc(void** pDevPtr, uint64_t size, int kind) {
    int r = 0;
    int devId = 0;

    CHECK(XpuRuntimeWrapApi::instance().raw_xpu_current_device_,
          "xpu_current_device not binded");
    CHECK(XpuRuntimeWrapApi::instance().raw_xpu_malloc_, "xpu_free not binded");

    // make malloc/free sequential to obtain a trusted memory usage footprint
    std::lock_guard<std::mutex> lock(
        XpuRuntimeWrapApi::instance().memory_api_mutex_);

    r = XpuRuntimeWrapApi::instance().raw_xpu_current_device_(&devId);
    if (r != 0) {
        return r;
    }
    CHECK_LT(devId, kMaxXpuDeviceNum,
             "devId({}) must less than kMaxXpuDeviceNum({})", devId,
             kMaxXpuDeviceNum);

    r = XpuRuntimeWrapApi::instance().raw_xpu_malloc_(pDevPtr, size, kind);
    if (r != 0) {
        LOG(WARN) << "[XpuRuntimeWrapApi xpuMalloc][failed] "
                  << "devId=" << devId << ","
                  << "size=" << size << ","
                  << "kind=" << kind << ","
                  << "gm_allocated="
                  << XpuRuntimeWrapApi::instance().allocated_gm_size_[devId]
                  << ","
                  << "gm_peak="
                  << XpuRuntimeWrapApi::instance().peak_gm_size_[devId];
        return r;
    }

    if (kind == (int)XpuMemKind::GLOBAL_MEMORY) {
        XpuRuntimeWrapApi::instance().allocated_gm_size_[devId] += size;
        XpuRuntimeWrapApi::instance().peak_gm_size_[devId] =
            std::max(XpuRuntimeWrapApi::instance().peak_gm_size_[devId],
                     XpuRuntimeWrapApi::instance().allocated_gm_size_[devId]);
    } else if (kind == (int)XpuMemKind::L3_MEMORY) {
        XpuRuntimeWrapApi::instance().allocated_l3_size_[devId] += size;
        XpuRuntimeWrapApi::instance().peak_l3_size_[devId] =
            std::max(XpuRuntimeWrapApi::instance().peak_l3_size_[devId],
                     XpuRuntimeWrapApi::instance().allocated_l3_size_[devId]);
    }

    XpuRuntimeWrapApi::instance().allocated_ptr_map_[devId][*pDevPtr] = {
        *pDevPtr, size, (XpuMemKind)kind};

    LOG(WARN) << "[XpuRuntimeWrapApi xpuMalloc][success] "
              << "devId=" << devId << ","
              << "size=" << size << ","
              << "kind=" << kind << ","
              << "gm_allocated="
              << XpuRuntimeWrapApi::instance().allocated_gm_size_[devId] << ","
              << "gm_peak="
              << XpuRuntimeWrapApi::instance().peak_gm_size_[devId];

    return r;
}

int XpuRuntimeWrapApi::xpuFree(void* devPtr) {
    int r = 0;
    int devId = 0;

    CHECK(XpuRuntimeWrapApi::instance().raw_xpu_current_device_,
          "xpu_current_device not binded");
    CHECK(XpuRuntimeWrapApi::instance().raw_xpu_free_, "xpu_free not binded");

    // make malloc/free sequential to obtain a trusted memory usage footprint
    std::lock_guard<std::mutex> lock(
        XpuRuntimeWrapApi::instance().memory_api_mutex_);

    r = XpuRuntimeWrapApi::instance().raw_xpu_current_device_(&devId);
    if (r != 0) {
        return r;
    }
    CHECK_LT(devId, kMaxXpuDeviceNum,
             "devId({}) must less than kMaxXpuDeviceNum({})", devId,
             kMaxXpuDeviceNum);

    r = XpuRuntimeWrapApi::instance().raw_xpu_free_(devPtr);
    if (r != 0) {
        return r;
    }

    auto it =
        XpuRuntimeWrapApi::instance().allocated_ptr_map_[devId].find(devPtr);
    if (it == XpuRuntimeWrapApi::instance().allocated_ptr_map_[devId].end()) {
        return r;
    }

    XpuDataPtr dataPtr = it->second;

    if (dataPtr.kind == XpuMemKind::GLOBAL_MEMORY) {
        XpuRuntimeWrapApi::instance().allocated_gm_size_[devId] -= dataPtr.size;
    } else if (dataPtr.kind == XpuMemKind::L3_MEMORY) {
        XpuRuntimeWrapApi::instance().allocated_l3_size_[devId] -= dataPtr.size;
    }

    XpuRuntimeWrapApi::instance().allocated_ptr_map_[devId].erase(it);
    return r;
}

int XpuRuntimeWrapApi::xpuWait(void* devStream) {
    constexpr int kMaxStackDeep = 512;
    void* call_stack[kMaxStackDeep] = {0};
    char** symbols = nullptr;
    bool backtrace_ret = true;
    do {
        int num = backtrace(call_stack, kMaxStackDeep);
        CHECK(num > 0, "Expect frams num {} > 0!", num);
        CHECK(num <= kMaxStackDeep, "Expect frams num {} <= 512!", num);
        symbols = backtrace_symbols(call_stack, num);
        if (symbols == nullptr) {
            backtrace_ret = false;
            break;
        }

        LOG(WARN) << "[XpuRuntimeWrapApi xpuWait]"
                  << "get stack deep num:" << num;
        Dl_info info;
        for (int j = 0; j < num; j++) {
            if (dladdr(call_stack[j], &info) && info.dli_sname) {
                auto demangled = __support__demangle(info.dli_sname);
                std::string path(info.dli_fname);
                LOG(WARN) << "    frame " << j << path << ":" << demangled;
            } else {
                // filtering useless print
                // LOG(WARN) << "    frame " << j << call_stack[j];
            }
        }
        free(symbols);
    } while (0);
    if (!backtrace_ret) {
        LOG(WARN) << "collect native backtrace fail!";
    }
    return XpuRuntimeWrapApi::instance().raw_xpu_wait_(devStream);
}

int XpuRuntimeWrapApi::xpuMemcpy(void* dst, const void* src, uint64_t size,
                                 int kind) {
    LOG(WARN) << "[XpuRuntimeWrapApi xpuMemcpy]"
              << "entering PyGILState_Ensure";

    // Acquire the Global Interpreter Lock (GIL) before calling Python C API
    // functions from non-Python threads.
    PyGILState_STATE gstate = PyGILState_Ensure();

    LOG(WARN) << "[XpuRuntimeWrapApi xpuMemcpy]"
              << "after PyGILState_Ensure";
    LOG(WARN) << "[XpuRuntimeWrapApi xpuMemcpy]"
              << "Python stack trace:";
    // https://stackoverflow.com/questions/1796510/accessing-a-python-traceback-from-the-c-api
    PyThreadState* tstate = PyThreadState_GET();
    if (NULL != tstate && NULL != tstate->frame) {
        PyFrameObject* frame = tstate->frame;

        while (NULL != frame) {
            // int line = frame->f_lineno;
            /*
            frame->f_lineno will not always return the correct line number
            you need to call PyCode_Addr2Line().
            */
            int line = PyCode_Addr2Line(frame->f_code, frame->f_lasti);
            const char* filename = PyUnicode_AsUTF8(frame->f_code->co_filename);
            const char* funcname = PyUnicode_AsUTF8(frame->f_code->co_name);
            LOG(WARN) << "    " << filename << "(" << line << "): " << funcname;
            frame = frame->f_back;
        }
    }
    PyGILState_Release(gstate);
    return XpuRuntimeWrapApi::instance().raw_xpu_memcpy_(dst, src, size, kind);
}

int XpuRuntimeWrapApi::xpuSetDevice(int devId) {
    trace::CallFrames callFrames;
    callFrames.CollectNative();
    callFrames.CollectPython();
    LOG(WARN) << __func__ << " with frame:\n" << callFrames;
    return XpuRuntimeWrapApi::instance().raw_xpu_set_device_id_(devId);
}

struct XpuRuntimeApiHook : public hook::HookInstallerWrap<XpuRuntimeApiHook> {
    bool targetLib(const char* name) {
        return !strstr(name, "libxpurt.so.1") && !strstr(name, "libxpurt.so");
    }

    std::tuple<const char*, void*, void**> symbols[6] = {
        // malloc
        {"xpu_malloc", reinterpret_cast<void*>(&XpuRuntimeWrapApi::xpuMalloc),
         reinterpret_cast<void**>(
             &XpuRuntimeWrapApi::instance().raw_xpu_malloc_)},
        // free
        {"xpu_free", reinterpret_cast<void*>(&XpuRuntimeWrapApi::xpuFree),
         reinterpret_cast<void**>(
             &XpuRuntimeWrapApi::instance().raw_xpu_free_)},
        // get device id
        {"xpu_current_device",
         reinterpret_cast<void*>(
             XpuRuntimeWrapApi::instance().raw_xpu_current_device_),
         reinterpret_cast<void**>(
             &XpuRuntimeWrapApi::instance().raw_xpu_current_device_)},
        // sync device
        {"xpu_wait", reinterpret_cast<void*>(&XpuRuntimeWrapApi::xpuWait),
         reinterpret_cast<void**>(
             &XpuRuntimeWrapApi::instance().raw_xpu_wait_)},
        // memcpy
        {"xpu_memcpy", reinterpret_cast<void*>(&XpuRuntimeWrapApi::xpuMemcpy),
         reinterpret_cast<void**>(
             &XpuRuntimeWrapApi::instance().raw_xpu_memcpy_)},
        // set_device
        {"xpu_set_device",
         reinterpret_cast<void*>(&XpuRuntimeWrapApi::xpuSetDevice),
         reinterpret_cast<void**>(
             &XpuRuntimeWrapApi::instance().raw_xpu_set_device_id_)}};

    void onSuccess() {}
};

}  // namespace

extern "C" {

void xpu_dh_initialize() {
    static auto install_wrap = std::make_shared<XpuRuntimeApiHook>();
    install_wrap->install();
}
}