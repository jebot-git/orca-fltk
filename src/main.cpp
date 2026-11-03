// SPDX-License-Identifier: AGPL-3.0-or-later
#include "fltk_appearance.h"
#include "geometry.hpp"
#include "job.hpp"
#include "profiles.hpp"
#include "transfer.hpp"
#include "viewport.hpp"
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Float_Input.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Secret_Input.H>
#include <FL/Fl_Value_Slider.H>
#include <FL/fl_ask.H>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <locale>
#include <map>
#include <memory>
#include <pthread.h>
#include <set>
#include <spawn.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace slicer;
namespace {
std::string lower(std::string s) {
  for (auto &c : s)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
fs::path executable() { return fs::read_symlink("/proc/self/exe"); }
fs::path resources_default() {
  if (auto p = getenv("ORCA_FLTK_RESOURCES"))
    return p;
  auto installed = executable().parent_path() / "resources";
  if (fs::is_directory(installed / "profiles"))
    return installed;
  return DEFAULT_ORCA_RESOURCES;
}
fs::path worker_default() {
  if (auto p = getenv("ORCA_FLTK_WORKER"))
    return p;
  auto installed = executable().parent_path() / "orca-fltk-worker";
  if (fs::is_regular_file(installed))
    return installed;
  return DEFAULT_ORCA_WORKER;
}
std::vector<fs::path> default_roots(const fs::path &resources) {
  std::vector<fs::path> roots{resources / "profiles"};
  auto config = getenv("XDG_CONFIG_HOME");
  auto home = getenv("HOME");
  if (config)
    roots.emplace_back(fs::path(config) / "OrcaSlicer/user");
  else if (home)
    roots.emplace_back(fs::path(home) / ".config/OrcaSlicer/user");
  return roots;
}
struct Application : Fl_Double_Window {
  Profiles profiles;
  fs::path resources, worker, input, gcode, job_dir;
  std::vector<fs::path> roots;
  Job job;
  Transfer transfer;
  bool transfer_active = false;
  fs::path active_printer, compatible_printer, catalog_printer, settings_file;
  Json settings = Json::object();
  std::map<std::string, Json> compatibility_cache;
  std::map<Fl_Choice *, std::vector<std::string>> menu_labels;
  std::map<std::string, std::string> session_keys;
  Fl_Button *send = nullptr;
  Json job_result;
  std::shared_ptr<Mesh> original_mesh;
  std::array<double, 3> rotation{0, 0, 0};
  double scale = 1;
  std::ofstream job_log;
  Fl_Group *controls = nullptr;
  Fl_Input *filter = nullptr, *output = nullptr;
  Fl_Choice *machine = nullptr, *process = nullptr, *filament = nullptr;
  Fl_Check_Button *embedded = nullptr, *overrides = nullptr, *support = nullptr, *travel = nullptr;
  Fl_Float_Input *height = nullptr, *infill = nullptr;
  Fl_Button *slice = nullptr, *abort = nullptr, *xr = nullptr;
  Fl_Progress *progress = nullptr;
  Fl_Hold_Browser *logs = nullptr;
  Fl_Value_Slider *layer = nullptr;
  Viewport *viewport = nullptr;
  Fl_Box *info = nullptr;
  std::vector<const Profile *> machines, processes, filaments;
  std::vector<pid_t> previews;
  bool failed = false;
  Application(fs::path resource, fs::path engine, std::vector<fs::path> directories)
      : Fl_Double_Window(1220, 820, "Orca FLTK"), resources(std::move(resource)),
        worker(std::move(engine)), roots(std::move(directories)) {
    xclass("org.domed.OrcaSlicer");
    begin();
    controls = new Fl_Group(10, 10, 350, 630);
    controls->begin();
    auto open = new Fl_Button(15, 15, 155, 32, "Open model");
    open->callback(
        [](Fl_Widget *, void *p) {
          auto a = static_cast<Application *>(p);
          if (auto f = fl_file_chooser("Open model or sliced G-code", "*.{stl,obj,3mf,gcode,gco,g}",
                                       nullptr))
            a->guard([&] { a->open_file(f); });
        },
        this);
    auto add = new Fl_Button(180, 15, 175, 32, "Add profile folder");
    add->callback(
        [](Fl_Widget *, void *p) {
          auto a = static_cast<Application *>(p);
          if (auto f = fl_dir_chooser("Orca profile folder", nullptr))
            a->guard([&] {
              a->roots.emplace_back(f);
              a->scan();
            });
        },
        this);
    filter = new Fl_Input(15, 76, 340, 27, "Find profiles");
    filter->align(FL_ALIGN_TOP_LEFT);
    filter->when(FL_WHEN_CHANGED);
    filter->callback(
        [](Fl_Widget *, void *p) { static_cast<Application *>(p)->refresh_machines(); }, this);
    machine = new Fl_Choice(15, 130, 340, 30, "Printer profile");
    machine->align(FL_ALIGN_TOP_LEFT);
    machine->callback(
        [](Fl_Widget *, void *p) {
          auto a = static_cast<Application *>(p);
          a->guard([&] { a->select_printer(); });
        },
        this);
    process = new Fl_Choice(15, 192, 340, 30, "Process profile");
    process->align(FL_ALIGN_TOP_LEFT);
    filament = new Fl_Choice(15, 254, 340, 30, "Filament profile");
    filament->align(FL_ALIGN_TOP_LEFT);
    for (auto choice : {process, filament})
      choice->callback(
          [](Fl_Widget *, void *p) {
            auto a = static_cast<Application *>(p);
            a->guard([&] {
              a->remember_materials();
              a->save_settings();
            });
          },
          this);
    auto export_profile = new Fl_Button(15, 300, 160, 30, "Export profile");
    export_profile->callback(
        [](Fl_Widget *, void *p) {
          auto a = static_cast<Application *>(p);
          a->guard([&] { a->export_profile(); });
        },
        this);
    auto resolve = new Fl_Button(185, 300, 170, 30, "Validate profiles");
    resolve->callback(
        [](Fl_Widget *, void *p) {
          auto a = static_cast<Application *>(p);
          a->guard([&] { a->start("resolve"); });
        },
        this);
    embedded = new Fl_Check_Button(15, 344, 340, 28, "Use embedded 3MF settings");
    embedded->value(0);
    embedded->deactivate();
    overrides = new Fl_Check_Button(15, 380, 340, 28, "Override settings for this job");
    height = new Fl_Float_Input(15, 435, 155, 28, "Layer height (mm)");
    height->align(FL_ALIGN_TOP_LEFT);
    height->value("0.20");
    infill = new Fl_Float_Input(200, 435, 155, 28, "Infill (%)");
    infill->align(FL_ALIGN_TOP_LEFT);
    infill->value("15");
    support = new Fl_Check_Button(15, 490, 175, 28, "Generate supports");
    auto orient = new Fl_Button(200, 490, 155, 28, "Orient / scale");
    orient->callback(
        [](Fl_Widget *, void *p) {
          auto a = static_cast<Application *>(p);
          a->guard([&] { a->orient_model(); });
        },
        this);
    output = new Fl_Input(15, 550, 340, 28, "Output G-code (new file)");
    output->align(FL_ALIGN_TOP_LEFT);
    auto choose_output = new Fl_Button(15, 588, 340, 28, "Choose output file");
    choose_output->callback(
        [](Fl_Widget *, void *p) {
          auto a = static_cast<Application *>(p);
          if (auto f = fl_file_chooser("Save new sliced G-code", "*.gcode", a->output->value()))
            a->output->value(f);
        },
        this);
    controls->end();
    slice = new Fl_Button(15, 640, 165, 34, "Slice and preview");
    slice->callback(
        [](Fl_Widget *, void *p) {
          auto a = static_cast<Application *>(p);
          a->guard([&] { a->start("slice"); });
        },
        this);
    abort = new Fl_Button(190, 640, 165, 34, "Cancel job");
    abort->deactivate();
    abort->callback(
        [](Fl_Widget *, void *p) {
          auto a = static_cast<Application *>(p);
          a->job.cancel();
          a->transfer.cancel();
        },
        this);
    send = new Fl_Button(15, 684, 340, 30, "Send G-code to printer");
    send->deactivate();
    send->callback(
        [](Fl_Widget *, void *p) {
          auto a = static_cast<Application *>(p);
          a->guard([&] { a->upload_dialog(); });
        },
        this);
    xr = new Fl_Button(15, 720, 340, 30, "Open sliced preview in XR");
    xr->deactivate();
    xr->callback(
        [](Fl_Widget *, void *p) {
          auto a = static_cast<Application *>(p);
          a->guard([&] { a->launch_xr(); });
        },
        this);
    info = new Fl_Box(15, 758, 340, 54,
                      "Import a model to begin.\nOrca profile files remain read-only.");
    info->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    viewport = new Viewport(370, 15, 835, 535);
    layer = new Fl_Value_Slider(435, 560, 770, 27, "Layer");
    layer->align(FL_ALIGN_LEFT);
    layer->type(FL_HOR_NICE_SLIDER);
    layer->step(1);
    layer->bounds(0, 0);
    layer->value(0);
    layer->callback(
        [](Fl_Widget *, void *p) {
          auto a = static_cast<Application *>(p);
          a->viewport->layer_limit = unsigned(a->layer->value());
          a->viewport->changed();
        },
        this);
    travel = new Fl_Check_Button(370, 598, 170, 28, "Travel / custom");
    travel->callback(
        [](Fl_Widget *, void *p) {
          auto a = static_cast<Application *>(p);
          a->viewport->travels = a->travel->value();
          a->viewport->changed();
        },
        this);
    auto fit = new Fl_Button(550, 598, 130, 28, "Fit view");
    fit->callback([](Fl_Widget *, void *p) { static_cast<Application *>(p)->viewport->fit(); },
                  this);
    auto model = new Fl_Button(695, 598, 140, 28, "Model view");
    model->callback(
        [](Fl_Widget *, void *p) {
          auto a = static_cast<Application *>(p);
          if (a->viewport->mesh) {
            a->viewport->paths.reset();
            a->viewport->changed();
          }
        },
        this);
    auto preview = new Fl_Button(850, 598, 160, 28, "Sliced view");
    preview->callback(
        [](Fl_Widget *, void *p) {
          auto a = static_cast<Application *>(p);
          if (!a->gcode.empty())
            a->guard([&] { a->load_gcode(a->gcode); });
        },
        this);
    progress = new Fl_Progress(370, 640, 835, 25);
    progress->minimum(0);
    progress->maximum(100);
    progress->value(0);
    logs = new Fl_Hold_Browser(370, 680, 835, 125);
    logs->textsize(12);
    end();
    resizable(viewport);
    size_range(1050, 760);
    callback(
        [](Fl_Widget *w, void *p) {
          auto a = static_cast<Application *>(p);
          if ((a->job.busy() || a->transfer.status().running) &&
              fl_choice("Cancel the active job and close?", "Keep working", "Close", nullptr) != 1)
            return;
          a->job.cancel();
          a->transfer.cancel();
          w->hide();
        },
        this);
    load_settings();
    scan();
    dx::appearance();
    Fl::add_timeout(.05, tick, this);
  }
  ~Application() { Fl::remove_timeout(tick, this); }
  template <class F> void guard(F &&f) {
    try {
      f();
    } catch (const std::exception &ex) {
      log(ex.what());
      fl_alert("%s", ex.what());
    }
  }
  void log(const std::string &message) {
    logs->add(message.c_str());
    while (logs->size() > 500)
      logs->remove(1);
    logs->bottomline(logs->size());
    if (job_log)
      job_log << message << '\n';
  }
  const Profile *selected(Fl_Choice *control, const std::vector<const Profile *> &entries) {
    int index = control->value();
    return index >= 0 && size_t(index) < entries.size() ? entries[size_t(index)] : nullptr;
  }
  void fill(Fl_Choice *control, const std::vector<const Profile *> &entries,
            const fs::path &preserve = {}) {
    // Copy a flat menu: add() treats names as paths and merges duplicate labels.
    control->clear();
    auto &labels = menu_labels[control];
    labels.clear();
    labels.reserve(entries.size());
    std::map<std::string, size_t> names;
    for (const auto *entry : entries)
      ++names[entry->name];
    for (const auto *entry : entries) {
      auto label = entry->name + " [" + (entry->vendor.empty() ? "user" : entry->vendor) + "]";
      if (names[entry->name] > 1)
        label += " (" + entry->file.parent_path().string() + ")";
      // FLTK label drawing interprets ampersands as mnemonic markers.
      std::string display;
      for (char c : label) {
        display += c;
        if (c == '&')
          display += '&';
      }
      labels.push_back(std::move(display));
    }
    std::vector<Fl_Menu_Item> items(entries.size() + 1);
    int choice = -1;
    for (size_t i = 0; i < entries.size(); ++i) {
      items[i].label(labels[i].c_str());
      items[i].labelsize(control->textsize());
      items[i].labelfont(control->textfont());
      items[i].labelcolor(control->textcolor());
      if (entries[i]->file == preserve)
        choice = int(i);
    }
    control->copy(items.data());
    control->value(choice);
    if (entries.empty())
      control->deactivate();
    else
      control->activate();
  }
  void load_settings() {
    auto config = getenv("XDG_CONFIG_HOME");
    auto home = getenv("HOME");
    settings_file = (config ? fs::path(config)
                     : home ? fs::path(home) / ".config"
                            : fs::temp_directory_path()) /
                    "orca-fltk/settings.json";
    try {
      if (fs::is_regular_file(settings_file))
        settings = read_json(settings_file);
      for (const char *key : {"selections", "servers"}) {
        if (settings.contains(key) && !settings[key].is_object())
          throw std::runtime_error("Invalid saved preferences");
        for (const auto &entry : settings.value(key, Json::object())) {
          if (!entry.is_object())
            throw std::runtime_error("Invalid saved preference entry");
          for (const auto &value : entry)
            if (!value.is_string())
              throw std::runtime_error("Invalid saved preference value");
        }
      }
      if (!settings.is_object())
        throw std::runtime_error("Settings must be a JSON object");
      active_printer = settings.value("printer", "");
      for (const auto &root : settings.value("profile_roots", Json::array()))
        if (root.is_string() && fs::is_directory(root.get<std::string>()))
          roots.emplace_back(root.get<std::string>());
    } catch (const std::exception &) {
      settings = Json::object();
      active_printer.clear();
      log("Saved selection could not be read; select profiles again");
    }
  }
  void save_settings() {
    settings["printer"] = active_printer.string();
    settings["profile_roots"] = Json::array();
    std::set<fs::path> unique;
    for (const auto &root : roots)
      if (unique.insert(fs::weakly_canonical(root)).second)
        settings["profile_roots"].push_back(fs::absolute(root).string());
    fs::create_directories(settings_file.parent_path());
    auto pattern = settings_file.string() + ".XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back(0);
    int fd = mkstemp(name.data());
    if (fd < 0)
      throw std::runtime_error("Cannot save slicer settings");
    FILE *out = fdopen(fd, "w");
    if (!out) {
      close(fd);
      unlink(name.data());
      throw std::runtime_error("Cannot save slicer settings");
    }
    auto text = settings.dump(2) + "\n";
    bool good = fwrite(text.data(), 1, text.size(), out) == text.size();
    good = fflush(out) == 0 && good;
    good = fsync(fd) == 0 && good;
    good = fclose(out) == 0 && good;
    if (!good || rename(name.data(), settings_file.c_str())) {
      unlink(name.data());
      throw std::runtime_error("Cannot save slicer settings");
    }
  }
  void remember_materials() {
    if (active_printer.empty() || compatible_printer != active_printer)
      return;
    auto proc = selected(process, processes), material = selected(filament, filaments);
    settings["selections"][active_printer.string()] = {
        {"process", proc ? proc->file.string() : ""},
        {"filament", material ? material->file.string() : ""}};
  }
  void select_printer() {
    if (job.busy() || transfer.status().running)
      throw std::runtime_error("Wait for the current job or transfer");
    auto chosen = selected(machine, machines);
    if (!chosen)
      return;
    if (chosen->file == active_printer && compatible_printer == active_printer)
      return;
    remember_materials();
    active_printer = chosen->file;
    clear_materials();
    save_settings();
    auto cached = compatibility_cache.find(active_printer.string());
    if (cached != compatibility_cache.end())
      compatible_profiles(cached->second);
    else
      start("catalog");
  }
  void scan() {
    remember_materials();
    machine->clear();
    process->clear();
    filament->clear();
    machines.clear();
    processes.clear();
    filaments.clear();
    compatible_printer.clear();
    compatibility_cache.clear();
    profiles.scan(roots);
    auto active = profiles.find(active_printer);
    if (!active || active->type != "machine" || !active->instantiated())
      active_printer.clear();
    refresh_machines();
    log("Loaded " + std::to_string(profiles.entries().size()) + " Orca profile definitions");
    if (!profiles.warnings().empty())
      log(std::to_string(profiles.warnings().size()) + " profile files could not be read");
    if (!active_printer.empty())
      start("catalog");
  }
  void refresh_machines() {
    machines = profiles.choices("machine");
    auto query = lower(filter->value());
    if (!query.empty())
      machines.erase(std::remove_if(machines.begin(), machines.end(),
                                    [&](const Profile *p) {
                                      return p->file != active_printer &&
                                             lower(p->name).find(query) == std::string::npos;
                                    }),
                     machines.end());
    fill(machine, machines, active_printer);
    if (active_printer.empty())
      clear_materials();
  }
  void clear_materials() {
    processes.clear();
    filaments.clear();
    compatible_printer.clear();
    process->clear();
    filament->clear();
    process->deactivate();
    filament->deactivate();
    slice->deactivate();
  }
  Json request(const std::string &action) {
    Json data = {{"action", action},
                 {"resources", fs::absolute(resources).string()},
                 {"work_dir", job_dir.string()},
                 {"profile_roots", Json::array()},
                 {"center_on_bed", lower(input.extension().string()) != ".3mf"}};
    for (const auto &root : roots)
      data["profile_roots"].push_back(fs::absolute(root).string());
    if (action == "catalog") {
      auto chosen = selected(machine, machines);
      if (!chosen)
        throw std::runtime_error("Select a printer profile");
      data["machine"] = chosen->file.string();
      return data;
    }
    if (action == "inspect") {
      data["input"] = input.string();
      return data;
    }
    if (compatible_printer != active_printer || active_printer.empty())
      throw std::runtime_error("Load compatible profiles for the selected printer first");
    for (auto pair : {std::make_pair("machine", selected(machine, machines)),
                      std::make_pair("process", selected(process, processes)),
                      std::make_pair("filament", selected(filament, filaments))}) {
      if (!pair.second)
        throw std::runtime_error("Select a printer, process and filament profile");
      data["profiles"][pair.first] = pair.second->file.string();
    }
    if (action == "slice") {
      if (input.empty())
        throw std::runtime_error("Open a model before slicing");
      if (std::string(output->value()).empty())
        throw std::runtime_error("Choose a new output G-code filename");
      data["input"] = input.string();
      data["output"] = fs::absolute(output->value()).string();
      data["use_project_settings"] = bool(embedded->value());
      data["transform"] = {{"rotation_deg", rotation}, {"scale", scale}};
    }
    if (overrides->value())
      data["overrides"] = {{"layer_height", height->value()},
                           {"sparse_infill_density", std::string(infill->value()) + "%"},
                           {"enable_support", support->value() ? "1" : "0"}};
    return data;
  }
  void start(const std::string &action) {
    if (job.busy() || transfer.status().running)
      throw std::runtime_error("Wait for the current job or transfer");
    if (action == "slice" || action == "resolve") {
      remember_materials();
      save_settings();
    }
    job_dir = new_job_directory();
    auto data = request(action);
    if (action == "catalog")
      catalog_printer = data.at("machine").get<std::string>();
    write_json_new(job_dir / "request.json", data);
    job_result = Json();
    failed = false;
    job_log.close();
    job_log.open(job_dir / "worker.log");
    job.start(worker, job_dir / "request.json");
    controls->deactivate();
    send->deactivate();
    slice->deactivate();
    abort->activate();
    progress->value(0);
    log(action == "slice" ? "Slicing with the selected Orca profiles"
                          : "Loading with Orca's native configuration and model readers");
  }
  void open_file(const fs::path &path) {
    if (job.busy() || transfer.status().running)
      throw std::runtime_error("Wait for the current job or transfer");
    auto file = fs::absolute(path);
    auto extension = lower(file.extension().string());
    if (extension == ".gcode" || extension == ".gco" || extension == ".g") {
      input.clear();
      original_mesh.reset();
      viewport->mesh.reset();
      rotation = {0, 0, 0};
      scale = 1;
      embedded->value(0);
      embedded->deactivate();
      output->value("");
      load_gcode(file);
      return;
    }
    input = file;
    original_mesh.reset();
    rotation = {0, 0, 0};
    scale = 1;
    gcode.clear();
    xr->deactivate();
    send->deactivate();
    viewport->paths.reset();
    viewport->mesh.reset();
    viewport->changed();
    output->value((file.parent_path() / (file.stem().string() + "-sliced.gcode")).c_str());
    bool project = extension == ".3mf";
    embedded->value(project);
    if (project)
      embedded->activate();
    else
      embedded->deactivate();
    if (extension == ".stl") {
      original_mesh = std::make_shared<Mesh>(read_stl(file));
      viewport->mesh = original_mesh;
      viewport->fit();
      info->copy_label((file.filename().string() + "\n" +
                        std::to_string(viewport->mesh->triangles.size()) + " triangles")
                           .c_str());
    } else
      start("inspect");
  }
  void load_gcode(const fs::path &path) {
    auto parsed = std::make_shared<Toolpath>(read_gcode(path));
    gcode = fs::absolute(path);
    viewport->paths = std::move(parsed);
    layer->bounds(0, viewport->paths->heights.size() - 1);
    layer->value(viewport->paths->heights.size() - 1);
    viewport->layer_limit = unsigned(layer->value());
    viewport->changed();
    viewport->fit();
    xr->activate();
    if (!job.busy() && !transfer.status().running)
      send->activate();
    info->copy_label((gcode.filename().string() + "\n" +
                      std::to_string(viewport->paths->heights.size()) + " layers, " +
                      std::to_string(viewport->paths->segments.size()) + " moves")
                         .c_str());
  }
  void orient_model() {
    if (!original_mesh || lower(input.extension().string()) != ".stl")
      throw std::runtime_error("Open an STL model to edit orientation and scale");
    Fl_Double_Window dialog(400, 240, "Model orientation and scale");
    dialog.begin();
    Fl_Float_Input x(20, 50, 105, 28, "X rotation");
    x.align(FL_ALIGN_TOP_LEFT);
    Fl_Float_Input y(145, 50, 105, 28, "Y rotation");
    y.align(FL_ALIGN_TOP_LEFT);
    Fl_Float_Input z(270, 50, 105, 28, "Z rotation");
    z.align(FL_ALIGN_TOP_LEFT);
    Fl_Float_Input percent(20, 115, 150, 28, "Scale (%)");
    percent.align(FL_ALIGN_TOP_LEFT);
    auto xs = std::to_string(rotation[0]), ys = std::to_string(rotation[1]),
         zs = std::to_string(rotation[2]), ss = std::to_string(scale * 100);
    x.value(xs.c_str());
    y.value(ys.c_str());
    z.value(zs.c_str());
    percent.value(ss.c_str());
    Fl_Button cancel(165, 185, 100, 30, "Cancel"), apply(280, 185, 100, 30, "Apply");
    bool accepted = false;
    cancel.callback([](Fl_Widget *w, void *) { w->window()->hide(); });
    apply.callback(
        [](Fl_Widget *w, void *p) {
          *static_cast<bool *>(p) = true;
          w->window()->hide();
        },
        &accepted);
    dialog.end();
    dialog.set_modal();
    dialog.show();
    while (dialog.shown())
      Fl::wait();
    if (!accepted)
      return;
    auto number = [](const char *text) {
      std::istringstream in(text);
      in.imbue(std::locale::classic());
      double value;
      char extra;
      if (!(in >> value) || (in >> extra) || !std::isfinite(value))
        throw std::runtime_error("Enter finite numeric rotation and scale values");
      return value;
    };
    std::array<double, 3> next = {number(x.value()), number(y.value()), number(z.value())};
    double factor = number(percent.value()) / 100;
    auto transformed = std::make_shared<Mesh>(transform_mesh(*original_mesh, next, factor));
    rotation = next;
    scale = factor;
    viewport->mesh = std::move(transformed);
    viewport->paths.reset();
    viewport->fit();
    viewport->changed();
    log("Updated model orientation and scale; slicing will centre the STL on the selected bed");
  }
  void compatible_profiles(const Json &result) {
    if (result.value("machine", catalog_printer.string()) != active_printer.string()) {
      log("Discarded stale profile compatibility result");
      return;
    }
    auto saved =
        settings.value("selections", Json::object()).value(active_printer.string(), Json::object());
    fs::path process_path = saved.value("process", ""), filament_path = saved.value("filament", "");
    processes.clear();
    filaments.clear();
    for (const auto &path : result.at("compatible").at("process"))
      if (auto profile = profiles.find(path.get<std::string>()))
        processes.push_back(profile);
    for (const auto &path : result.at("compatible").at("filament"))
      if (auto profile = profiles.find(path.get<std::string>()))
        filaments.push_back(profile);
    fill(process, processes, process_path);
    fill(filament, filaments, filament_path);
    auto defaults = result.value("defaults", Json::object());
    if (process_path.empty() || !selected(process, processes) ||
        selected(process, processes)->file != process_path)
      for (size_t i = 0; i < processes.size(); ++i)
        if (processes[i]->name == defaults.value("process", "")) {
          process->value(int(i));
          break;
        }
    if (filament_path.empty() || !selected(filament, filaments) ||
        selected(filament, filaments)->file != filament_path)
      for (const auto &name : defaults.value("filament", Json::array())) {
        auto match = std::find_if(filaments.begin(), filaments.end(),
                                  [&](const Profile *p) { return p->name == name; });
        if (match != filaments.end()) {
          filament->value(int(match - filaments.begin()));
          break;
        }
      }
    if (!selected(process, processes) && !processes.empty())
      process->value(0);
    if (!selected(filament, filaments) && !filaments.empty())
      filament->value(0);
    compatible_printer = active_printer;
    compatibility_cache[active_printer.string()] = result;
    remember_materials();
    save_settings();
    if (selected(process, processes) && selected(filament, filaments))
      slice->activate();
    log("Orca compatibility: " + std::to_string(processes.size()) + " process and " +
        std::to_string(filaments.size()) + " filament profiles");
  }
  void export_profile() {
    int kind = fl_choice("Which original Orca profile should be exported?", "Printer", "Process",
                         "Filament");
    if (kind < 0)
      return;
    auto p = kind == 0 ? selected(machine, machines)
                       : (kind == 1 ? selected(process, processes) : selected(filament, filaments));
    if (!p)
      throw std::runtime_error("No profile is selected");
    if (auto file = fl_file_chooser("Export original Orca profile JSON", "*.json",
                                    p->file.filename().c_str())) {
      Profiles::export_original(*p, file);
      log("Exported original profile without changing its values or inheritance");
    }
  }
  void start_transfer(UploadRequest request) {
    if (job.busy() || transfer.status().running)
      throw std::runtime_error("Wait for the current job or transfer");
    Transfer::validate(request);
    const auto target = active_printer.empty() ? "default" : active_printer.string();
    settings["servers"][target] = {{"protocol", request.protocol},
                                   {"url", request.url},
                                   {"folder", request.folder},
                                   {"ca_file", request.ca_file}};
    save_settings();
    session_keys[request.protocol + "\n" + request.url] = request.api_key;
    transfer.start(std::move(request));
    transfer_active = true;
    controls->deactivate();
    slice->deactivate();
    send->deactivate();
    abort->activate();
    progress->value(0);
    log("Uploading G-code to print server");
  }
  void upload_dialog() {
    if (gcode.empty())
      throw std::runtime_error("Slice a model or open G-code first");
    if (job.busy() || transfer.status().running)
      throw std::runtime_error("Wait for the current job or transfer");
    const auto target = active_printer.empty() ? "default" : active_printer.string();
    auto saved = settings.value("servers", Json::object()).value(target, Json::object());
    Fl_Double_Window dialog(600, 510, "Send sliced G-code");
    dialog.begin();
    Fl_Choice protocol(20, 45, 560, 28, "Print server");
    protocol.align(FL_ALIGN_TOP_LEFT);
    protocol.add("OctoPrint");
    protocol.add("Moonraker / Klipper");
    protocol.value(saved.value("protocol", "octoprint") == "moonraker" ? 1 : 0);
    Fl_Input url(20, 105, 560, 28, "Server base URL (http://printer or https://printer)");
    url.align(FL_ALIGN_TOP_LEFT);
    auto address = saved.value("url", "");
    url.value(address.c_str());
    Fl_Secret_Input key(20, 165, 560, 28, "API key (kept in memory for this session)");
    key.align(FL_ALIGN_TOP_LEFT);
    auto secret = session_keys[(protocol.value() == 1 ? "moonraker" : "octoprint") +
                               std::string("\n") + address];
    key.value(secret.c_str());
    Fl_Input remote(20, 225, 560, 28, "Remote G-code filename");
    remote.align(FL_ALIGN_TOP_LEFT);
    remote.value(gcode.filename().c_str());
    Fl_Input folder(20, 285, 560, 28, "Remote folder (optional, relative to G-code storage)");
    folder.align(FL_ALIGN_TOP_LEFT);
    auto path = saved.value("folder", "");
    folder.value(path.c_str());
    Fl_Input ca(20, 345, 560, 28, "Custom CA certificate file (optional)");
    ca.align(FL_ALIGN_TOP_LEFT);
    auto certificate = saved.value("ca_file", "");
    ca.value(certificate.c_str());
    Fl_Box note(20, 385, 560, 48,
                "Uploads only; printing is not started.\nA matching remote filename may be "
                "replaced by the server.");
    note.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    Fl_Button cancel(320, 455, 120, 32, "Cancel"), upload(455, 455, 125, 32, "Upload");
    cancel.callback([](Fl_Widget *w, void *) { w->window()->hide(); });
    struct Context {
      Application *app;
      Fl_Choice *protocol;
      Fl_Input *url, *key, *remote, *folder, *ca;
    } context{this, &protocol, &url, &key, &remote, &folder, &ca};
    upload.callback(
        [](Fl_Widget *w, void *p) {
          auto &c = *static_cast<Context *>(p);
          c.app->guard([&] {
            c.app->start_transfer(
                {c.app->gcode, c.protocol->value() == 1 ? "moonraker" : "octoprint", c.url->value(),
                 c.key->value(), c.remote->value(), c.folder->value(), c.ca->value()});
            w->window()->hide();
          });
        },
        &context);
    dialog.end();
    dialog.set_modal();
    dx::appearance();
    dialog.show();
    while (dialog.shown())
      Fl::wait();
  }
  void launch_xr() {
    if (gcode.empty())
      throw std::runtime_error("Slice a model or open G-code first");
    if (access("/usr/local/bin/dome-link", X_OK))
      throw std::runtime_error("DomeD's application launcher is required for XR preview");
    std::string app = executable().string(), file = gcode.string();
    char *args[] = {const_cast<char *>("/usr/local/bin/dome-link"),
                    const_cast<char *>("--realm"),
                    const_cast<char *>("xr"),
                    const_cast<char *>("--app-id"),
                    const_cast<char *>("org.domed.OrcaPreview.Sbs"),
                    const_cast<char *>("--"),
                    app.data(),
                    const_cast<char *>("--xr-preview"),
                    file.data(),
                    nullptr};
    pid_t pid;
    auto status = posix_spawn(&pid, args[0], nullptr, nullptr, args, environ);
    if (status)
      throw std::runtime_error("Cannot launch XR preview");
    previews.push_back(pid);
    log("Opened a stereoscopic sliced-object preview in DomeD's XR realm");
  }
  static void tick(void *p) {
    auto a = static_cast<Application *>(p);
    a->previews.erase(
        std::remove_if(a->previews.begin(), a->previews.end(),
                       [](pid_t pid) { return waitpid(pid, nullptr, WNOHANG) == pid; }),
        a->previews.end());
    a->job.poll(
        [&](const std::string &line) {
          auto value = Json::parse(line, nullptr, false);
          if (value.is_discarded() || !value.is_object() || !value.contains("event")) {
            if (!line.empty())
              a->log(line);
            return;
          }
          auto event = value.value("event", "");
          if (event == "progress") {
            auto percent = value.value("percent", -1);
            if (percent >= 0)
              a->progress->value(std::clamp(percent, 0, 100));
            auto text = value.value("message", "");
            if (!text.empty())
              a->log(text);
          } else if (event == "result")
            a->job_result = value;
          else {
            auto text = value.value("message", line);
            a->log(text);
            if (event == "error")
              a->failed = true;
          }
        },
        [&](int status) {
          a->controls->activate();
          if (a->compatible_printer == a->active_printer && a->selected(a->process, a->processes) &&
              a->selected(a->filament, a->filaments))
            a->slice->activate();
          if (!a->gcode.empty())
            a->send->activate();
          a->abort->deactivate();
          if (lower(a->input.extension().string()) != ".3mf")
            a->embedded->deactivate();
          if (status || a->failed || a->job_result.is_null()) {
            a->log(status == 130 ? "Job cancelled" : "Job failed; the worker log contains details");
            a->job_log.close();
            return;
          }
          a->progress->value(100);
          a->guard([&] {
            if (a->job_result.contains("compatible"))
              a->compatible_profiles(a->job_result);
            if (a->job_result.contains("mesh")) {
              a->viewport->mesh =
                  std::make_shared<Mesh>(read_stl(a->job_result["mesh"].get<std::string>()));
              a->viewport->fit();
              a->info->copy_label((a->input.filename().string() + "\n" +
                                   std::to_string(a->job_result.value("objects", 0)) +
                                   " objects imported")
                                      .c_str());
            }
            if (a->job_result.contains("output"))
              a->load_gcode(a->job_result["output"].get<std::string>());
            if (a->job_result.contains("config"))
              a->log("Resolved configuration: " + a->job_result["config"].get<std::string>());
          });
          a->log("Job completed");
          a->job_log.close();
        });
    if (a->transfer_active) {
      auto state = a->transfer.status();
      a->progress->value(state.percent);
      if (!state.running) {
        a->transfer_active = false;
        a->controls->activate();
        a->send->activate();
        a->abort->deactivate();
        if (a->compatible_printer == a->active_printer && a->selected(a->process, a->processes) &&
            a->selected(a->filament, a->filaments))
          a->slice->activate();
        if (lower(a->input.extension().string()) != ".3mf")
          a->embedded->deactivate();
        a->log(state.message);
      }
    }
    Fl::repeat_timeout(.05, tick, p);
  }
};
} // namespace
#ifndef ORCA_FLTK_NO_MAIN
int main(int argc, char **argv) {
  sigset_t empty;
  sigemptyset(&empty);
  pthread_sigmask(SIG_SETMASK, &empty, nullptr);
  try {
    if (argc == 2 && std::string(argv[1]) == "--version") {
      std::cout << "Orca FLTK 0.2.0 / Orca engine fa5f7e3\n";
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--help") {
      std::cout
          << "orca-fltk [MODEL.stl|MODEL.obj|PROJECT.3mf|FILE.gcode] [--resources DIR] [--profiles "
             "DIR] [--worker FILE]\n  --xr-preview FILE.gcode    Stereoscopic preview for DomeD's "
             "XR realm\n  --list-profiles            List original Orca profile identities as "
             "JSON\n  --inspect-gcode FILE       Print preview statistics as JSON\n  --render-test "
             "FILE IMAGE [--stereo]   Render a preview capture\n";
      return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--inspect-gcode") {
      auto p = read_gcode(argv[2]);
      std::cout << Json({{"layers", p.heights.size()}, {"segments", p.segments.size()}}).dump()
                << '\n';
      return 0;
    }
    bool xr = argc == 3 && std::string(argv[1]) == "--xr-preview",
         render = argc >= 4 && std::string(argv[1]) == "--render-test";
    if (xr || render) {
      dx::init();
      Viewport preview(0, 0, 1000, 700, "Orca sliced-object preview");
      preview.paths = std::make_shared<Toolpath>(read_gcode(argv[2]));
      preview.stereo = xr || (argc == 5 && std::string(argv[4]) == "--stereo");
      preview.xclass(preview.stereo ? "org.domed.OrcaPreview.Sbs" : "org.domed.OrcaPreview");
      preview.show();
      if (render) {
        for (int i = 0; i < 5; ++i)
          Fl::wait(.04);
        preview.capture(argv[3]);
        return 0;
      }
      return Fl::run();
    }
    auto resource = resources_default(), worker = worker_default();
    std::vector<fs::path> extra;
    fs::path input;
    bool listing = false;
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg == "--list-profiles")
        listing = true;
      else if ((arg == "--resources" || arg == "--profiles" || arg == "--worker") && i + 1 < argc) {
        fs::path value = argv[++i];
        if (arg == "--resources")
          resource = value;
        else if (arg == "--worker")
          worker = value;
        else
          extra.push_back(value);
      } else if (arg.empty() || arg[0] == '-')
        throw std::runtime_error("Unknown or incomplete option: " + arg);
      else if (input.empty())
        input = arg;
      else
        throw std::runtime_error("Open one model or project at a time");
    }
    auto roots = default_roots(resource);
    roots.insert(roots.end(), extra.begin(), extra.end());
    if (listing) {
      Profiles catalog;
      catalog.scan(roots);
      Json data = Json::array();
      for (const auto &p : catalog.entries())
        if (p.instantiated())
          data.push_back({{"name", p.name},
                          {"type", p.type},
                          {"vendor", p.vendor},
                          {"path", p.file.string()}});
      std::cout << data.dump(2) << '\n';
      return 0;
    }
    dx::init();
    Application app(resource, worker, roots);
    app.show();
    if (!input.empty())
      app.guard([&] { app.open_file(input); });
    return Fl::run();
  } catch (const std::exception &ex) {
    std::cerr << "Orca FLTK: " << ex.what() << '\n';
    return 1;
  }
}

#endif
