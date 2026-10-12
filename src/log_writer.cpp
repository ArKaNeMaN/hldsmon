// HldsMonitoring — LogWriter implementation (Phase 2).
// Append-only JSONL with daily UTC rotation, 0600 file / 0700 dir perms.
// Host-testable: no engine/metamod headers, no exceptions, no RTTI.

#include "log_writer.h"
#include "retention.h"

#include <cerrno>
#include <cstring>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace hldsmon {

namespace {

LogFdObserver g_fd_observer = nullptr;

void notify_fd(FILE* f)
{
    if (g_fd_observer != nullptr) {
        g_fd_observer(f != nullptr ? fileno(f) : -1);
    }
}

// Decompose a wall-clock epoch (UTC) into Y/M/D. Returns false on failure.
bool utc_ymd(double now_utc, int* year, int* month, int* day)
{
    const time_t secs = static_cast<time_t>(now_utc);
    struct tm tmv;
    if (gmtime_r(&secs, &tmv) == nullptr) {
        return false;
    }
    *year  = tmv.tm_year + 1900;
    *month = tmv.tm_mon + 1;
    *day   = tmv.tm_mday;
    return true;
}

bool mkdir_p(const char* path, mode_t mode)
{
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    char tmp[4096];
    const size_t len = std::strlen(path);
    if (len == 0 || len >= sizeof(tmp)) {
        return false;
    }
    std::memcpy(tmp, path, len + 1);
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }
    for (char* p = tmp + 1; *p != '\0'; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
                return false;
            }
            *p = '/';
        }
    }
    return mkdir(tmp, mode) == 0 || errno == EEXIST;
}

} // namespace

void set_log_fd_observer(LogFdObserver observer) noexcept
{
    g_fd_observer = observer;
}

LogWriter::~LogWriter()
{
    close_file();
}

void LogWriter::close_file()
{
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    notify_fd(nullptr);
}

bool LogWriter::open_day(int year, int month, int day)
{
    char name[HLDSMON_LOGWRITER_PATH_MAX];
    const int n = std::snprintf(name, sizeof(name),
                                "%s/hldsmon-%04d-%02d-%02d.jsonl",
                                dir_, year, month, day);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(name)) {
        ++error_count_;
        return false;
    }

    FILE* f = std::fopen(name, "a");
    if (f == nullptr) {
        ++error_count_;
        return false;
    }

    // Enforce 0600 even when the file already existed with looser perms.
    if (fchmod(fileno(f), 0600) != 0) {
        std::fclose(f);
        ++error_count_;
        return false;
    }

    file_ = f;
    std::memcpy(path_, name, static_cast<size_t>(n) + 1);
    year_        = year;
    month_       = month;
    day_         = day;
    bytes_today_ = 0;
    size_capped_ = false;
    enabled_     = true;
    notify_fd(file_);
    prune();
    return true;
}

bool LogWriter::open(const char* dir, double now_utc)
{
    close_file();
    enabled_     = false;
    size_capped_ = false;
    path_[0]     = '\0';
    bytes_today_ = 0;

    if (dir == nullptr) {
        ++error_count_;
        return false;
    }

    const int n = std::snprintf(dir_, sizeof(dir_), "%s", dir);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(dir_)) {
        ++error_count_;
        return false;
    }

    if (!mkdir_p(dir_, 0700)) {
        ++error_count_;
        return false;
    }

    int y = 0, m = 0, d = 0;
    if (!utc_ymd(now_utc, &y, &m, &d)) {
        ++error_count_;
        return false;
    }

    return open_day(y, m, d);
}

void LogWriter::write(std::span<const char> line)
{
    if (!enabled_ || file_ == nullptr) {
        return;
    }
    if (line.data() == nullptr && !line.empty()) {
        ++error_count_;
        return;
    }
    const size_t len = line.size();

    if (len > 0 && std::fwrite(line.data(), 1, len, file_) != len) {
        ++error_count_;
        enabled_ = false;
        return;
    }
    if (std::fputc('\n', file_) == EOF) {
        ++error_count_;
        enabled_ = false;
        return;
    }

    bytes_today_ += static_cast<uint64_t>(len) + 1u;

    if (max_bytes_ != 0 && bytes_today_ >= max_bytes_) {
        // Stop writing for the day but keep the object usable; a new day's
        // open() resets the counter and the cap.
        size_capped_ = true;
        enabled_     = false;
    }
}

void LogWriter::flush()
{
    if (file_ != nullptr) {
        std::fflush(file_);
    }
}

void LogWriter::maybe_rotate(double now_utc)
{
    if (file_ == nullptr) {
        return;
    }

    int y = 0, m = 0, d = 0;
    if (!utc_ymd(now_utc, &y, &m, &d)) {
        ++error_count_;
        return;
    }
    if (y == year_ && m == month_ && d == day_) {
        return;
    }

    std::fclose(file_);
    file_    = nullptr;
    enabled_ = false;
    // open_day resets bytes/cap and re-enables on success; on failure it
    // increments error_count_ and leaves the writer disabled.
    (void)open_day(y, m, d);
}

void LogWriter::set_max_mb(uint64_t mb)
{
    const uint64_t kLimitMb = UINT64_MAX / (1024ULL * 1024ULL);
    max_bytes_ = (mb > kLimitMb) ? 0 : mb * 1024ULL * 1024ULL;

    if (max_bytes_ != 0 && bytes_today_ >= max_bytes_) {
        size_capped_ = true;
        enabled_     = false;
    }
}

void LogWriter::set_retention_days(int days)
{
    retention_days_ = days;
}

void LogWriter::prune()
{
    if (retention_days_ <= 0) {
        return; // retention disabled: keep all files
    }

    DIR* d = ::opendir(dir_);
    if (d == nullptr) {
        ++error_count_; // counted, non-fatal: writing/rotation continue
        return;
    }

    constexpr size_t kMaxNames = 1024;
    constexpr size_t kNameCap  = 256;
    static char s_names[kMaxNames][kNameCap];
    static const char* s_ptrs[kMaxNames];
    size_t n = 0;
    struct dirent* ent = nullptr;
    while ((ent = ::readdir(d)) != nullptr) {
        if (n >= kMaxNames) {
            break; // bounded: any further entries are left untouched
        }
        std::snprintf(s_names[n], kNameCap, "%s", ent->d_name);
        s_ptrs[n] = s_names[n];
        ++n;
    }
    ::closedir(d);

    if (n == 0) {
        return;
    }

    static const char* s_doomed[kMaxNames];
    size_t doomed_n = 0;
    if (!retention_select(s_ptrs, n, year_, month_, day_, retention_days_,
                          s_doomed, kMaxNames, &doomed_n)) {
        return; // fail-safe: selection overflow => exactly zero unlink
    }

    uint32_t removed = 0;
    for (size_t i = 0; i < doomed_n; ++i) {
        char full[HLDSMON_LOGWRITER_PATH_MAX];
        const int fn = std::snprintf(full, sizeof(full), "%s/%s", dir_, s_doomed[i]);
        if (fn < 0 || static_cast<size_t>(fn) >= sizeof(full)) {
            ++error_count_;
            continue;
        }
        if (std::strcmp(full, path_) == 0) {
            continue; // never delete the currently-open file
        }
        if (::unlink(full) != 0) {
            ++error_count_; // counted, loop continues with other candidates
            continue;
        }
        ++removed;
    }
    const uint32_t room = 0xFFFFFFFFu - last_prune_count_;
    last_prune_count_ = (removed > room) ? 0xFFFFFFFFu : last_prune_count_ + removed;
}

} // namespace hldsmon
