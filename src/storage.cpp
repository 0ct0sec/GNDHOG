#include "storage.h"
#include "strutil.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/statvfs.h>
#endif

namespace bf {
namespace {

std::string envOr(const char* name, const std::string& fallback) {
    const char* v = ::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

bool fsyncDir(const std::string& dir) {
    const int fd = ::open(dir.c_str(), O_RDONLY);
    if (fd < 0) return false;
    const bool ok = (::fsync(fd) == 0);
    ::close(fd);
    return ok;
}

// <prefix>[_craft]_<stamp>[_board]<suffix>: the Configurator's own shape.
std::string stampedName(const char* prefix, const std::string& craft,
                        const std::string& board, const char* suffix) {
    std::string name = prefix;
    const std::string c = sanitizeForFilename(craft);
    if (!c.empty()) name += "_" + c;
    name += "_" + timestampCompact();
    const std::string b = sanitizeForFilename(board);
    if (!b.empty()) name += "_" + b;
    return name + suffix;
}

// Only a path inside our own directory is ever unlinked, and never one that
// walks out of it. A transcript that is already gone is not an error; a
// backup that is already gone is, because the operator just asked for it.
bool deleteInside(const std::string& dir, const char* what, const std::string& path,
                  bool missingOk, std::string& error) {
    if (path.rfind(dir + "/", 0) != 0 || path.find("..") != std::string::npos) {
        error = std::string("refusing to delete a path outside the ") + what + " directory";
        return false;
    }
    if (::unlink(path.c_str()) != 0 && !(missingOk && errno == ENOENT)) {
        error = std::string("unlink: ") + std::strerror(errno);
        return false;
    }
    return true;
}

} // namespace

bool makeDirs(const std::string& path, std::string& error) {
    std::string acc;
    if (!path.empty() && path[0] == '/') acc = "/";
    for (const std::string& part : splitFields(path, '/')) {
        if (part.empty()) continue;
        acc += part;
        if (::mkdir(acc.c_str(), 0700) != 0 && errno != EEXIST) {
            error = acc + ": " + std::strerror(errno);
            return false;
        }
        acc += "/";
    }
    return true;
}

std::string humanBytes(uint64_t n) {
    char buf[32];
    if (n < 1024) std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(n));
    else if (n < 1024ull * 1024) std::snprintf(buf, sizeof(buf), "%.1f KB", n / 1024.0);
    else if (n < 1024ull * 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.1f MB", n / (1024.0 * 1024));
    else std::snprintf(buf, sizeof(buf), "%.2f GB", n / (1024.0 * 1024 * 1024));
    return buf;
}

std::string formatLocalTime(int64_t epochSeconds, const char* format) {
    const std::time_t t = static_cast<std::time_t>(epochSeconds);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    ::localtime_r(&t, &tmv);
#endif
    char buf[64];
    if (std::strftime(buf, sizeof(buf), format, &tmv) == 0) return {};
    return buf;
}

std::string timestampCompact() {
    return formatLocalTime(static_cast<int64_t>(std::time(nullptr)), "%Y%m%d_%H%M%S");
}

std::string sanitizeForFilename(const std::string& s) {
    std::string out;
    for (char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.') {
            out += c;
        } else if (c == ' ') {
            out += '_';
        }
        if (out.size() >= 40) break;
    }
    while (!out.empty() && (out.back() == '_' || out.back() == '.')) out.pop_back();
    return out;
}

std::string BackupFile::sizeText() const { return humanBytes(bytes); }

std::string BackupFile::dateText() const { return formatLocalTime(mtime, "%Y-%m-%d %H:%M"); }

bool Storage::init(std::string& error) {
    const std::string home = envOr("HOME", ".");
    const std::string xdg = envOr("XDG_DATA_HOME", home + "/.local/share");
    dataDir_ = envOr("BFCLI_DATA_DIR", xdg + "/bfcli");
    backupDir_ = dataDir_ + "/backups";
    diagnosticDir_ = dataDir_ + "/diagnostics";
    meshDir_ = dataDir_ + "/mesh";
    if (!makeDirs(backupDir_, error)) return false;
    if (!makeDirs(diagnosticDir_, error)) return false;
    if (!makeDirs(meshDir_, error)) return false;
    return true;
}

std::vector<std::string> Storage::listMeshChatFiles() const {
    std::vector<std::string> out;
    for (const std::string& name : listDirectory(meshDir_)) {
        if (name.size() < 6 || !endsWith(name, ".chat")) continue;
        struct stat st{};
        if (::stat((meshDir_ + "/" + name).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        out.push_back(name);
    }
    return out;
}

std::string Storage::configPath() const { return dataDir_ + "/config.ini"; }
std::string Storage::historyPath() const { return dataDir_ + "/history.txt"; }
std::string Storage::marksPath() const { return dataDir_ + "/marks.txt"; }

bool Storage::writeAtomic(const std::string& path, const std::string& content,
                          std::string& error) const {
    const size_t slash = path.find_last_of('/');
    const std::string dir = slash == std::string::npos ? std::string(".") : path.substr(0, slash);
    const std::string tmp = path + ".tmp";

    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        error = tmp + ": " + std::strerror(errno);
        return false;
    }
    // A temp file that could not be completed is not left behind.
    const auto abandon = [&](const char* what) {
        error = std::string(what) + ": " + std::strerror(errno);
        ::close(fd);
        ::unlink(tmp.c_str());
        return false;
    };
    size_t off = 0;
    while (off < content.size()) {
        const ssize_t n = ::write(fd, content.data() + off, content.size() - off);
        if (n > 0) { off += static_cast<size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        return abandon("write");
    }
    if (::fsync(fd) != 0) return abandon("fsync");
    ::close(fd);
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        error = std::string("rename: ") + std::strerror(errno);
        ::unlink(tmp.c_str());
        return false;
    }
    fsyncDir(dir);   // best effort: the rename is already durable on ext4
    return true;
}

bool Storage::readFile(const std::string& path, std::string& out, std::string& error) const {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = path + ": " + std::strerror(errno);
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

std::string readFirstLine(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::string line;
    std::getline(f, line);
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
    return line;
}

std::vector<std::string> listDirectory(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = ::opendir(dir.c_str());
    if (!d) return out;
    while (dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        out.push_back(name);
    }
    ::closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<BackupFile> Storage::listBackups() const {
    std::vector<BackupFile> out;
    for (const std::string& name : listDirectory(backupDir_)) {
        if (!endsWith(name, ".txt")) continue;
        BackupFile b;
        b.name = name;
        b.path = backupDir_ + "/" + name;
        struct stat st{};
        if (::stat(b.path.c_str(), &st) == 0) {
            if (!S_ISREG(st.st_mode)) continue;
            b.bytes = static_cast<uint64_t>(st.st_size);
            b.mtime = static_cast<int64_t>(st.st_mtime);
        }
        out.push_back(std::move(b));
    }
    std::sort(out.begin(), out.end(),
              [](const BackupFile& a, const BackupFile& b) { return a.mtime > b.mtime; });
    return out;
}

bool Storage::deleteMeshChat(const std::string& path, std::string& error) const {
    return deleteInside(meshDir_, "mesh", path, true, error);
}

bool Storage::deleteBackup(const std::string& path, std::string& error) const {
    return deleteInside(backupDir_, "backup", path, false, error);
}

std::string Storage::makeBackupName(const std::string& craft, const std::string& board) const {
    return stampedName("BTFL_cli", craft, board, "_backup.txt");
}

std::string Storage::makeDiagnosticName(const std::string& craft, const std::string& board) const {
    return stampedName("GNDHOG_fieldcheck", craft, board, ".txt");
}

uint64_t Storage::freeBytes() const {
#if defined(__linux__)
    struct statvfs st{};
    if (::statvfs(backupDir_.c_str(), &st) != 0) return 0;
    return static_cast<uint64_t>(st.f_bavail) * st.f_frsize;
#else
    return 0;
#endif
}

bool Storage::saveHistory(const std::vector<std::string>& lines) const {
    std::string blob;
    const size_t start = lines.size() > 100 ? lines.size() - 100 : 0;
    for (size_t i = start; i < lines.size(); ++i) {
        blob += lines[i];
        blob += '\n';
    }
    std::string err;
    return writeAtomic(historyPath(), blob, err);
}

std::vector<std::string> Storage::loadHistory() const {
    std::vector<std::string> out;
    std::ifstream f(historyPath());
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

// ------------------------------------------------------------------- Config

void Config::load(const Storage& s) {
    std::ifstream f(s.configPath());
    std::string line;
    while (std::getline(f, line)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        values_[trim(t.substr(0, eq))] = trim(t.substr(eq + 1));
    }
}

bool Config::save(const Storage& s, std::string& error) const {
    std::string blob =
        "# GNDHOG ZERO configuration (legacy bfcli data directory)\n"
        "# sym.<scan> overrides the character produced by the Sym layer for one\n"
        "# physical key; find a key's scan code in the app under Menu > Keymap.\n"
        "# Example:  sym.0x28 = _\n\n";
    for (const auto& kv : values_) {
        blob += kv.first + " = " + kv.second + "\n";
    }
    return s.writeAtomic(s.configPath(), blob, error);
}

std::string Config::get(const std::string& key, const std::string& fallback) const {
    const auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second;
}

int Config::getInt(const std::string& key, int fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    try {
        return std::stoi(it->second, nullptr, 0);
    } catch (...) {
        return fallback;
    }
}

bool Config::getBool(const std::string& key, bool fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    const std::string v = it->second;
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

double Config::getDouble(const std::string& key, double fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    char* end = nullptr;
    const double value = std::strtod(it->second.c_str(), &end);
    return end == it->second.c_str() ? fallback : value;
}

void Config::set(const std::string& key, const std::string& value) { values_[key] = value; }
void Config::setInt(const std::string& key, int value) { values_[key] = std::to_string(value); }
void Config::setBool(const std::string& key, bool value) { values_[key] = value ? "1" : "0"; }

void Config::setDouble(const std::string& key, double value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", value);
    values_[key] = buf;
}

} // namespace bf
