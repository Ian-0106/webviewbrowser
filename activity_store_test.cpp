#include "activity_store.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      (L"webview-browser-store-test-" + ActivityStore::NewId());
    const auto file = root / L"activity.dat";
    std::filesystem::create_directories(root);

    {
        ActivityStore store(file);
        assert(store.Load());
        assert(store.RecordHistory(1, L"file:///tmp/test.html", L"Local", 100).empty());
        const auto first = store.RecordHistory(1, L"https://example.com", L"Example", 200);
        const auto repeated = store.RecordHistory(1, L"https://example.com", L"Updated", 300);
        const auto otherTab = store.RecordHistory(2, L"https://example.com", L"Example", 400);
        assert(first == repeated);
        assert(first != otherTab);
        assert(store.History().size() == 2);

        DownloadEntry done;
        done.filename = L"done.zip";
        done.state = L"completed";
        done.startedAt = 200;
        done.finishedAt = 300;
        store.AddDownload(done);
        DownloadEntry active;
        active.filename = L"active.zip";
        active.state = L"progressing";
        active.startedAt = ActivityStore::NowMilliseconds();
        store.AddDownload(active);
    }

    {
        ActivityStore restarted(file);
        assert(restarted.Load());
        bool sawInterrupted = false;
        for (const auto &entry : restarted.Downloads()) {
            if (entry.filename == L"active.zip") sawInterrupted = entry.state == L"interrupted";
        }
        assert(sawInterrupted);
        restarted.ClearTerminalDownloads();
        assert(restarted.Downloads().empty());
    }

    {
        const auto corrupt = root / L"corrupt.dat";
        std::ofstream output(corrupt, std::ios::binary);
        output << "not-a-valid-record\n";
        output.close();
        ActivityStore recovered(corrupt);
        assert(recovered.Load());
        assert(recovered.History().empty());
        bool foundEvidence = false;
        for (const auto &item : std::filesystem::directory_iterator(root)) {
            if (item.path().filename().wstring().find(L"corrupt.dat.corrupt-") == 0) {
                foundEvidence = true;
            }
        }
        assert(foundEvidence);
    }

    std::filesystem::remove_all(root);
    std::cout << "activity_store_test passed\n";
    return 0;
}
