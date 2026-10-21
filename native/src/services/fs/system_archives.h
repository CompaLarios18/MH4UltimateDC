// The open-source replacements of the system archives (N1-12): the bytes Azahar ships in
// externals/open_source_archives, embedded at build time from native/third_party/open_source_archives
// (see its PROVENANCE.md). Each is a RomFS image (it starts with the level 3 header, not the IVFC one).
#pragma once

namespace mh4n::fs::embedded {

extern const unsigned char kBadWordList[];
extern const unsigned long long kBadWordList_size;
extern const unsigned char kCountryList[];
extern const unsigned long long kCountryList_size;
extern const unsigned char kMii[];
extern const unsigned long long kMii_size;
extern const unsigned char kSharedFont[];
extern const unsigned long long kSharedFont_size;

}  // namespace mh4n::fs::embedded
