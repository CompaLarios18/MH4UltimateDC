// The lines tools/native/check_no_game_data.py calls trace-like, hex dumps, disassembly and byte
// arrays (its TRACE_PATTERNS and LISTING_PATTERNS), copied as text so that Make drops what
// report_check.py would refuse. tests/test_native_report_check.py compares each of these with the
// guard's own, so a change there that is not copied here fails the suite. ECMAScript syntax; the
// Python patterns are the same text.
#pragma once

namespace mh4n::app::report {

inline constexpr const char* kTracePatterns[] = {
    R"MH4N(^\d+ [0-9A-F]{2} pc=[0-9A-F]{8})MH4N",
    R"MH4N(^# thread )MH4N",
    R"MH4N(seq=\d+ session=)MH4N",
    R"MH4N(^f=\d+ t=\d+)MH4N",
    R"MH4N(^\s*count=\d+\s+addr=(0x)?[0-9A-Fa-f]+\s+size=\d+\s+rw=(read|write)\b)MH4N",
    R"MH4N(svc=0x)MH4N",
    R"MH4N(cmd=0x)MH4N",
};

inline constexpr const char* kListingPatterns[] = {
    R"MH4N(^\s*[0-9A-Fa-f]{4,8}:?(?:\s+[0-9A-Fa-f]{2}(?![0-9A-Fa-f])){8,})MH4N",
    R"MH4N(^\s*[0-9A-Fa-f]{4,8}:?(?:\s+[0-9A-Fa-f]{4}(?![0-9A-Fa-f])){4,})MH4N",
    R"MH4N(^\s*(?:[A-Za-z_.][\w.]*:)?(?:0[xX])?[0-9A-Fa-f]+\s*(?:<[^>\n]*>)?:?(?:\s+[0-9A-Fa-f]{4}(?:[0-9A-Fa-f]{4})?(?![0-9A-Fa-f])|(?:\s+[0-9A-Fa-f]{2}(?![0-9A-Fa-f])){1,8})\s+(?![0-9A-Fa-f]{2}(?:\s|$))[A-Za-z][A-Za-z0-9._]*(?:\s|$))MH4N",
    R"MH4N((?:0[xX][0-9A-Fa-f]{2}[\s,]+){15}0[xX][0-9A-Fa-f]{2}(?![0-9A-Fa-f]))MH4N",
};

/// The guard's trace-line limit (TRACE_LINE_LIMIT) and its dense-blob length (DENSE_LINE_MIN).
inline constexpr int kTraceLineLimit = 50;
inline constexpr int kDenseLineMin = 1024;
/// report_check.py's limits: the most lines, and the hex digits in a row at which a line is refused.
inline constexpr int kMaxReportLines = 650;
inline constexpr int kHexRunLimit = 64;
/// The longest line a report keeps (longer ones are cut after redaction).
inline constexpr int kMaxLineLength = 600;

}  // namespace mh4n::app::report
