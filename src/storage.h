#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bf {

struct BackupFile {
    std::string path;
    std::string name;
    uint64_t bytes = 0;
    int64_t mtime = 0;
    int lines = 0;

    std::string sizeText() const;
    std::string dateText() const;
};

// App-private storage under XDG_DATA_HOME (or ~/.local/share), overridable with
// BFCLI_DATA_DIR. Nothing is written outside this tree, and the boot partition
// and other applications' files are never touched.
class Storage {
public:
    bool init(std::string& error);
    const std::string& dataDir() const { return dataDir_; }
    const std::string& backupDir() const { return backupDir_; }
    const std::string& diagnosticDir() const { return diagnosticDir_; }
    // Mesh conversations, one plain-text file per peer.
    const std::string& meshDir() const { return meshDir_; }
    std::vector<std::string> listMeshChatFiles() const;
    std::string configPath() const;
    std::string historyPath() const;
    // Saved places: the car, the launch pad, a crash site. One file, beside
    // the config, because it is the one thing worth copying off by hand.
    std::string marksPath() const;

    // Durable replacement: temp file, fsync, rename, then fsync the directory.
    bool writeAtomic(const std::string& path, const std::string& content,
                     std::string& error) const;
    bool readFile(const std::string& path, std::string& out, std::string& error) const;

    std::vector<BackupFile> listBackups() const;
    bool deleteBackup(const std::string& path, std::string& error) const;
    bool deleteMeshChat(const std::string& path, std::string& error) const;

    // Configurator-compatible name: BTFL_cli_<craft>_<stamp>_<board>_backup.txt
    std::string makeBackupName(const std::string& craft, const std::string& board) const;
    // Field-check reports are kept away from restorable backup files.
    std::string makeDiagnosticName(const std::string& craft, const std::string& board) const;

    // Free space on the backup filesystem, or 0 when it cannot be determined.
    uint64_t freeBytes() const;

    bool saveHistory(const std::vector<std::string>& lines) const;
    std::vector<std::string> loadHistory() const;

private:
    std::string dataDir_;
    std::string backupDir_;
    std::string diagnosticDir_;
    std::string meshDir_;
};

// A tiny `key = value` config file. Unknown keys are preserved on rewrite so a
// hand-edited file does not lose comments' worth of intent.
class Config {
public:
    void load(const Storage& s);
    bool save(const Storage& s, std::string& error) const;

    std::string get(const std::string& key, const std::string& fallback = "") const;
    int getInt(const std::string& key, int fallback) const;
    bool getBool(const std::string& key, bool fallback) const;
    void set(const std::string& key, const std::string& value);
    void setInt(const std::string& key, int value);
    void setBool(const std::string& key, bool value);
    const std::map<std::string, std::string>& all() const { return values_; }

private:
    std::map<std::string, std::string> values_;
};

// mkdir -p, 0700. Succeeds when the path already exists.
bool makeDirs(const std::string& path, std::string& error);

std::string humanBytes(uint64_t n);
// strftime in the local zone, with whatever format strftime takes.
std::string formatLocalTime(int64_t epochSeconds, const char* format);
std::string timestampCompact();          // 20260831_135905
// The first line of a small text file -- a sysfs attribute, usually -- with
// trailing whitespace removed. Empty when the file cannot be read.
std::string readFirstLine(const std::string& path);
std::string sanitizeForFilename(const std::string& s);

} // namespace bf
