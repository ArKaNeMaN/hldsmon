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
const double kDay3 = kDay2 + 86400.0;
const double kDay4 = kDay3 + 86400.0;

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

bool is_symlink(const char* path)
{
    struct stat st;
    return ::lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}

// Create `dir/<name>` holding one JSON line. Returns the full path in `out`.
void make_daily_file(const char* dir, const char* name, char* out, size_t cap)
{
    std::snprintf(out, cap, "%s/%s", dir, name);
    FILE* f = std::fopen(out, "w");
    if (f != nullptr) {
        std::fputs("{}\n", f);
        std::fclose(f);
    }
}

// Full path of the daily file for `now_utc`.
void daily_path(const char* dir, double now_utc, char* out, size_t cap)
{
    char name[128];
    day_name(now_utc, name, sizeof(name));
    std::snprintf(out, cap, "%s/%s", dir, name);
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
    w.write(std::span<const char>(line, std::strlen(line)));
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

    w.write(std::span<const char>(line, std::strlen(line)));
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
    w.set_retention_days(3);
    const bool ok = w.open("/proc/nonexistent/hldsmon", kDay1);
    check(!ok, "open returns false");
    check(!w.enabled(), "writer disabled");
    check(w.error_count() > 0, "error counted");
    check(w.current_path()[0] == '\0', "no current path");
    // Must remain callable without crashing.
    w.write(std::span<const char>("{}", 2));
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
        w.write(std::span<const char>(line, len));
        ++attempts;
    }

    check(w.size_capped(), "size_capped after exceeding limit");
    check(!w.enabled(), "writing disabled after cap");
    check(w.bytes_today() >= 1024 * 1024, "bytes_today reached limit");

    const uint64_t frozen = w.bytes_today();
    for (int i = 0; i < 10; ++i) {
        w.write(std::span<const char>(line, len));
    }
    check(w.bytes_today() == frozen, "further writes are no-ops");
    check(w.error_count() == 0, "no spurious errors on clean cap");
}

// REQ-L4 / design §28.3 п.9: set_max_mb(0)=unlimited. The producer must keep
// writing past the old 1 MiB cap and never report size_capped(); the non-zero
// valve is still enforced by test_size_cap().
void test_size_unlimited()
{
    std::printf("[size unlimited]\n");
    char dir[256];
    if (!make_temp_dir(dir, sizeof(dir))) {
        check(false, "mkdtemp for size unlimited");
        return;
    }

    hldsmon::LogWriter w;
    check(w.open(dir, kDay1), "open for size unlimited");
    w.set_max_mb(0);

    char line[256];
    std::memset(line, 'x', sizeof(line));
    line[sizeof(line) - 1] = '\0';
    const size_t len = std::strlen(line); // 255

    // 5200 * (255 + '\n') = 1,331,200 bytes > 1 MiB (the old cap size).
    const unsigned writes = 5200;
    for (unsigned i = 0; i < writes; ++i) {
        w.write(std::span<const char>(line, len));
    }

    check(!w.size_capped(), "set_max_mb(0): never size capped");
    check(w.enabled(), "set_max_mb(0): stays enabled past 1 MiB");
    check(w.bytes_today() == static_cast<uint64_t>(writes) * (len + 1),
          "bytes_today accumulates without cap");
    check(w.error_count() == 0, "no errors on unlimited writes");
}

void test_prune_basic()
{
    std::printf("[prune basic]\n");
    char dir[256];
    if (!make_temp_dir(dir, sizeof(dir))) {
        check(false, "mkdtemp for prune basic");
        return;
    }

    char name[128];
    char p1[512], p2[512], p3[512], p4[512];
    day_name(kDay1, name, sizeof(name));
    make_daily_file(dir, name, p1, sizeof(p1));
    day_name(kDay2, name, sizeof(name));
    make_daily_file(dir, name, p2, sizeof(p2));
    day_name(kDay3, name, sizeof(name));
    make_daily_file(dir, name, p3, sizeof(p3));
    daily_path(dir, kDay4, p4, sizeof(p4));

    char marker[512];
    std::snprintf(marker, sizeof(marker), "%s/hldsmon-crash.marker", dir);
    FILE* m = std::fopen(marker, "w");
    if (m != nullptr) {
        std::fputs("x\n", m);
        std::fclose(m);
    }

    hldsmon::LogWriter w;
    w.set_retention_days(3);
    check(w.open(dir, kDay4), "open day4 with retention 3");
    check(!file_exists(p1), "day1 (T-3) pruned");
    check(file_exists(p2), "day2 (T-2) kept");
    check(file_exists(p3), "day3 (T-1) kept");
    check(file_exists(p4), "day4 (T) kept");
    check(std::strcmp(w.current_path(), p4) == 0, "current file is day4");
    check(file_exists(marker), "crash marker never pruned");
    check(w.error_count() == 0, "no errors on clean prune");
    check(w.last_prune_count() == 1, "prune count tracks removed files");
    w.clear_last_prune_count();
    check(w.last_prune_count() == 0, "prune count clear");
}

void test_prune_rotation()
{
    std::printf("[prune rotation N=1]\n");
    char dir[256];
    if (!make_temp_dir(dir, sizeof(dir))) {
        check(false, "mkdtemp for prune rotation");
        return;
    }

    char p1[512], p2[512];
    daily_path(dir, kDay1, p1, sizeof(p1));
    daily_path(dir, kDay2, p2, sizeof(p2));

    hldsmon::LogWriter w;
    w.set_retention_days(1);
    check(w.open(dir, kDay1), "open day1 with retention 1");
    const char* line = "{\"type\":\"server\",\"t\":1}";
    w.write(std::span<const char>(line, std::strlen(line)));
    w.flush();

    w.maybe_rotate(kDay2);
    check(!file_exists(p1), "day1 pruned after rotation (N=1)");
    check(file_exists(p2), "day2 kept after rotation");
    check(std::strcmp(w.current_path(), p2) == 0, "current file is day2");
    check(w.error_count() == 0, "no errors across rotation prune");
    check(w.last_prune_count() == 1, "rotation prune counted");
}

void test_prune_symlink()
{
    std::printf("[prune symlink]\n");
    char dir[256];
    if (!make_temp_dir(dir, sizeof(dir))) {
        check(false, "mkdtemp for symlink");
        return;
    }

    char target[512];
    std::snprintf(target, sizeof(target), "%s/real.dat", dir);
    FILE* f = std::fopen(target, "w");
    if (f != nullptr) {
        std::fputs("secret\n", f);
        std::fclose(f);
    }

    char link[512];
    daily_path(dir, kDay1, link, sizeof(link));
    check(::symlink("real.dat", link) == 0, "create daily-named symlink");
    check(is_symlink(link), "link is a symlink");

    hldsmon::LogWriter w;
    w.set_retention_days(1);
    check(w.open(dir, kDay2), "open day2 with retention 1");
    check(!is_symlink(link), "daily-named symlink unlinked");
    check(file_exists(target), "symlink target file survives");

    FILE* rf = std::fopen(target, "r");
    char buf[32] = {0};
    const bool ok = rf != nullptr && std::fgets(buf, sizeof(buf), rf) != nullptr &&
                    std::strcmp(buf, "secret\n") == 0;
    if (rf != nullptr) {
        std::fclose(rf);
    }
    check(ok, "target content untouched");
}

void test_prune_unlink_open()
{
    std::printf("[prune unlink-while-open]\n");
    char dir[256];
    if (!make_temp_dir(dir, sizeof(dir))) {
        check(false, "mkdtemp for unlink-open");
        return;
    }

    char p1[512];
    daily_path(dir, kDay1, p1, sizeof(p1));
    FILE* f = std::fopen(p1, "w");
    if (f != nullptr) {
        std::fputs("one-line\n", f);
        std::fclose(f);
    }

    FILE* rd = std::fopen(p1, "r");
    check(rd != nullptr, "open reader on the old file");

    hldsmon::LogWriter w;
    w.set_retention_days(1);
    check(w.open(dir, kDay2), "open day2 with retention 1");
    check(!file_exists(p1), "old file unlinked while a reader holds it");

    char buf[32] = {0};
    const bool read_ok = rd != nullptr && std::fgets(buf, sizeof(buf), rd) != nullptr &&
                         std::strcmp(buf, "one-line\n") == 0;
    if (rd != nullptr) {
        std::fclose(rd);
    }
    check(read_ok, "reader still reads the unlinked inode");
    check(w.error_count() == 0, "no errors on unlink-while-open");
}

void test_prune_empty_dir()
{
    std::printf("[prune fresh dir]\n");
    char dir[256];
    if (!make_temp_dir(dir, sizeof(dir))) {
        check(false, "mkdtemp for prune empty");
        return;
    }

    hldsmon::LogWriter w;
    w.set_retention_days(3);
    check(w.open(dir, kDay1), "open fresh dir with retention");
    check(w.enabled(), "enabled on fresh dir");
    check(w.error_count() == 0, "no error when only the current file exists");
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
    test_size_unlimited();
    test_prune_basic();
    test_prune_rotation();
    test_prune_symlink();
    test_prune_unlink_open();
    test_prune_empty_dir();
    test_fd_observer();

    if (g_failures == 0) {
        std::printf("PASS\n");
        return 0;
    }
    std::printf("FAILED: %d check(s)\n", g_failures);
    return 1;
}
