// SPDX-License-Identifier: AGPL-3.0-or-later
#include "fltk_appearance.h"
#include "geometry.hpp"
#include "job.hpp"
#include "profiles.hpp"
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
#include <FL/Fl_Value_Slider.H>
#include <FL/fl_ask.H>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <csignal>
#include <fstream>
#include <iostream>
#include <locale>
#include <memory>
#include <pthread.h>
#include <spawn.h>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>
using namespace slicer;
namespace {
std::string lower(std::string s) {
  for (auto &c : s)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
std::string menu_label(const std::string &s) {
  std::string out;
  for (char c : s) {
    if (c == '/' || c == '\\' || c == '&')
      out += '\\';
    out += c;
  }
  return out;
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
          a->guard([&] {
            a->clear_materials();
            a->start("catalog");
          });
        },
        this);
    process = new Fl_Choice(15, 192, 340, 30, "Process profile");
    process->align(FL_ALIGN_TOP_LEFT);
    filament = new Fl_Choice(15, 254, 340, 30, "Filament profile");
    filament->align(FL_ALIGN_TOP_LEFT);
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
    abort->callback([](Fl_Widget *, void *p) { static_cast<Application *>(p)->job.cancel(); },
                    this);
    xr = new Fl_Button(15, 690, 340, 32, "Open sliced preview in XR");
    xr->deactivate();
    xr->callback(
        [](Fl_Widget *, void *p) {
          auto a = static_cast<Application *>(p);
          a->guard([&] { a->launch_xr(); });
        },
        this);
    info = new Fl_Box(15, 735, 340, 67,
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
          if (a->job.busy() &&
              fl_choice("Cancel the active job and close?", "Keep working", "Close", nullptr) != 1)
            return;
          a->job.cancel();
          w->hide();
        },
        this);
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
    control->clear();
    int choice = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
      auto label = entries[i]->name + (entries[i]->vendor.empty() ? " [user]" : "");
      control->add(menu_label(label).c_str());
      if (entries[i]->file == preserve)
        choice = int(i);
    }
    if (!entries.empty())
      control->value(choice);
  }
  void scan() {
    machines.clear();
    processes.clear();
    filaments.clear();
    profiles.scan(roots);
    refresh_machines();
    log("Loaded " + std::to_string(profiles.entries().size()) + " Orca profile definitions");
    if (!profiles.warnings().empty())
      log(std::to_string(profiles.warnings().size()) + " profile files could not be read");
  }
  void refresh_machines() {
    auto old = selected(machine, machines);
    auto preserve = old ? old->file : fs::path();
    machines = profiles.choices("machine");
    auto query = lower(filter->value());
    if (!query.empty())
      machines.erase(std::remove_if(machines.begin(), machines.end(),
                                    [&](const Profile *p) {
                                      return lower(p->name).find(query) == std::string::npos;
                                    }),
                     machines.end());
    fill(machine, machines, preserve);
    if (preserve.empty() || !selected(machine, machines) ||
        selected(machine, machines)->file != preserve)
      machine->value(-1);
    if (!selected(machine, machines))
      clear_materials();
  }
  void clear_materials() {
    processes.clear();
    filaments.clear();
    process->clear();
    filament->clear();
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
    if (job.busy())
      throw std::runtime_error("Wait for the current job");
    job_dir = new_job_directory();
    auto data = request(action);
    write_json_new(job_dir / "request.json", data);
    job_result = Json();
    failed = false;
    job_log.close();
    job_log.open(job_dir / "worker.log");
    job.start(worker, job_dir / "request.json");
    controls->deactivate();
    slice->deactivate();
    abort->activate();
    progress->value(0);
    log(action == "slice" ? "Slicing with the selected Orca profiles"
                          : "Loading with Orca's native configuration and model readers");
  }
  void open_file(const fs::path &path) {
    if (job.busy())
      throw std::runtime_error("Wait for the current job");
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
    auto old_process = selected(process, processes), old_filament = selected(filament, filaments);
    fs::path process_path = old_process ? old_process->file : fs::path();
    fs::path filament_path = old_filament ? old_filament->file : fs::path();
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
      for (const auto &name : defaults.value("filament", Json::array()))
        for (size_t i = 0; i < filaments.size(); ++i)
          if (filaments[i]->name == name) {
            filament->value(int(i));
            break;
          }
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
          a->slice->activate();
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
      std::cout << "Orca FLTK 0.1.0 / Orca engine fa5f7e3\n";
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
