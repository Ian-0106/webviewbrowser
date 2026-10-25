#include "activity_store.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>

namespace {
constexpr std::int64_t kDayMs = 24LL * 60 * 60 * 1000;
constexpr std::int64_t kHistoryAgeMs = 90LL * kDayMs;
constexpr std::int64_t kDownloadAgeMs = 365LL * kDayMs;
constexpr std::size_t kHistoryMax = 10000;
constexpr std::size_t kDownloadMax = 5000;

std::string WideToUtf8Local(const std::wstring &value) {
    if (value.empty()) return {};
    int count = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                    static_cast<int>(value.size()), nullptr, 0,
                                    nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), count, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWideLocal(const std::string &value) {
    if (value.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                    static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), count);
    return result;
}

std::string Encode(const std::wstring &value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    const std::string bytes = WideToUtf8Local(value);
    std::string result;
    result.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 0x0f]);
    }
    return result;
}

bool DecodeNibble(char value, unsigned char &result) {
    if (value >= '0' && value <= '9') result = static_cast<unsigned char>(value - '0');
    else if (value >= 'A' && value <= 'F') result = static_cast<unsigned char>(value - 'A' + 10);
    else if (value >= 'a' && value <= 'f') result = static_cast<unsigned char>(value - 'a' + 10);
    else return false;
    return true;
}

bool Decode(const std::string &value, std::wstring &result) {
    if (value.size() % 2 != 0) return false;
    std::string bytes;
    bytes.reserve(value.size() / 2);
    for (std::size_t i = 0; i < value.size(); i += 2) {
        unsigned char high = 0, low = 0;
        if (!DecodeNibble(value[i], high) || !DecodeNibble(value[i + 1], low)) return false;
        bytes.push_back(static_cast<char>((high << 4) | low));
    }
    result = Utf8ToWideLocal(bytes);
    return bytes.empty() || !result.empty();
}

std::vector<std::string> Split(const std::string &line) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        std::size_t end = line.find('\t', start);
        parts.push_back(line.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return parts;
}

template <class T>
bool ParseNumber(const std::string &value, T &output) {
    try {
        std::size_t used = 0;
        long long number = std::stoll(value, &used);
        if (used != value.size()) return false;
        output = static_cast<T>(number);
        return true;
    } catch (...) {
        return false;
    }
}

bool HistoryNewest(const HistoryEntry &left, const HistoryEntry &right) {
    return left.visitedAt > right.visitedAt;
}

bool DownloadNewest(const DownloadEntry &left, const DownloadEntry &right) {
    return left.startedAt > right.startedAt;
}
}

ActivityStore::ActivityStore(std::filesystem::path filePath)
    : filePath_(std::move(filePath)) {}

bool ActivityStore::Load() {
    history_.clear();
    downloads_.clear();
    lastHistoryByTab_.clear();
    std::ifstream input(filePath_, std::ios::binary);
    if (!input) return true;

    std::string line;
    bool malformed = false;
    while (std::getline(input, line)) {
        auto fields = Split(line);
        if (fields.empty()) continue;
        bool parsed = false;
        if (fields[0] == "H" && fields.size() == 5) {
            HistoryEntry entry;
            if (Decode(fields[1], entry.id) && Decode(fields[2], entry.title) &&
                Decode(fields[3], entry.url) && ParseNumber(fields[4], entry.visitedAt)) {
                history_.push_back(std::move(entry));
                parsed = true;
            }
        } else if (fields[0] == "D" && fields.size() == 13) {
            DownloadEntry entry;
            if (Decode(fields[1], entry.id) && Decode(fields[2], entry.filename) &&
                Decode(fields[3], entry.url) && Decode(fields[4], entry.savePath) &&
                Decode(fields[5], entry.state) && ParseNumber(fields[6], entry.receivedBytes) &&
                ParseNumber(fields[7], entry.totalBytes) && ParseNumber(fields[8], entry.startedAt) &&
                ParseNumber(fields[9], entry.finishedAt) && Decode(fields[10], entry.interruptReason) &&
                Decode(fields[11], entry.retryOf)) {
                downloads_.push_back(std::move(entry));
                parsed = true;
            }
        }
        if (!line.empty() && !parsed) malformed = true;
    }
    input.close();
    if (malformed) {
        std::error_code copyError;
        auto evidence = filePath_;
        evidence += L".corrupt-" + std::to_wstring(NowMilliseconds());
        std::filesystem::copy_file(filePath_, evidence,
                                   std::filesystem::copy_options::overwrite_existing,
                                   copyError);
    }

    const std::int64_t now = NowMilliseconds();
    bool changed = malformed;
    for (auto &entry : downloads_) {
        if (IsActiveDownloadState(entry.state)) {
            entry.state = L"interrupted";
            if (entry.interruptReason.empty()) entry.interruptReason = L"Application exited before the download finished";
            entry.finishedAt = now;
            changed = true;
        }
    }
    const auto oldHistory = history_.size();
    const auto oldDownloads = downloads_.size();
    Prune(now);
    changed = changed || oldHistory != history_.size() || oldDownloads != downloads_.size();
    if (changed) Save();
    return true;
}

bool ActivityStore::Save() const {
    std::error_code ec;
    std::filesystem::create_directories(filePath_.parent_path(), ec);
    auto temporary = filePath_;
    temporary += L".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    for (const auto &entry : history_) {
        output << "H\t" << Encode(entry.id) << '\t' << Encode(entry.title) << '\t'
               << Encode(entry.url) << '\t' << entry.visitedAt << '\n';
    }
    for (const auto &entry : downloads_) {
        output << "D\t" << Encode(entry.id) << '\t' << Encode(entry.filename) << '\t'
               << Encode(entry.url) << '\t' << Encode(entry.savePath) << '\t'
               << Encode(entry.state) << '\t' << entry.receivedBytes << '\t'
               << entry.totalBytes << '\t' << entry.startedAt << '\t' << entry.finishedAt
               << '\t' << Encode(entry.interruptReason) << '\t' << Encode(entry.retryOf)
               << "\t0\n";
    }
    output.close();
    if (!output) return false;
    return MoveFileExW(temporary.c_str(), filePath_.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

void ActivityStore::Prune(std::int64_t now) {
    history_.erase(std::remove_if(history_.begin(), history_.end(), [&](const auto &entry) {
        return now - entry.visitedAt > kHistoryAgeMs;
    }), history_.end());
    std::sort(history_.begin(), history_.end(), HistoryNewest);
    if (history_.size() > kHistoryMax) history_.resize(kHistoryMax);

    std::vector<DownloadEntry> kept;
    kept.reserve(downloads_.size());
    std::size_t terminalCount = 0;
    std::sort(downloads_.begin(), downloads_.end(), DownloadNewest);
    for (auto &entry : downloads_) {
        if (IsActiveDownloadState(entry.state)) {
            kept.push_back(std::move(entry));
        } else if (terminalCount < kDownloadMax &&
                   now - (entry.finishedAt ? entry.finishedAt : entry.startedAt) <= kDownloadAgeMs) {
            kept.push_back(std::move(entry));
            ++terminalCount;
        }
    }
    downloads_ = std::move(kept);
    std::sort(downloads_.begin(), downloads_.end(), DownloadNewest);
}

std::wstring ActivityStore::RecordHistory(std::uintptr_t tabKey, const std::wstring &url,
                                          const std::wstring &title, std::int64_t now) {
    if (!IsWebUrl(url)) return {};
    auto previous = lastHistoryByTab_.find(tabKey);
    if (previous != lastHistoryByTab_.end()) {
        auto item = std::find_if(history_.begin(), history_.end(), [&](const auto &entry) {
            return entry.id == previous->second;
        });
        if (item != history_.end() && item->url == url) {
            item->title = title.empty() ? item->title : title;
            item->visitedAt = now;
            std::sort(history_.begin(), history_.end(), HistoryNewest);
            Save();
            return item->id;
        }
    }
    HistoryEntry entry{NewId(), title.empty() ? url : title, url, now};
    const std::wstring id = entry.id;
    history_.push_back(std::move(entry));
    lastHistoryByTab_[tabKey] = id;
    Prune(now);
    Save();
    return id;
}

bool ActivityStore::UpdateHistoryTitle(const std::wstring &id, const std::wstring &title) {
    if (title.empty()) return false;
    auto item = std::find_if(history_.begin(), history_.end(), [&](const auto &entry) { return entry.id == id; });
    if (item == history_.end() || item->title == title) return false;
    item->title = title;
    Save();
    return true;
}

std::size_t ActivityStore::RemoveHistory(const std::vector<std::wstring> &ids) {
    std::set<std::wstring> selected(ids.begin(), ids.end());
    const auto before = history_.size();
    history_.erase(std::remove_if(history_.begin(), history_.end(), [&](const auto &entry) {
        return selected.count(entry.id) != 0;
    }), history_.end());
    for (auto it = lastHistoryByTab_.begin(); it != lastHistoryByTab_.end();) {
        if (selected.count(it->second)) it = lastHistoryByTab_.erase(it); else ++it;
    }
    if (before != history_.size()) Save();
    return before - history_.size();
}

std::size_t ActivityStore::ClearHistory() {
    const auto count = history_.size();
    history_.clear();
    lastHistoryByTab_.clear();
    if (count) Save();
    return count;
}

std::wstring ActivityStore::AddDownload(DownloadEntry entry) {
    if (entry.id.empty()) entry.id = NewId();
    if (!entry.startedAt) entry.startedAt = NowMilliseconds();
    const std::wstring id = entry.id;
    downloads_.insert(downloads_.begin(), std::move(entry));
    Prune(NowMilliseconds());
    Save();
    return id;
}

DownloadEntry *ActivityStore::FindDownload(const std::wstring &id) {
    auto item = std::find_if(downloads_.begin(), downloads_.end(), [&](const auto &entry) { return entry.id == id; });
    return item == downloads_.end() ? nullptr : &*item;
}

const DownloadEntry *ActivityStore::FindDownload(const std::wstring &id) const {
    auto item = std::find_if(downloads_.begin(), downloads_.end(), [&](const auto &entry) { return entry.id == id; });
    return item == downloads_.end() ? nullptr : &*item;
}

std::size_t ActivityStore::RemoveDownloads(const std::vector<std::wstring> &ids, bool terminalOnly) {
    std::set<std::wstring> selected(ids.begin(), ids.end());
    const auto before = downloads_.size();
    downloads_.erase(std::remove_if(downloads_.begin(), downloads_.end(), [&](const auto &entry) {
        return selected.count(entry.id) && (!terminalOnly || !IsActiveDownloadState(entry.state));
    }), downloads_.end());
    if (before != downloads_.size()) Save();
    return before - downloads_.size();
}

std::size_t ActivityStore::ClearTerminalDownloads() {
    const auto before = downloads_.size();
    downloads_.erase(std::remove_if(downloads_.begin(), downloads_.end(), [&](const auto &entry) {
        return !IsActiveDownloadState(entry.state);
    }), downloads_.end());
    if (before != downloads_.size()) Save();
    return before - downloads_.size();
}

std::size_t ActivityStore::ActiveDownloadCount() const {
    return static_cast<std::size_t>(std::count_if(downloads_.begin(), downloads_.end(), [](const auto &entry) {
        return IsActiveDownloadState(entry.state);
    }));
}

bool ActivityStore::IsWebUrl(const std::wstring &url) {
    return url.rfind(L"https://", 0) == 0 || url.rfind(L"http://", 0) == 0;
}

bool ActivityStore::IsActiveDownloadState(const std::wstring &state) {
    return state == L"waiting" || state == L"progressing" || state == L"paused";
}

std::int64_t ActivityStore::NowMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::wstring ActivityStore::NewId() {
    static std::atomic<unsigned long long> sequence{0};
    return std::to_wstring(NowMilliseconds()) + L"-" + std::to_wstring(++sequence);
}

std::filesystem::path DefaultActivityStorePath() {
    PWSTR local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local))) {
        std::filesystem::path result(local);
        CoTaskMemFree(local);
        return result / L"WebView Browser" / L"activity.dat";
    }
    return std::filesystem::temp_directory_path() / L"WebView Browser" / L"activity.dat";
}
