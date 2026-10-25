#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

struct HistoryEntry {
    std::wstring id;
    std::wstring title;
    std::wstring url;
    std::int64_t visitedAt = 0;
};

struct DownloadEntry {
    std::wstring id;
    std::wstring filename;
    std::wstring url;
    std::wstring savePath;
    std::wstring state;
    std::int64_t receivedBytes = 0;
    std::int64_t totalBytes = 0;
    std::int64_t startedAt = 0;
    std::int64_t finishedAt = 0;
    std::wstring interruptReason;
    std::wstring retryOf;
};

class ActivityStore {
public:
    explicit ActivityStore(std::filesystem::path filePath);

    bool Load();
    bool Save() const;
    void Prune(std::int64_t now);

    std::wstring RecordHistory(std::uintptr_t tabKey, const std::wstring &url,
                               const std::wstring &title, std::int64_t now);
    bool UpdateHistoryTitle(const std::wstring &id, const std::wstring &title);
    std::size_t RemoveHistory(const std::vector<std::wstring> &ids);
    std::size_t ClearHistory();

    std::wstring AddDownload(DownloadEntry entry);
    DownloadEntry *FindDownload(const std::wstring &id);
    const DownloadEntry *FindDownload(const std::wstring &id) const;
    std::size_t RemoveDownloads(const std::vector<std::wstring> &ids,
                                bool terminalOnly);
    std::size_t ClearTerminalDownloads();
    std::size_t ActiveDownloadCount() const;

    const std::vector<HistoryEntry> &History() const { return history_; }
    const std::vector<DownloadEntry> &Downloads() const { return downloads_; }
    std::vector<DownloadEntry> &MutableDownloads() { return downloads_; }
    const std::filesystem::path &FilePath() const { return filePath_; }

    static bool IsWebUrl(const std::wstring &url);
    static bool IsActiveDownloadState(const std::wstring &state);
    static std::int64_t NowMilliseconds();
    static std::wstring NewId();

private:
    std::filesystem::path filePath_;
    std::vector<HistoryEntry> history_;
    std::vector<DownloadEntry> downloads_;
    std::unordered_map<std::uintptr_t, std::wstring> lastHistoryByTab_;
};

std::filesystem::path DefaultActivityStorePath();
