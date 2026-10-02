// live/file_lock.hpp -- one writer per ledger.
//
// Two altair_live_engine processes on the same data/live/ would both append
// to the same journal and both rewrite the same snapshot: interleaved rows,
// lost updates, a position held twice. The engine takes this lock before it
// touches anything there and refuses to start without it.
//
// An OS lock (flock on POSIX, LockFileEx on Windows), not a marker file: it
// dies with the process, so a crash never leaves a stale lock that a person
// has to find and delete. The file itself is left in place; only the lock
// matters, and it holds the owner's pid for whoever looks.

#pragma once

#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace altair::live {

class LiveFileLock {
public:
    /// Try once, without waiting. held() says whether it was taken.
    explicit LiveFileLock(const std::string& path) {
#ifdef _WIN32
        h_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) { why_ = "cannot open " + path; return; }
        OVERLAPPED ov{};
        if (!LockFileEx(h_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) {
            why_ = "another process holds " + path;
            CloseHandle(h_);
            h_ = INVALID_HANDLE_VALUE;
            return;
        }
        held_ = true;
        const std::string pid = std::to_string(GetCurrentProcessId()) + "\n";
        DWORD wrote = 0;
        SetEndOfFile(h_);
        (void)WriteFile(h_, pid.data(), static_cast<DWORD>(pid.size()), &wrote, nullptr);
#else
        fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd_ < 0) { why_ = "cannot open " + path + ": " + std::strerror(errno); return; }
        if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            why_ = errno == EWOULDBLOCK ? "another process holds " + path : "cannot lock " + path + ": " + std::strerror(errno);
            ::close(fd_);
            fd_ = -1;
            return;
        }
        held_ = true;
        const std::string pid = std::to_string(::getpid()) + "\n";
        if (::ftruncate(fd_, 0) == 0) (void)!::write(fd_, pid.data(), pid.size());
#endif
    }
    ~LiveFileLock() {
#ifdef _WIN32
        if (h_ != INVALID_HANDLE_VALUE) {
            OVERLAPPED ov{};
            UnlockFileEx(h_, 0, 1, 0, &ov);
            CloseHandle(h_);
        }
#else
        if (fd_ >= 0) { ::flock(fd_, LOCK_UN); ::close(fd_); }
#endif
    }
    LiveFileLock(const LiveFileLock&) = delete;
    LiveFileLock& operator=(const LiveFileLock&) = delete;

    [[nodiscard]] bool held() const noexcept { return held_; }
    [[nodiscard]] const std::string& why() const noexcept { return why_; }

private:
#ifdef _WIN32
    HANDLE h_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
    bool held_ = false;
    std::string why_;
};

} // namespace altair::live
