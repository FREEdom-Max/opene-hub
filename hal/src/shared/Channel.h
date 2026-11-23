#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>

// 笔与键盘共用的通道实现。两者的厂商 DLL 调用模型相同，产出的数据形状也相同：一份可重复
// 读的状态快照，加一串一次性的边沿事件。抽在这里而不是各写一份，是因为 seqlock 的读写
// 配对很容易在复制粘贴时改错一侧，而那种错误只在并发下偶发。
namespace Gaokun::Channel {

inline constexpr uint32_t kAbiVersion = 1;
inline constexpr wchar_t kAccessoryChangedEventName[] = L"Local\\OpenEHubAccessoryChanged";

// 共享内存的线路格式。写者在 ARM64EC 的宿主里，读者在原生 ARM64 的调用方里，两侧都实例化
// 同一个模板，因此只用固定宽度类型，不依赖任一侧编译器的填充规则。
template <typename Payload>
struct SharedBlock {
    uint32_t abiVersion;

    // seqlock：写入期间为奇数。
    //
    // 必须是 std::atomic 而不是 volatile。volatile 不对周围的普通存储排序，而
    // atomic_thread_fence 只排序原子操作；本机是 ARM64，弱序模型下 payload 的写入可能先于
    // 奇数计数变得可见，读者就会接受一份写到一半的快照。
    std::atomic<uint32_t> sequence;

    Payload payload;
};

// 允许任何用户读取。快照里只有电量、版本号一类信息，而读者是用户会话里的界面进程，
// 拿不到也不需要服务账户的令牌。
[[nodiscard]] inline bool BuildSharedSecurity(SECURITY_ATTRIBUTES &sa,
                                              SECURITY_DESCRIPTOR &sd) noexcept {
#if defined(_M_X64) && !defined(_M_ARM64EC)
    // The x64 UI and both accessory hosts run as the same user.
    (void)sa;
    (void)sd;
    return false; // Caller uses the process token's default DACL.
#else
    if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION)) return false;
    if (!SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE)) return false;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;
    return true;
#endif
}

// Global\ 需要 SeCreateGlobalPrivilege，服务账户具备；手工运行宿主时退到会话本地名，
// 这样不装服务也能试。
[[nodiscard]] inline std::wstring LocalName(const wchar_t *globalName) {
    const std::wstring full(globalName);
    const size_t slash = full.find(L'\\');
    return slash == std::wstring::npos ? full : full.substr(slash + 1);
}

template <typename Payload>
class SeqlockWriter {
public:
    SeqlockWriter() noexcept = default;
    ~SeqlockWriter() noexcept {
        if (m_view) UnmapViewOfFile(m_view);
        if (m_mapping) CloseHandle(m_mapping);
    }

    SeqlockWriter(const SeqlockWriter &) = delete;
    SeqlockWriter &operator=(const SeqlockWriter &) = delete;

    [[nodiscard]] bool Open(const wchar_t *name) noexcept {
        using Block = SharedBlock<Payload>;

        SECURITY_ATTRIBUTES sa{};
        SECURITY_DESCRIPTOR sd{};
        const bool haveSecurity = BuildSharedSecurity(sa, sd);

        HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, haveSecurity ? &sa : nullptr,
                                            PAGE_READWRITE, 0, sizeof(Block), name);
        if (!mapping) {
            const std::wstring local = LocalName(name);
            mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, haveSecurity ? &sa : nullptr,
                                         PAGE_READWRITE, 0, sizeof(Block), local.c_str());
        }
        if (!mapping) return false;

        void *view = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(Block));
        if (!view) {
            CloseHandle(mapping);
            return false;
        }

        auto *block = static_cast<Block *>(view);
        block->abiVersion = kAbiVersion;
        block->sequence.store(0, std::memory_order_release);

        m_mapping = mapping;
        m_view = view;
        return true;
    }

    void Publish(const Payload &payload) noexcept {
        if (!m_view) return;
        auto *block = static_cast<SharedBlock<Payload> *>(m_view);

        const uint32_t start = block->sequence.load(std::memory_order_relaxed);
        block->sequence.store(start + 1, std::memory_order_release); // 奇数：写入中
        std::atomic_thread_fence(std::memory_order_release);

        block->payload = payload;

        std::atomic_thread_fence(std::memory_order_release);
        block->sequence.store(start + 2, std::memory_order_release); // 偶数：可读
    }

private:
    HANDLE m_mapping = nullptr;
    void *m_view = nullptr;
};

template <typename Payload>
class SeqlockReader {
public:
    SeqlockReader() noexcept = default;
    ~SeqlockReader() noexcept {
        if (m_view) UnmapViewOfFile(m_view);
        if (m_mapping) CloseHandle(m_mapping);
    }

    SeqlockReader(const SeqlockReader &) = delete;
    SeqlockReader &operator=(const SeqlockReader &) = delete;

    [[nodiscard]] bool Open(const wchar_t *name) noexcept {
        using Block = SharedBlock<Payload>;

        HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
        if (!mapping) {
            const std::wstring local = LocalName(name);
            mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, local.c_str());
        }
        if (!mapping) return false;

        void *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(Block));
        if (!view) {
            CloseHandle(mapping);
            return false;
        }

        m_mapping = mapping;
        m_view = view;
        return true;
    }

    [[nodiscard]] bool Read(Payload &out) const noexcept {
        if (!m_view) return false;
        const auto *block = static_cast<const SharedBlock<Payload> *>(m_view);
        if (block->abiVersion != kAbiVersion) return false;

        // 前后各读一次计数，相同且为偶数才说明这份拷贝没有跨越一次写入。重试有限次，
        // 免得写者异常时读者在这里空转。
        for (int attempt = 0; attempt < 16; ++attempt) {
            const uint32_t before = block->sequence.load(std::memory_order_acquire);
            if (before & 1u) continue;

            std::atomic_thread_fence(std::memory_order_acquire);
            const Payload copy = block->payload;
            std::atomic_thread_fence(std::memory_order_acquire);

            if (block->sequence.load(std::memory_order_acquire) == before) {
                out = copy;
                return true;
            }
        }
        return false;
    }

private:
    HANDLE m_mapping = nullptr;
    void *m_view = nullptr;
};

// 事件管道。记录是定长的，笔与键盘的事件结构大小相同，所以这里只按字节数处理。
template <typename Event>
class EventPipeWriter {
public:
    EventPipeWriter() noexcept = default;
    ~EventPipeWriter() noexcept {
        m_closing = true;
        CloseClient();
        if (m_pipe && m_pipe != INVALID_HANDLE_VALUE) {
            (void)CancelIo(m_pipe);
            CloseHandle(m_pipe);
        }
        if (m_connectEvent) CloseHandle(m_connectEvent);
        if (m_writeEvent) CloseHandle(m_writeEvent);
    }

    EventPipeWriter(const EventPipeWriter &) = delete;
    EventPipeWriter &operator=(const EventPipeWriter &) = delete;

    [[nodiscard]] bool Open(const wchar_t *name) noexcept {
        SECURITY_ATTRIBUTES sa{};
        SECURITY_DESCRIPTOR sd{};
        const bool haveSecurity = BuildSharedSecurity(sa, sd);

        HANDLE pipe = CreateNamedPipeW(
            name, PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1, sizeof(Event) * 64, sizeof(Event) * 64, 0,
            haveSecurity ? &sa : nullptr);
        if (pipe == INVALID_HANDLE_VALUE) return false;

        m_connectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_writeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!m_connectEvent || !m_writeEvent) {
            CloseHandle(pipe);
            if (m_connectEvent) { CloseHandle(m_connectEvent); m_connectEvent = nullptr; }
            if (m_writeEvent) { CloseHandle(m_writeEvent); m_writeEvent = nullptr; }
            return false;
        }
        m_pipe = pipe;
        StartConnect();
        return true;
    }

    [[nodiscard]] HANDLE WaitHandle() const noexcept { return m_connectEvent; }

    void PollForReader() noexcept {
        if (!m_pipe || m_pipe == INVALID_HANDLE_VALUE || m_connected) return;
        if (m_connecting) {
            DWORD transferred = 0;
            if (GetOverlappedResult(m_pipe, &m_connectOv, &transferred, FALSE)) {
                m_connecting = false;
                m_connected = true;
                (void)ResetEvent(m_connectEvent);
                return;
            }
            if (GetLastError() == ERROR_IO_INCOMPLETE) return;
            m_connecting = false;
            (void)ResetEvent(m_connectEvent);
        }
        StartConnect();
    }

    bool Send(const Event &event) noexcept {
        if (!m_pipe || m_pipe == INVALID_HANDLE_VALUE || !m_connected) return false;
        OVERLAPPED ov{};
        ov.hEvent = m_writeEvent;
        (void)ResetEvent(m_writeEvent);

        DWORD written = 0;
        if (WriteFile(m_pipe, &event, sizeof(event), &written, &ov)) {
            if (written == sizeof(event)) return true;
            CloseClient();
            return false;
        }
        if (GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(m_writeEvent, 50) == WAIT_OBJECT_0 &&
                GetOverlappedResult(m_pipe, &ov, &written, FALSE) &&
                written == sizeof(event)) {
                return true;
            }
            (void)CancelIoEx(m_pipe, &ov);
            DWORD ignored = 0;
            (void)GetOverlappedResult(m_pipe, &ov, &ignored, TRUE);
        }
        // 读者断开了。回到监听态，好让下一个读者能连上，而不是从此静默。
        CloseClient();
        return false;
    }

private:
    void StartConnect() noexcept {
        if (m_closing || !m_pipe || m_pipe == INVALID_HANDLE_VALUE || m_connected || m_connecting) {
            return;
        }
        (void)ResetEvent(m_connectEvent);
        m_connectOv = {};
        m_connectOv.hEvent = m_connectEvent;
        if (ConnectNamedPipe(m_pipe, &m_connectOv)) {
            m_connected = true;
            return;
        }
        const DWORD err = GetLastError();
        if (err == ERROR_PIPE_CONNECTED) {
            m_connected = true;
        } else if (err == ERROR_IO_PENDING) {
            m_connecting = true;
        }
    }

    void CloseClient() noexcept {
        if (m_pipe && m_pipe != INVALID_HANDLE_VALUE) {
            if (m_connecting) {
                (void)CancelIoEx(m_pipe, &m_connectOv);
                DWORD ignored = 0;
                (void)GetOverlappedResult(m_pipe, &m_connectOv, &ignored, TRUE);
                m_connecting = false;
            }
            if (m_connected) {
                (void)DisconnectNamedPipe(m_pipe);
                m_connected = false;
            }
        }
        if (m_connectEvent) (void)ResetEvent(m_connectEvent);
        if (!m_closing) StartConnect();
    }

    HANDLE m_pipe = nullptr;
    HANDLE m_connectEvent = nullptr;
    HANDLE m_writeEvent = nullptr;
    OVERLAPPED m_connectOv{};
    bool m_connected = false;
    bool m_connecting = false;
    bool m_closing = false;
};

template <typename Event>
class EventPipeReader {
public:
    EventPipeReader() noexcept = default;
    ~EventPipeReader() noexcept {
        if (m_pipe && m_pipe != INVALID_HANDLE_VALUE) CloseHandle(m_pipe);
    }

    EventPipeReader(const EventPipeReader &) = delete;
    EventPipeReader &operator=(const EventPipeReader &) = delete;

    [[nodiscard]] bool Open(const wchar_t *name) noexcept {
        HANDLE pipe = CreateFileW(name, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) return false;

        DWORD mode = PIPE_READMODE_MESSAGE | PIPE_NOWAIT;
        if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
            CloseHandle(pipe);
            return false;
        }
        m_pipe = pipe;
        return true;
    }

    [[nodiscard]] bool Poll(Event &out) noexcept {
        if (!m_pipe || m_pipe == INVALID_HANDLE_VALUE) return false;
        DWORD read = 0;
        return ReadFile(m_pipe, &out, sizeof(out), &read, nullptr) && read == sizeof(out);
    }

private:
    HANDLE m_pipe = nullptr;
};

// 下行命令管道。宿主是服务端读者，控制进程是客户端写者；与上面的事件管道方向相反。
// 采用重叠 I/O，宿主主循环可以直接阻塞在 WaitHandle() 上，无需定时轮询。
template <typename Command>
class CommandPipeReader {
public:
    CommandPipeReader() noexcept = default;
    ~CommandPipeReader() noexcept {
        m_closing = true;
        CloseClient();
        if (m_pipe && m_pipe != INVALID_HANDLE_VALUE) {
            (void)CancelIo(m_pipe);
            CloseHandle(m_pipe);
        }
        if (m_event) CloseHandle(m_event);
    }

    CommandPipeReader(const CommandPipeReader &) = delete;
    CommandPipeReader &operator=(const CommandPipeReader &) = delete;

    [[nodiscard]] bool Open(const wchar_t *name) noexcept {
        SECURITY_ATTRIBUTES sa{};
        SECURITY_DESCRIPTOR sd{};
        const bool haveSecurity = BuildSharedSecurity(sa, sd);

        HANDLE pipe = CreateNamedPipeW(
            name, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1, sizeof(Command) * 16, sizeof(Command) * 16, 0,
            haveSecurity ? &sa : nullptr);
        if (pipe == INVALID_HANDLE_VALUE) return false;

        m_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!m_event) {
            CloseHandle(pipe);
            return false;
        }
        m_pipe = pipe;
        StartConnect();
        return true;
    }

    [[nodiscard]] HANDLE WaitHandle() const noexcept { return m_event; }

    void PollForWriter() noexcept {
        if (!m_pipe || m_pipe == INVALID_HANDLE_VALUE) return;
        if (m_state == State::Connecting) {
            DWORD transferred = 0;
            if (GetOverlappedResult(m_pipe, &m_ov, &transferred, FALSE)) {
                (void)ResetEvent(m_event);
                m_state = State::ConnectedIdle;
                StartRead();
                return;
            }
            if (GetLastError() == ERROR_IO_INCOMPLETE) return;
            CloseClient();
            return;
        }
        if (m_state == State::Disconnected) StartConnect();
    }

    [[nodiscard]] bool Poll(Command &out) noexcept {
        if (!m_pipe || m_pipe == INVALID_HANDLE_VALUE) return false;
        if (m_hasReady) {
            out = m_pending;
            m_hasReady = false;
            (void)ResetEvent(m_event);
            StartRead();
            return true;
        }
        if (m_state != State::Reading) return false;
        DWORD read = 0;
        if (GetOverlappedResult(m_pipe, &m_ov, &read, FALSE)) {
            (void)ResetEvent(m_event);
            m_state = State::ConnectedIdle;
            if (read == sizeof(Command)) {
                out = m_pending;
                StartRead();
                return true;
            }
            StartRead();
            return false;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_IO_INCOMPLETE) return false;
        CloseClient();
        return false;
    }

private:
    enum class State { Disconnected, Connecting, ConnectedIdle, Reading };

    void StartConnect() noexcept {
        if (m_closing || !m_pipe || m_pipe == INVALID_HANDLE_VALUE) return;
        (void)ResetEvent(m_event);
        m_ov = {};
        m_ov.hEvent = m_event;
        if (ConnectNamedPipe(m_pipe, &m_ov)) {
            m_state = State::ConnectedIdle;
            StartRead();
            return;
        }
        const DWORD err = GetLastError();
        if (err == ERROR_PIPE_CONNECTED) {
            m_state = State::ConnectedIdle;
            StartRead();
        } else if (err == ERROR_IO_PENDING) {
            m_state = State::Connecting;
        } else {
            m_state = State::Disconnected;
        }
    }

    void StartRead() noexcept {
        if (m_closing || !m_pipe || m_pipe == INVALID_HANDLE_VALUE) return;
        (void)ResetEvent(m_event);
        m_ov = {};
        m_ov.hEvent = m_event;
        DWORD read = 0;
        if (ReadFile(m_pipe, &m_pending, sizeof(Command), &read, &m_ov)) {
            if (read == sizeof(Command)) {
                m_hasReady = true;
                m_state = State::ConnectedIdle;
                (void)SetEvent(m_event);
                return;
            }
            CloseClient();
            return;
        }
        const DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            m_state = State::Reading;
        } else {
            CloseClient();
        }
    }

    void CloseClient() noexcept {
        if (m_pipe && m_pipe != INVALID_HANDLE_VALUE) {
            if (m_state == State::Connecting || m_state == State::Reading) {
                (void)CancelIoEx(m_pipe, &m_ov);
                DWORD ignored = 0;
                (void)GetOverlappedResult(m_pipe, &m_ov, &ignored, TRUE);
            }
            if (m_state == State::ConnectedIdle || m_state == State::Reading) {
                (void)DisconnectNamedPipe(m_pipe);
            }
        }
        m_state = State::Disconnected;
        m_hasReady = false;
        if (m_event) (void)ResetEvent(m_event);
        if (!m_closing) StartConnect();
    }

    HANDLE m_pipe = nullptr;
    HANDLE m_event = nullptr;
    OVERLAPPED m_ov{};
    Command m_pending{};
    State m_state = State::Disconnected;
    bool m_hasReady = false;
    bool m_closing = false;
};

template <typename Command>
class CommandPipeWriter {
public:
    CommandPipeWriter() noexcept = default;
    ~CommandPipeWriter() noexcept {
        if (m_pipe && m_pipe != INVALID_HANDLE_VALUE) CloseHandle(m_pipe);
    }

    CommandPipeWriter(const CommandPipeWriter &) = delete;
    CommandPipeWriter &operator=(const CommandPipeWriter &) = delete;

    [[nodiscard]] bool Open(const wchar_t *name) noexcept {
        HANDLE pipe = CreateFileW(name, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) return false;
        m_pipe = pipe;
        return true;
    }

    [[nodiscard]] bool Send(const Command &command) noexcept {
        if (!m_pipe || m_pipe == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        return WriteFile(m_pipe, &command, sizeof(command), &written, nullptr) &&
               written == sizeof(command);
    }

private:
    HANDLE m_pipe = nullptr;
};

} // namespace Gaokun::Channel
