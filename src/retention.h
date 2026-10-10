// HldsMonitoring — retention selection core (Stage 3b, design §20.5).
// Pure: no filesystem, no allocation, no engine/metamod headers. Compiled into
// the host tests native g++ via CORE_SRCS.

#ifndef HLDSMON_RETENTION_H
#define HLDSMON_RETENTION_H

#include <cstddef>
#include <cstdint>

namespace hldsmon {

// Day number (days since 1970-01-01) for a proleptic Gregorian date, using the
// civil-from-days inverse (Howard Hinnant). Pure integer math; no libc/timegm,
// so month/year boundaries and leap years compare correctly.
int64_t days_from_civil(int year, int month, int day) noexcept;

// Parse a basename "hldsmon-YYYY-MM-DD.jsonl" (exact template). On an exact
// match fills year/month/day and returns true, otherwise false. Validates the
// calendar date: YYYY in [1970,9999], MM in [1..12], DD in [1..days_in_month]
// with leap-year awareness (e.g. 2026-02-29 is rejected).
bool parse_log_date(const char* name, int* year, int* month, int* day) noexcept;

// Choose the rotated daily files to delete, given the current UTC date
// (year,month,day) and retention_days == N (max files kept incl. the current):
//   - N <= 0                         -> success, *out_n = 0 (retention off);
//   - a name failing parse_log_date  -> ignored;
//   - a name is doomed iff day_num(name) <= today_num - N.
// Writes non-owning pointers to the doomed elements of `names` into `out`
// (capacity out_cap) and the number into *out_n; returns true on success.
// Fail-safe (R-2): if doomed candidates exceed out_cap, writes nothing, leaves
// *out_n untouched and returns false — the filesystem caller must then perform
// exactly zero unlink calls.
bool retention_select(const char* const* names, size_t count,
                      int year, int month, int day, int retention_days,
                      const char** out, size_t out_cap, size_t* out_n) noexcept;

} // namespace hldsmon

#endif // HLDSMON_RETENTION_H
