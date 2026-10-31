// Paths for the launcher's own file browser (browse.h): a folder's entries in the order it shows
// them, the places to start from (home, Desktop, Downloads, removable drives such as a Steam
// Deck's SD card, the drive letters) and a path as typed or pasted. The standard library and the
// system's own calls only, so that a test builds it on its own (tests/test_app_paths.py).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Launcher::Paths {

/// A folder or a file of a listing.
struct Entry {
    std::string name;
    std::string path;
    bool folder = false;
    std::uint64_t size = 0;  ///< a file's size in bytes
};

/// A folder's entries.
struct Listing {
    std::string folder;          ///< the folder listed (Clean)
    std::vector<Entry> entries;  ///< its folders, then its files, each in name order (NameLess)
    std::size_t left_out = 0;    ///< entries past the most a listing holds
    std::string error;           ///< why it couldn't be read ("" when it could)
};

/// The folders in `folder` and, with `files`, its files with one of `extensions` ("3ds", in any
/// case; all of them when there are none). Hidden ones are left out: names starting with a dot,
/// and on Windows what Explorer hides.
Listing List(const std::string& folder, const std::vector<std::string>& extensions, bool files);

/// Whether `name` ends in a dot and one of `extensions`, in any case (true when there are none).
bool HasExtension(const std::string& name, const std::vector<std::string>& extensions);

/// Names in the order a person expects: case aside, and numbers by their value ("Mod 2" before
/// "Mod 10").
bool NameLess(const std::string& a, const std::string& b);

/// `path` absolute and tidied: no "." or ".." parts, no separator at the end but a root's, and
/// on Windows its own separators. Empty stays empty.
std::string Clean(const std::string& path);

/// The folder `path` is in ("" for a root: "/", "C:\").
std::string Parent(const std::string& path);

/// The last part of `path` (a root's whole path for a root).
std::string Name(const std::string& path);

enum class Kind { Missing, Folder, File };
/// What is at `path`.
Kind What(const std::string& path);

/// A path as typed or pasted: spaces and a pair of quotes around it left out, a file:// address
/// (a file manager's copy) read as its path, ~ as `home`, a relative one from `base`; on Linux
/// and macOS a shell's backslashes ("My\ Games") when the path is only there without them.
/// Empty when nothing was typed.
std::string FromTyped(const std::string& text, const std::string& base, const std::string& home);

/// The home folder ("" if there is none).
std::string Home();

/// A place the browser can start from.
struct Place {
    std::string label;
    std::string path;
    bool drive = false;  ///< a drive (removable, the root, a drive letter): listed after the others
};

/// Home, Desktop and Downloads (by the desktop's own names for them), the removable drives
/// (Linux: what is mounted under /run/media, /media and /mnt, a Steam Deck's SD card among them;
/// a Mac: /Volumes), the root, and on Windows the drive letters. Only folders that are there.
std::vector<Place> Places();

/// XDG's folder `key` ("DESKTOP", "DOWNLOAD") as user-dirs.dirs (its `text`) names it, $HOME
/// read as `home`; "" when it doesn't name it.
std::string UserDir(const std::string& text, const std::string& key, const std::string& home);

/// The removable drives in a mount table (the text of /proc/mounts): mount points under
/// /run/media, /media and /mnt, by their folder's name; an SD card (a /dev/mmcblk device) as
/// "SD card".
std::vector<Place> Mounted(const std::string& mounts);

} // namespace Launcher::Paths
