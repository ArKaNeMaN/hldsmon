// HldsMonitoring — host tests for LogWriter (Phase 2).
// Native g++ build (no -m32, no engine headers), e.g.:
//   g++ -std=c++17 -Wall -Wextra -Werror -Isrc tests/test_log_writer.cpp src/log_writer.cpp -o /tmp/tlw

#include "log_writer.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

int g_failures = 0;

void check(bool cond, const char* what)
{
    if (cond) {
        std::printf("  ok   : %s\n", what);
    } else {
        std::printf("  FAIL : %s\n", what);
        ++g_failures;
    }
}

// Epoch for 2024-04-05 12:00:00 UTC (monotonic reference point).
const double kDay1 = 1712322000.0;
const double kDay2 = kDay1 + 86400.0;

void day_name(double now_utc, char* out, size_t cap)
{
    const time_t secs = static_cast<time_t>(now_utc);
    struct tm tmv;
    gmtime_r(&secs, &tmv);
    std::snprintf(out, cap, "hldsmon-%04d-%02d-%02d.jsonl",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
}

bool file_exists(const char* path)
{
    struct stat st;
    return ::stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool is_dir(const char* path)
{
    struct stat st;
    return ::stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// Create a unique temp directory; returns false on failure.
bool make_temp_dir(char* out, size_t cap)
{
    std::snprintf(out, cap, "/tmp/hldsmon_lw_XXXXXX");
    return mkdtemp(out) != nullptr;
}

void test_rotation()
{
    std::printf("[rotation]\n");
    char dir[256];
    if (!make_temp_dir(dir, sizeof(dir))) {
        check(false, "mkdtemp for rotation");
        return;
    }

    hldsmon::LogWriter w;
    check(w.open(dir, kDay1), "open day 1");
    check(w.enabled(), "enabled after open");

    char n1[128];
    day_name(kDay1, n1, sizeof(n1));
    char p1[512];
    std::snprintf(p1, sizeof(p1), "%s/%s", dir, n1);
    check(std::strcmp(w.current_path(), p1) == 0, "current_path is day-1 file");

    const char* line = "{\"type\":\"server\",\"t\":1}";
    w.write(line, std::strlen(line));
    w.flush();

    check(w.bytes_today() == std::strlen(line) + 1, "bytes_today counts line+nl");

    w.maybe_rotate(kDay2);
    check(w.enabled(), "enabled after rotation");

    char n2[128];
    day_name(kDay2, n2, sizeof(n2));
    char p2[512];
    std::snprintf(p2, sizeof(p2), "%s/%s", dir, n2);
    check(std::strcmp(w.current_path(), p2) == 0, "current_path is day-2 file");
    check(std::strcmp(w.current_path(), p1) != 0, "current_path changed");
    check(file_exists(p2), "day-2 file exists");
    check(w.bytes_today() == 0, "byte counter reset on rotation");

    w.write(line, std::strlen(line));
    w.flush();
    check(file_exists(p1) && file_exists(p2), "both day files present");

    struct stat st1;
    check(::stat(p1, &st1) == 0 && st1.st_size == static_cast<off_t>(std::strlen(line) + 1),
          "day-1 file holds exactly one line");
}

void test_permissions()
{
    std::printf("[permissions]\n");
    char dir[256];
    if (!make_temp_dir(dir, sizeof(dir))) {
        check(false, "mkdtemp for perms");
        return;
    }

    hldsmon::LogWriter w;
    check(w.open(dir, kDay1), "open for perms");

    struct stat ds;
    check(::stat(dir, &ds) == 0 && is_dir(dir), "dir exists");
    check((ds.st_mode & 0777) == 0700, "dir mode is 0700");

    struct stat fs;
    check(::stat(w.current_path(), &fs) == 0, "file stat");
    check((fs.st_mode & 0777) == 0600, "file mode is 0600");
}

void test_unavailable_dir()
{
    std::printf("[unavailable dir]\n");
    hldsmon::LogWriter w;
    const bool ok = w.open("/proc/nonexistent/hldsmon", kDay1);
    check(!ok, "open returns false");
    check(!w.enabled(), "writer disabled");
    check(w.error_count() > 0, "error counted");
    check(w.current_path()[0] == '\0', "no current path");
    // Must remain callable without crashing.
    w.write("{}", 2);
    w.flush();
    w.maybe_rotate(kDay2);
    check(!w.enabled(), "still disabled after no-op calls");
}

void test_size_cap()
{
    std::printf("[size cap]\n");
    char dir[256];
    if (!make_temp_dir(dir, sizeof(dir))) {
        check(false, "mkdtemp for size cap");
        return;
    }

    hldsmon::LogWriter w;
    check(w.open(dir, kDay1), "open for size cap");
    w.set_max_mb(1); // 1 MiB

    char line[256];
    std::memset(line, 'x', sizeof(line));
    line[sizeof(line) - 1] = '\0';
    const size_t len = std::strlen(line); // 255

    unsigned attempts = 0;
    while (!w.size_capped() && attempts < 20000) {
        w.write(line, len);
        ++attempts;
    }

    check(w.size_capped(), "size_capped after exceeding limit");
    check(!w.enabled(), "writing disabled after cap");
    check(w.bytes_today() >= 1024 * 1024, "bytes_today reached limit");

    const uint64_t frozen = w.bytes_today();
    for (int i = 0; i < 10; ++i) {
        w.write(line, len);
    }
    check(w.bytes_today() == frozen, "further writes are no-ops");
    check(w.error_count() == 0, "no spurious errors on clean cap");
}

int g_obs_fd = -2;
int g_obs_calls = 0;

void observe_fd(int fd)
{
    g_obs_fd = fd;
    ++g_obs_calls;
}

void test_fd_observer()
{
    std::printf("[fd observer]\n");
    char dir[256];
    if (!make_temp_dir(dir, sizeof(dir))) {
        check(false, "mkdtemp for observer");
        return;
    }

    hldsmon::set_log_fd_observer(&observe_fd);
    g_obs_fd = -2;
    g_obs_calls = 0;

    {
        hldsmon::LogWriter w;
        check(w.open(dir, kDay1), "open with observer");
        check(g_obs_fd >= 0, "observer saw a live fd on open");
        const int calls_after_open = g_obs_calls;

        w.maybe_rotate(kDay2);
        check(g_obs_calls > calls_after_open, "observer notified on rotation");
        check(g_obs_fd >= 0, "observer sees a live fd after rotation");
    }

    check(g_obs_fd == -1, "observer notified -1 once the file is closed");
    hldsmon::set_log_fd_observer(nullptr);
}

} // namespace

int main()
{
    std::printf("test_log_writer\n");
    test_rotation();
    test_permissions();
    test_unavailable_dir();
    test_size_cap();
    test_fd_observer();

    if (g_failures == 0) {
        std::printf("PASS\n");
        return 0;
    }
    std::printf("FAILED: %d check(s)\n", g_failures);
    return 1;
}
