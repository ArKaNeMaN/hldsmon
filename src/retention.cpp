// HldsMonitoring — retention selection core implementation (Stage 3b, §20.5).
// Pure logic only: no filesystem, no allocation, no engine headers.

#include "retention.h"

namespace hldsmon {

namespace {

bool is_leap(int y) noexcept
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

int days_in_month(int y, int m) noexcept
{
    static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12) {
        return 0;
    }
    if (m == 2 && is_leap(y)) {
        return 29;
    }
    return kDays[m - 1];
}

bool is_digit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

} // namespace

int64_t days_from_civil(int y, int m, int d) noexcept
{
    y -= (m <= 2) ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy =
        static_cast<unsigned>((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

bool parse_log_date(const char* name, int* year, int* month, int* day) noexcept
{
    if (name == nullptr) {
        return false;
    }

    // "hldsmon-" (8) + YYYY (4) + '-' (1) + MM (2) + '-' (1) + DD (2) +
    // ".jsonl" (6) = 24 chars + NUL. Any mismatch (including an early NUL)
    // returns immediately, so the C string is never read past its terminator.
    static const char kPrefix[] = "hldsmon-";
    for (int i = 0; i < 8; ++i) {
        if (name[i] != kPrefix[i]) {
            return false;
        }
    }
    for (int i = 8; i < 12; ++i) {
        if (!is_digit(name[i])) {
            return false;
        }
    }
    if (name[12] != '-') {
        return false;
    }
    if (!is_digit(name[13]) || !is_digit(name[14])) {
        return false;
    }
    if (name[15] != '-') {
        return false;
    }
    if (!is_digit(name[16]) || !is_digit(name[17])) {
        return false;
    }
    static const char kSuffix[] = ".jsonl";
    for (int i = 0; i < 6; ++i) {
        if (name[18 + i] != kSuffix[i]) {
            return false;
        }
    }
    if (name[24] != '\0') {
        return false;
    }

    const int y = (name[8] - '0') * 1000 + (name[9] - '0') * 100 +
                  (name[10] - '0') * 10 + (name[11] - '0');
    const int mo = (name[13] - '0') * 10 + (name[14] - '0');
    const int d = (name[16] - '0') * 10 + (name[17] - '0');

    if (y < 1970 || y > 9999) {
        return false;
    }
    if (mo < 1 || mo > 12) {
        return false;
    }
    if (d < 1 || d > days_in_month(y, mo)) {
        return false;
    }

    if (year != nullptr) {
        *year = y;
    }
    if (month != nullptr) {
        *month = mo;
    }
    if (day != nullptr) {
        *day = d;
    }
    return true;
}

bool retention_select(const char* const* names, size_t count,
                      int year, int month, int day, int retention_days,
                      const char** out, size_t out_cap, size_t* out_n) noexcept
{
    if (out_n == nullptr) {
        return false;
    }
    if (retention_days <= 0) {
        *out_n = 0; // retention disabled: keep everything
        return true;
    }

    const int64_t today = days_from_civil(year, month, day);
    const int64_t cutoff = today - static_cast<int64_t>(retention_days);

    // Pass 1: count doomed candidates without writing, so an out_cap overflow
    // can fail before any output slot is touched (R-2 fail-safe).
    size_t candidates = 0;
    for (size_t i = 0; i < count; ++i) {
        if (names == nullptr || names[i] == nullptr) {
            continue;
        }
        int y = 0, m = 0, d = 0;
        if (!parse_log_date(names[i], &y, &m, &d)) {
            continue;
        }
        if (days_from_civil(y, m, d) <= cutoff) {
            ++candidates;
        }
    }
    if (candidates > out_cap) {
        return false; // keep *out_n untouched; caller unlinks nothing
    }

    // Pass 2: fill `out` (out_cap >= candidates is guaranteed here).
    size_t k = 0;
    for (size_t i = 0; i < count && k < candidates; ++i) {
        if (names == nullptr || names[i] == nullptr) {
            continue;
        }
        int y = 0, m = 0, d = 0;
        if (!parse_log_date(names[i], &y, &m, &d)) {
            continue;
        }
        if (days_from_civil(y, m, d) <= cutoff) {
            if (out != nullptr) {
                out[k] = names[i];
            }
            ++k;
        }
    }
    *out_n = k;
    return true;
}

} // namespace hldsmon
