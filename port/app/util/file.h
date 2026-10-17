// File helpers of the app: whether a path exists, creating folders, reading and writing a whole
// file, listing a folder. Paths are UTF-8 strings (on Windows they are converted for the system).
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace App::Util {

/// Whether something (a file or a folder) is at `path`. A folder's path may end in '/'.
bool Exists(const std::string& path);

bool IsDirectory(const std::string& path);

/// Creates every missing folder of `path` up to its last separator: "a/b/c.txt" makes a/ and
/// a/b/, "a/b/" makes both too. True when they exist afterwards.
bool CreateFullPath(const std::string& path);

/// Writes `text` as the whole content of the file, replacing it, and returns the number of bytes
/// written (0 when the file couldn't be written, and for empty text). `text_file` opens it in
/// text mode, which on Windows turns each '\n' into "\r\n".
std::size_t WriteStringToFile(bool text_file, const std::string& path, std::string_view text);

/// Reads the whole file into `text` and returns the number of bytes read; 0, with `text`
/// untouched, when the file can't be opened. `text_file` opens it in text mode.
std::size_t ReadFileToString(bool text_file, const std::string& path, std::string& text);

/// The names of the entries of a folder ("." and ".." left out), in the system's order. False
/// when the folder can't be read.
bool ListDirectory(const std::string& path, std::vector<std::string>& names);

} // namespace App::Util
