#pragma once

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace fluxcap::test_support {

inline constexpr std::size_t child_process_max_wire_message_bytes = 4 * 1'024;
inline constexpr DWORD child_process_pipe_buffer_bytes = 64 * 1'024;

class UniqueHandle final {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE value) noexcept : value_(value) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : value_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }

    [[nodiscard]] HANDLE release() noexcept {
        return std::exchange(value_, nullptr);
    }

    void reset(HANDLE value = nullptr) noexcept {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
        value_ = value;
    }

private:
    HANDLE value_ = nullptr;
};

inline bool read_exact(HANDLE pipe, void* output, std::size_t size) noexcept {
    auto* cursor = static_cast<std::uint8_t*>(output);
    while (size != 0) {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
            size, std::numeric_limits<DWORD>::max()));
        if (!ReadFile(pipe, cursor, chunk, &read, nullptr) || read == 0) {
            return false;
        }
        cursor += read;
        size -= read;
    }
    return true;
}

inline bool write_exact(
    HANDLE pipe,
    const void* input,
    std::size_t size) noexcept {
    const auto* cursor = static_cast<const std::uint8_t*>(input);
    while (size != 0) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
            size, std::numeric_limits<DWORD>::max()));
        if (!WriteFile(pipe, cursor, chunk, &written, nullptr)
            || written == 0) {
            return false;
        }
        cursor += written;
        size -= written;
    }
    return true;
}

inline HANDLE parse_inherited_handle(const wchar_t* text) noexcept {
    if (text == nullptr || *text == L'\0') return nullptr;
    wchar_t* end = nullptr;
    const unsigned long long value = std::wcstoull(text, &end, 10);
    if (end == text || *end != L'\0'
        || value > std::numeric_limits<std::uintptr_t>::max()) {
        return nullptr;
    }
    return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(value));
}

namespace detail {

class ProcessAttributeList final {
public:
    ProcessAttributeList() {
        SIZE_T bytes = 0;
        (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        if (bytes == 0) {
            throw std::runtime_error(
                "InitializeProcThreadAttributeList sizing failed: "
                + std::to_string(GetLastError()));
        }
        storage_.resize(bytes);
        list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        if (!InitializeProcThreadAttributeList(list_, 1, 0, &bytes)) {
            list_ = nullptr;
            throw std::runtime_error(
                "InitializeProcThreadAttributeList failed: "
                + std::to_string(GetLastError()));
        }
    }

    ~ProcessAttributeList() {
        if (list_ != nullptr) DeleteProcThreadAttributeList(list_);
    }

    ProcessAttributeList(const ProcessAttributeList&) = delete;
    ProcessAttributeList& operator=(const ProcessAttributeList&) = delete;

    void set_inherited_handles(HANDLE* handles, std::size_t count) {
        if (handles == nullptr || count == 0
            || !UpdateProcThreadAttribute(
                list_,
                0,
                PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                handles,
                count * sizeof(HANDLE),
                nullptr,
                nullptr)) {
            throw std::runtime_error(
                "UpdateProcThreadAttribute(handle list) failed: "
                + std::to_string(GetLastError()));
        }
    }

    [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST get() const noexcept {
        return list_;
    }

private:
    std::vector<std::byte> storage_;
    LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
};

inline std::wstring quote_argument(std::wstring_view argument) {
    if (!argument.empty()
        && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(argument);
    }

    std::wstring output;
    output.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            output.append(backslashes * 2 + 1, L'\\');
            output.push_back(L'"');
        } else {
            output.append(backslashes, L'\\');
            output.push_back(character);
        }
        backslashes = 0;
    }
    output.append(backslashes * 2, L'\\');
    output.push_back(L'"');
    return output;
}

inline void append_argument(std::wstring& command_line, std::wstring_view value) {
    if (!command_line.empty()) command_line.push_back(L' ');
    command_line += quote_argument(value);
}

inline std::string diagnostic_label(std::wstring_view mode) {
    std::string result;
    result.reserve(mode.size());
    for (const wchar_t character : mode) {
        result.push_back(character >= 0x20 && character <= 0x7e
            ? static_cast<char>(character) : '?');
    }
    return result.empty() ? std::string("child") : result;
}

} // namespace detail

class ChildProcess final {
public:
    ChildProcess() noexcept = default;
    ~ChildProcess() { cleanup(); }

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    ChildProcess(ChildProcess&& other) noexcept
        : process_(std::move(other.process_)),
          thread_(std::move(other.thread_)),
          from_child_(std::move(other.from_child_)),
          to_child_(std::move(other.to_child_)),
          process_id_(std::exchange(other.process_id_, 0)),
          label_(std::move(other.label_)),
          command_outstanding_(std::exchange(other.command_outstanding_, false)) {}

    ChildProcess& operator=(ChildProcess&& other) noexcept {
        if (this == &other) return *this;
        cleanup();
        process_ = std::move(other.process_);
        thread_ = std::move(other.thread_);
        from_child_ = std::move(other.from_child_);
        to_child_ = std::move(other.to_child_);
        process_id_ = std::exchange(other.process_id_, 0);
        label_ = std::move(other.label_);
        command_outstanding_ = std::exchange(
            other.command_outstanding_, false);
        return *this;
    }

    static ChildProcess spawn(
        std::wstring_view mode,
        const std::vector<std::wstring>& extra_arguments = {}) {
        if (mode.empty()) {
            throw std::invalid_argument("child process mode must not be empty");
        }

        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.bInheritHandle = TRUE;

        HANDLE child_read_raw = nullptr;
        HANDLE parent_write_raw = nullptr;
        if (!CreatePipe(
                &child_read_raw,
                &parent_write_raw,
                &security,
                child_process_pipe_buffer_bytes)) {
            throw std::runtime_error(
                "CreatePipe(child input) failed: "
                + std::to_string(GetLastError()));
        }
        UniqueHandle child_read(child_read_raw);
        UniqueHandle parent_write(parent_write_raw);

        HANDLE parent_read_raw = nullptr;
        HANDLE child_write_raw = nullptr;
        if (!CreatePipe(
                &parent_read_raw,
                &child_write_raw,
                &security,
                child_process_pipe_buffer_bytes)) {
            throw std::runtime_error(
                "CreatePipe(child output) failed: "
                + std::to_string(GetLastError()));
        }
        UniqueHandle parent_read(parent_read_raw);
        UniqueHandle child_write(child_write_raw);
        if (!SetHandleInformation(parent_write.get(), HANDLE_FLAG_INHERIT, 0)
            || !SetHandleInformation(parent_read.get(), HANDLE_FLAG_INHERIT, 0)) {
            throw std::runtime_error(
                "SetHandleInformation failed: "
                + std::to_string(GetLastError()));
        }

        std::vector<wchar_t> executable(32'768);
        const DWORD length = GetModuleFileNameW(
            nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (length == 0 || length >= executable.size()) {
            throw std::runtime_error(
                "GetModuleFileNameW failed: "
                + std::to_string(GetLastError()));
        }

        std::wstring command_line;
        detail::append_argument(
            command_line, std::wstring_view(executable.data(), length));
        detail::append_argument(command_line, mode);
        detail::append_argument(
            command_line,
            std::to_wstring(reinterpret_cast<std::uintptr_t>(child_read.get())));
        detail::append_argument(
            command_line,
            std::to_wstring(reinterpret_cast<std::uintptr_t>(child_write.get())));
        for (const std::wstring& argument : extra_arguments) {
            detail::append_argument(command_line, argument);
        }

        HANDLE inherited[] = {child_read.get(), child_write.get()};
        detail::ProcessAttributeList attributes;
        attributes.set_inherited_handles(inherited, std::size(inherited));
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = attributes.get();
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(
                nullptr,
                command_line.data(),
                nullptr,
                nullptr,
                TRUE,
                CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                nullptr,
                nullptr,
                &startup.StartupInfo,
                &process)) {
            throw std::runtime_error(
                "CreateProcessW(" + detail::diagnostic_label(mode)
                + ") failed: " + std::to_string(GetLastError()));
        }

        ChildProcess result;
        result.process_.reset(process.hProcess);
        result.thread_.reset(process.hThread);
        result.from_child_ = std::move(parent_read);
        result.to_child_ = std::move(parent_write);
        result.process_id_ = process.dwProcessId;
        result.label_ = detail::diagnostic_label(mode);
        child_read.reset();
        child_write.reset();
        return result;
    }

    [[nodiscard]] HANDLE process() const noexcept { return process_.get(); }
    [[nodiscard]] DWORD process_id() const noexcept { return process_id_; }

    template <class T>
    void send(const T& value, std::string_view context) {
        static_assert(std::is_trivially_copyable_v<T>);
        static_assert(sizeof(T) <= child_process_max_wire_message_bytes,
            "child-process wire messages must be chunked to 4 KiB or less");
        if (command_outstanding_) {
            throw std::logic_error(child_failure(
                context, "previous command has not received a response"));
        }
        if (!write_exact(to_child_.get(), &value, sizeof(value))) {
            throw std::runtime_error(child_failure(context, "pipe write failed"));
        }
        command_outstanding_ = true;
    }

    template <class T>
    T receive(std::uint32_t timeout_ms, std::string_view context) {
        static_assert(std::is_trivially_copyable_v<T>);
        static_assert(sizeof(T) <= child_process_max_wire_message_bytes,
            "child-process wire messages must be chunked to 4 KiB or less");
        const ULONGLONG deadline = GetTickCount64() + timeout_ms;
        T value{};
        auto* cursor = reinterpret_cast<std::uint8_t*>(&value);
        std::size_t remaining = sizeof(value);
        for (;;) {
            DWORD available = 0;
            if (!PeekNamedPipe(
                    from_child_.get(), nullptr, 0, nullptr, &available, nullptr)) {
                throw std::runtime_error(
                    child_failure(context, "response pipe closed"));
            }
            if (available != 0) {
                const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
                    remaining, available));
                DWORD read = 0;
                if (!ReadFile(
                        from_child_.get(), cursor, chunk, &read, nullptr)
                    || read == 0) {
                    throw std::runtime_error(
                        child_failure(context, "response read failed"));
                }
                cursor += read;
                remaining -= read;
                if (remaining == 0) {
                    command_outstanding_ = false;
                    return value;
                }
            }
            const DWORD waited = WaitForSingleObject(process_.get(), 0);
            if (waited == WAIT_OBJECT_0) {
                throw std::runtime_error(
                    child_failure(context, "process exited before response"));
            }
            if (waited == WAIT_FAILED) {
                throw std::runtime_error(
                    child_failure(context, "process wait failed"));
            }
            if (GetTickCount64() >= deadline) {
                throw std::runtime_error(child_failure(
                    context,
                    "timed out after " + std::to_string(timeout_ms) + " ms"));
            }
            Sleep(1);
        }
    }

    void wait(std::uint32_t timeout_ms, std::string_view context) {
        const DWORD waited = WaitForSingleObject(process_.get(), timeout_ms);
        if (waited != WAIT_OBJECT_0) {
            throw std::runtime_error(child_failure(
                context, waited == WAIT_TIMEOUT ? "timed out" : "wait failed"));
        }
        DWORD exit_code = 0;
        if (!GetExitCodeProcess(process_.get(), &exit_code) || exit_code != 0) {
            throw std::runtime_error(child_failure(
                context, "unexpected code " + std::to_string(exit_code)));
        }
    }

    void wait_for_exit(std::uint32_t timeout_ms) {
        wait(timeout_ms, "exit");
    }

    void terminate(UINT exit_code) noexcept {
        if (process_.get() == nullptr) return;
        if (WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT) {
            (void)TerminateProcess(process_.get(), exit_code);
            (void)WaitForSingleObject(process_.get(), 2'000);
        }
    }

private:
    std::string child_failure(
        std::string_view context,
        std::string detail) const {
        DWORD exit_code = STILL_ACTIVE;
        if (process_.get() != nullptr) {
            (void)GetExitCodeProcess(process_.get(), &exit_code);
        }
        return label_ + " (pid=" + std::to_string(process_id_) + ") "
            + std::string(context) + ": " + std::move(detail)
            + ", exit=" + std::to_string(exit_code)
            + ", win32=" + std::to_string(GetLastError());
    }

    void cleanup() noexcept {
        to_child_.reset();
        if (process_.get() != nullptr
            && WaitForSingleObject(process_.get(), 250) == WAIT_TIMEOUT) {
            terminate(0xfcb6u);
        }
        from_child_.reset();
        thread_.reset();
        process_.reset();
        process_id_ = 0;
        label_.clear();
        command_outstanding_ = false;
    }

    UniqueHandle process_;
    UniqueHandle thread_;
    UniqueHandle from_child_;
    UniqueHandle to_child_;
    DWORD process_id_ = 0;
    std::string label_ = "child";
    bool command_outstanding_ = false;
};

} // namespace fluxcap::test_support
