// HldsMonitoring — LogWriter (Phase 2).
// Appends caller-provided JSONL lines to a daily-rotated UTC file.
// Host-testable: no engine/metamod headers, no exceptions, no RTTI.

#ifndef HLDSMON_LOG_WRITER_H
#define HLDSMON_LOG_WRITER_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>

#ifndef HLDSMON_LOGWRITER_PATH_MAX
#define HLDSMON_LOGWRITER_PATH_MAX 4096
#endif

namespace hldsmon {

// Optional observer receiving the raw fd of the current log file after every
// (re)open and rotation, or -1 once the file is closed. The crash handler
// registers it to target the live file. Host builds leave it unset. Invoked
// on the server thread; must not allocate.
using LogFdObserver = void (*)(int fd);
void set_log_fd_observer(LogFdObserver observer) noexcept;

// Append-only JSONL writer with daily UTC rotation.
//
// Contract:
//  - open() either succeeds (enabled()==true) or fails cleanly:
//    enabled()==false, error_count()>0, no crash.
//  - write()/flush()/maybe_rotate() run on the server thread only.
//  - write() performs no heap allocation (buffered FILE* only).
//  - On size-cap or write failure, writing is disabled but the object stays
//    usable; success on a later day's open re-enables it.
class LogWriter {
public:
    LogWriter() = default;
    ~LogWriter();

    LogWriter(const LogWriter&) = delete;
    LogWriter& operator=(const LogWriter&) = delete;

    // Ensure `dir` exists (mode 0700) and open/append the day's file
    // (mode 0600) named hldsmon-YYYY-MM-DD.jsonl for the UTC date of
    // `now_utc`. Returns true on success. On failure: enabled=false,
    // error_count incremented, no crash.
    bool open(const char* dir, double now_utc);

    // Append `line` followed by '\n'. No-op if not enabled. Errors counted.
    void write(const char* line, size_t len);

    // fflush the current file (no-op if none).
    void flush();

    // If the UTC date of `now_utc` differs from the open file's date,
    // close it and open the new day's file in the same directory.
    void maybe_rotate(double now_utc);

    bool enabled() const { return enabled_; }
    bool size_capped() const { return size_capped_; }
    const char* current_path() const { return path_; }
    uint64_t bytes_today() const { return bytes_today_; }
    uint64_t error_count() const { return error_count_; }

    // Per-day size limit. When bytes_today() >= mb*1024*1024, disable
    // writing (size_capped()==true) while keeping the object usable.
    // mb==0 disables the cap.
    void set_max_mb(uint64_t mb);

private:
    bool open_day(int year, int month, int day);
    void close_file();

    FILE*    file_         = nullptr;
    bool     enabled_      = false;
    bool     size_capped_  = false;
    char     dir_[HLDSMON_LOGWRITER_PATH_MAX]  = {0};
    char     path_[HLDSMON_LOGWRITER_PATH_MAX] = {0};
    int      year_  = 0;
    int      month_ = 0;
    int      day_   = 0;
    uint64_t bytes_today_ = 0;
    uint64_t error_count_ = 0;
    uint64_t max_bytes_   = 0;
};

} // namespace hldsmon

#endif // HLDSMON_LOG_WRITER_H
