// SPDX-License-Identifier: AGPL-3.0-or-later
#include "profiles.hpp"
#include <algorithm>
#include <fstream>
#include <set>
#include <stdexcept>
#include <tuple>
namespace slicer {
Json read_json(const fs::path &file) {
  std::ifstream in(file);
  if (!in)
    throw std::runtime_error("Cannot open " + file.string());
  return Json::parse(in);
}
void write_json_new(const fs::path &file, const Json &document) {
  if (fs::exists(file))
    throw std::runtime_error("Output already exists: " + file.string());
  std::ofstream out(file);
  if (!out)
    throw std::runtime_error("Cannot create " + file.string());
  out << document.dump(2) << '\n';
  if (!out)
    throw std::runtime_error("Cannot write " + file.string());
}
bool Profile::instantiated() const {
  auto it = document.find("instantiation");
  if (it == document.end())
    return true;
  return *it == true || *it == "true" || *it == 1;
}
void Profiles::scan(const std::vector<fs::path> &roots) {
  entries_.clear();
  warnings_.clear();
  std::set<fs::path> visited;
  for (const auto &root : roots) {
    if (!fs::is_directory(root))
      continue;
    for (const auto &entry :
         fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".json")
        continue;
      auto path = fs::weakly_canonical(entry.path());
      if (!visited.insert(path).second)
        continue;
      try {
        auto doc = read_json(path);
        if (!doc.is_object())
          continue;
        std::string type = doc.value("type", "");
        if (type.empty()) {
          auto parent = path.parent_path().filename().string();
          if (parent == "machine" || parent == "filament" || parent == "process")
            type = parent;
        }
        if (type != "machine" && type != "filament" && type != "process")
          continue;
        auto name = doc.value("name", path.stem().string());
        std::string vendor;
        for (auto dir = path.parent_path(); !dir.empty() && dir != dir.root_path();
             dir = dir.parent_path()) {
          if (fs::is_regular_file(dir.parent_path() / (dir.filename().string() + ".json"))) {
            vendor = dir.filename().string();
            break;
          }
        }
        entries_.push_back({path, name, type, vendor, std::move(doc)});
      } catch (const std::exception &ex) {
        warnings_.push_back(path.string() + ": " + ex.what());
      }
    }
  }
  std::sort(entries_.begin(), entries_.end(), [](const Profile &a, const Profile &b) {
    return std::tie(a.type, a.name, a.file) < std::tie(b.type, b.name, b.file);
  });
}
std::vector<const Profile *> Profiles::choices(const std::string &type,
                                               const std::string &printer) const {
  std::vector<const Profile *> result;
  for (const auto &p : entries_) {
    if (p.type != type || !p.instantiated())
      continue;
    auto it = p.document.find("compatible_printers");
    if (!printer.empty() && it != p.document.end() && it->is_array() && !it->empty() &&
        std::find(it->begin(), it->end(), printer) == it->end())
      continue;
    result.push_back(&p);
  }
  return result;
}
const Profile *Profiles::find(const fs::path &file) const {
  auto path = fs::weakly_canonical(file);
  for (const auto &p : entries_)
    if (p.file == path)
      return &p;
  return nullptr;
}
void Profiles::export_original(const Profile &p, const fs::path &destination) {
  if (fs::exists(destination))
    throw std::runtime_error("Output already exists: " + destination.string());
  fs::copy_file(p.file, destination, fs::copy_options::none);
}
} // namespace slicer
