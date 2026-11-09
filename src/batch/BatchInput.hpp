#pragma once
#include <string>
#include <vector>

namespace vr {

// One .dem file queued for processing. For demos found inside a .zip
// archive, `tempExtractedPath` holds the path of the copy extracted to a
// temp directory (which the caller should clean up after use);
// `displayName` is what to show the user (original name inside the
// archive, or just the plain filename for on-disk demos).
struct BatchInputItem {
    std::string path;          // actual filesystem path to read the .dem from
    std::string displayName;   // name to show in UI/output (may differ from path for zip entries)
    bool fromZip = false;
};

struct BatchInputResult {
    std::vector<BatchInputItem> items;
    std::vector<std::string> tempDirsToCleanup; // temp extraction dirs, remove_all these when done
    std::string error;
};

// Resolves a single user-provided path (via drag&drop or CLI arg) into a
// flat list of .dem files to process:
//   - a single .dem file                 -> that one file
//   - a directory                        -> every .dem found recursively inside it
//   - a .zip archive                     -> every .dem entry, extracted to a temp dir
// Returns ok with an empty item list (not an error) if nothing matched.
BatchInputResult ResolveBatchInput(const std::string& inputPathUtf8);

} // namespace vr
