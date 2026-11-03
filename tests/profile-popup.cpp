// SPDX-License-Identifier: AGPL-3.0-or-later
#define ORCA_FLTK_NO_MAIN
#include "../src/main.cpp"
#include <cassert>
struct PopupCheck {
  int count = 0;
  bool bounded = true, accept = false;
};
static void close_popup(void *p) {
  auto &check = *static_cast<PopupCheck *>(p);
  auto window = Fl::grab() ? Fl::grab() : Fl::first_window();
  assert(window);
  std::cerr << "selector geometry " << window->w() << 'x' << window->h() << '\n';
  check.bounded &= window->w() <= 1000 && window->h() <= 800;
  ++check.count;
  if (check.accept) {
    auto list = dynamic_cast<Fl_Hold_Browser *>(window->child(1));
    assert(list && list->size() > 100);
    list->value(list->size());
    list->bottomline(list->size());
    window->child(3)->do_callback();
  } else if (Fl::grab()) {
    Fl::e_keysym = FL_Escape;
    Fl::grab()->handle(FL_KEYBOARD);
  } else
    window->hide();
}
int main(int argc, char **argv) {
  assert(argc == 4);
  fs::path out = fs::absolute(argv[3]);
  fs::create_directories(out / "config");
  setenv("XDG_CONFIG_HOME", (out / "config").c_str(), 1);
  dx::init();
  Application app(argv[1], argv[2], {fs::path(argv[1]) / "profiles"});
  app.show();
  Fl::wait(.3);
  app.processes = app.profiles.choices("process");
  app.filaments = app.profiles.choices("filament");
  app.fill(app.process, app.processes);
  app.fill(app.filament, app.filaments);
  PopupCheck check;
  for (auto choice : {app.machine, app.process, app.filament, app.machine}) {
    assert(choice->size() > 100);
    Fl::add_timeout(.5, close_popup, &check);
    Fl::e_keysym = ' ';
    Fl::e_state = 0;
    choice->handle(FL_KEYBOARD);
    assert(app.shown() && choice->value() == -1);
  }
  int callbacks = 0;
  app.machine->callback([](Fl_Widget *, void *p) { ++*static_cast<int *>(p); }, &callbacks);
  check.accept = true;
  Fl::add_timeout(.5, close_popup, &check);
  Fl::e_keysym = ' ';
  app.machine->handle(FL_KEYBOARD);
  assert(app.machine->value() == int(app.machines.size() - 1) && callbacks == 1);
  assert(check.count == 5 && check.bounded);
  fs::remove(out / "report.json");
  write_json_new(out / "report.json", {{"passed", true},
                                       {"bounded_selectors", check.count},
                                       {"printers", app.machines.size()},
                                       {"processes", app.processes.size()},
                                       {"filaments", app.filaments.size()}});
  app.hide();
  std::cout << "PASS unfiltered printer, process and filament selectors open, scroll and cancel "
               "without exiting\n";
}
