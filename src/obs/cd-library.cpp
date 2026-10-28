#include "cd-library.h"

#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>

namespace cdobs {

namespace {

bool is_video(const char *name)
{
	const char *dot = strrchr(name, '.');
	if (!dot)
		return false;
	std::string ext(dot + 1);
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return ext == "mp4" || ext == "mov" || ext == "mkv" || ext == "webm" || ext == "m4v";
}

std::string join(const std::string &a, const std::string &b)
{
	if (a.empty())
		return b;
	const char last = a.back();
	return (last == '/' || last == '\\') ? a + b : a + "/" + b;
}

bool load_index(const std::string &cd_dir, int folder, Library &lib)
{
	const std::string path = join(join(cd_dir, ".clipdirector"), "index.json");
	if (!os_file_exists(path.c_str()))
		return false;
	obs_data_t *root = obs_data_create_from_json_file(path.c_str());
	if (!root)
		return false;
	obs_data_t *clips = obs_data_get_obj(root, "clips");
	int n = 0;
	if (clips) {
		for (obs_data_item_t *it = obs_data_first(clips); it; obs_data_item_next(&it)) {
			obs_data_t *e = obs_data_item_get_obj(it);
			if (!e)
				continue;
			const char *file = obs_data_get_string(e, "file");
			const std::string full = join(cd_dir, file);
			if (*file && os_file_exists(full.c_str())) {
				cd::Clip c;
				c.path = full;
				c.folder = folder;
				c.duration = obs_data_get_double(e, "duration");
				c.keyframe_s = obs_data_get_double(e, "keyframe_s");
				obs_data_t *s = obs_data_get_obj(e, "stats");
				if (s) {
					c.stats.has = true;
					c.stats.mean = obs_data_get_double(s, "mean");
					c.stats.std = obs_data_get_double(s, "std");
					c.stats.p02 = obs_data_get_double(s, "p02");
					c.stats.p50 = obs_data_get_double(s, "p50");
					c.stats.p98 = obs_data_get_double(s, "p98");
					c.stats.dark = obs_data_get_double(s, "dark");
					obs_data_release(s);
					lib.with_stats++;
				}
				lib.clips.push_back(c);
				std::string nm = file;
				const size_t dot = nm.rfind('.');
				lib.names.push_back(dot == std::string::npos ? nm : nm.substr(0, dot));
				const char *th = obs_data_get_string(e, "thumb");
				const std::string thp = *th ? join(join(cd_dir, ".clipdirector"), th) : std::string();
				lib.thumbs.push_back(!thp.empty() && os_file_exists(thp.c_str()) ? thp : std::string());
				n++;
			}
			obs_data_release(e);
		}
		obs_data_release(clips);
	}
	obs_data_release(root);
	char buf[512];
	snprintf(buf, sizeof(buf), "folder %d: %d clips from ingest index %s", folder, n, path.c_str());
	lib.notes.push_back(buf);
	return n > 0;
}

void scan_raw(const std::string &dir, int folder, Library &lib)
{
	os_dir_t *d = os_opendir(dir.c_str());
	int n = 0;
	if (d) {
		std::vector<std::string> files;
		for (struct os_dirent *ent = os_readdir(d); ent; ent = os_readdir(d))
			if (!ent->directory && is_video(ent->d_name))
				files.push_back(ent->d_name);
		os_closedir(d);
		std::sort(files.begin(), files.end());
		for (const auto &f : files) {
			cd::Clip c;
			c.path = join(dir, f);
			c.folder = folder;
			c.duration = 0.0; // unknown: the picker starts it at 0 and lets it loop
			lib.clips.push_back(c);
			const size_t dot = f.rfind('.');
			lib.names.push_back(dot == std::string::npos ? f : f.substr(0, dot));
			lib.thumbs.emplace_back();
			n++;
		}
	}
	char buf[512];
	snprintf(buf, sizeof(buf), "folder %d: %d raw clips (no ingest stats) in %s", folder, n, dir.c_str());
	lib.notes.push_back(buf);
}

} // namespace

Library load_library(const std::vector<cd::Folder> &folders)
{
	Library lib;
	lib.folders = folders;
	for (int i = 0; i < static_cast<int>(folders.size()); ++i) {
		const std::string &dir = folders[i].path;
		if (load_index(join(dir, "_cd"), i, lib))
			continue;
		if (load_index(dir, i, lib))
			continue;
		scan_raw(dir, i, lib);
	}
	return lib;
}

} // namespace cdobs
