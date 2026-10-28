#pragma once
// Clip Director: library loading (libobs side). Reads cd-ingest's index when present.
#include <string>
#include <vector>

#include "types.h"

namespace cdobs {

struct Library {
	std::vector<cd::Folder> folders;
	std::vector<cd::Clip> clips;
	std::vector<std::string> names;  // display names, parallel to clips
	std::vector<std::string> thumbs; // thumbnail paths (ingest), "" if none, parallel to clips
	int with_stats = 0;
	std::vector<std::string> notes; // one line per folder for the log
};

// For each folder: SRC/_cd/.clipdirector/index.json (ingest output) wins; a folder that *is* an
// ingest output (has .clipdirector/index.json) works too; otherwise the folder's videos are used
// as-is, without stats (start points on a 1 s grid, levels measured live).
Library load_library(const std::vector<cd::Folder> &folders);

} // namespace cdobs
