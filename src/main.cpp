#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include "batch/BatchInput.hpp"
#include "batch/CliOptions.hpp"
#include "batch/Dedup.hpp"
#include "batch/DemoProcessing.hpp"
#include "batch/PathUtf8.hpp"
#include "batch/ReportPrinter.hpp"

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> utf8Args = vr::GetUtf8Argv();
    argc = static_cast<int>(utf8Args.size());
#else
    std::vector<std::string> utf8Args(argv, argv + argc);
#endif

    vr::CliOptions o;
    int exitCode = 0;
    if (!vr::ParseCliArgs(argc, utf8Args, o, exitCode)) {
        return exitCode;
    }

#ifndef VR_WITH_MP3
    if (o.writeMp3 && !o.quiet) {
        std::cout << "  note: built without libmp3lame, --mp3 ignored (WAV still written)\n";
    }
#endif

    if (!o.quiet) {
        vr::PrintBanner();
    }

    // Resolve every dropped/passed input (each could itself be a demo, a
    // folder, or a zip) and merge them all into one flat batch, so e.g.
    // dragging 3 .dem files onto the .exe at once processes all 3 and
    // prints one combined summary, instead of only looking at argv[1]
    // and silently ignoring (or erroring out on) the rest.
    vr::BatchInputResult batch;
    for (auto& inputPath : o.inputs) {
        vr::BatchInputResult one = vr::ResolveBatchInput(inputPath);
        if (!one.error.empty()) {
            std::cout << "  [!] " << one.error << "\n";
            continue; // keep going with whatever other inputs were given
        }
        batch.items.insert(batch.items.end(), one.items.begin(), one.items.end());
        batch.tempDirsToCleanup.insert(batch.tempDirsToCleanup.end(),
                                        one.tempDirsToCleanup.begin(), one.tempDirsToCleanup.end());
    }
    if (batch.items.empty()) {
        std::cout << "  no .dem files found in the given input(s).\n";
        if (!o.noAutoClose) std::this_thread::sleep_for(std::chrono::seconds(o.autoCloseSeconds));
        return 0;
    }

    // Output always goes next to voiceripper.exe itself, never next to
    // wherever the input happened to be (a dragged-in folder, a zip
    // sitting on the desktop, etc.) -- this is intentional: the user
    // wants one predictable place to find results regardless of where
    // they pulled the input from. Each demo gets its own
    // "<demoname>_voice" folder created directly next to the exe (no
    // extra shared wrapper folder in between), matching the original
    // single-file behaviour.
    std::string exeDir = vr::GetExecutableDirUtf8();
    std::string outRootUtf8 = o.haveOutDir ? o.outDir : exeDir;

    // Skip demos whose exact file contents were already processed in a
    // previous run (hash-based, not filename-based, so a renamed copy or
    // the same demo seen both loose and inside a .zip is still caught).
    // The manifest lives next to the exe, not in outRootUtf8, so it keeps
    // working even if --out points somewhere different on a later run.
    std::string manifestPath = (exeDir.empty() ? "" : (exeDir + "/")) + "voiceripper_done.txt";
    vr::ProcessedDemoSet processedSet(manifestPath);

    if (!o.force) {
        std::vector<vr::BatchInputItem> keep;
        keep.reserve(batch.items.size());
        size_t skipped = 0;
        for (auto& item : batch.items) {
            uint64_t h = vr::HashFileContents(item.path);
            if (h != 0 && processedSet.Contains(h)) {
                skipped++;
                continue;
            }
            keep.push_back(item);
        }
        if (skipped > 0 && !o.quiet) {
            std::cout << "  skipping " << skipped << " demo(s) already processed before"
                      << " (use --force to redo them)\n";
        }
        batch.items = std::move(keep);
    }

    if (batch.items.empty()) {
        std::cout << "  nothing new to process (everything was already done before).\n";
        if (!o.noAutoClose) std::this_thread::sleep_for(std::chrono::seconds(o.autoCloseSeconds));
        return 0;
    }

    // Each demo is fully independent (own DemoFile, own codec instances,
    // own output folder) so they can safely run in parallel -- this is
    // what actually made batch runs slow before: N demos were processed
    // one at a time even though the machine has plenty of spare cores.
    // Cap worker count at (cores - 1, min 1) so the machine stays
    // responsive, and never spin up more workers than there are demos.
    unsigned hwThreads = std::thread::hardware_concurrency();
    size_t workerCount = std::max<size_t>(1, hwThreads > 1 ? hwThreads - 1 : 1);
    workerCount = std::min(workerCount, batch.items.size());

    if (!o.quiet) {
        std::cout << "  input: " << o.inputs.size() << " item(s) given  ->  "
                  << batch.items.size() << " demo(s) found\n";
        std::cout << "  output: " << (outRootUtf8.empty() ? "." : outRootUtf8) << "\\<demoname>_voice\\\n";
        std::cout << "  codecs: speex " << (o.extract.useSpeex ? "on" : "off")
                  << ", silk " << (o.extract.useSilk ? "on" : "off")
                  << ", opus " << (o.extract.useOpus ? "on" : "off") << "\n";
        std::cout << "  normalize: " << (o.extract.normalize ? "on" : "off") << "\n";
        std::cout << "  parallel workers: " << workerCount << "\n";
    }

    std::vector<vr::DemoSummary> summaries(batch.items.size());

    std::atomic<size_t> nextIndex{0};
    std::mutex printMutex;

    auto worker = [&]() {
        while (true) {
            size_t i = nextIndex.fetch_add(1);
            if (i >= batch.items.size()) break;

            auto& item = batch.items[i];
            std::string demoOutDir = (outRootUtf8.empty() ? "" : (outRootUtf8 + "/"))
                                    + vr::Utf8Stem(item.displayName) + "_voice";

            vr::DemoSummary s = vr::ProcessOneDemo(item.path, item.displayName, demoOutDir, o);

            std::lock_guard<std::mutex> lock(printMutex);
            vr::PrintDemoSummary(s, demoOutDir, o.quiet);
            if (s.ok) {
                uint64_t h = vr::HashFileContents(item.path);
                if (h != 0) processedSet.Add(h);
            }
            summaries[i] = std::move(s);
        }
    };

    if (workerCount <= 1 || batch.items.size() <= 1) {
        worker();
    } else {
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for (size_t w = 0; w < workerCount; w++) {
            workers.emplace_back(worker);
        }
        for (auto& t : workers) t.join();
    }

    for (auto& tempDir : batch.tempDirsToCleanup) {
        std::error_code ec;
        std::filesystem::remove_all(vr::PathFromUtf8(tempDir), ec);
    }

    processedSet.Save();

    if (!o.quiet) {
        size_t okCount = 0, failCount = 0, emptyCount = 0, warnCount = 0;
        for (auto& s : summaries) {
            if (!s.ok) failCount++;
            else if (s.written.empty()) emptyCount++;
            else okCount++;
            if (!s.warning.empty()) warnCount++;
        }
        std::cout << "\n  ----------------------------------------\n";
        std::cout << "  " << summaries.size() << " demo(s) processed: "
                  << okCount << " with voice, " << emptyCount << " empty, " << failCount << " failed";
        if (warnCount > 0) std::cout << ", " << warnCount << " with warnings";
        std::cout << "\n";
        std::cout << "  all output saved under: " << outRootUtf8 << "\n";
        std::cout << "\n  done.\n";
        if (!o.noAutoClose) {
            std::cout << "  (closing in " << o.autoCloseSeconds << "s...)\n";
        }
    }

    if (!o.noAutoClose) {
        std::this_thread::sleep_for(std::chrono::seconds(o.autoCloseSeconds));
    }
    return 0;
}
