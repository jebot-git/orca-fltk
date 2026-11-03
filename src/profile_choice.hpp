// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Return_Button.H>
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>
namespace slicer {
// Fl_Choice allocates a popup as tall as every item combined. Native Wayland
// renderers cannot accommodate a full Orca catalog in one such surface.
class ProfileChoice : public Fl_Choice {
public:
  using Fl_Choice::Fl_Choice;
  int handle(int event) override {
    const bool open =
        event == FL_PUSH ||
        (event == FL_KEYBOARD &&
         (Fl::event_key() == ' ' || Fl::event_key() == FL_Down || Fl::event_key() == FL_Enter)) ||
        (event == FL_SHORTCUT && Fl_Widget::test_shortcut());
    if (!open)
      return Fl_Choice::handle(event);
    if (!active_r() || size() <= 1)
      return 1;
    take_focus();
    choose_profile();
    return 1;
  }

private:
  void choose_profile() {
    std::string title = std::string("Select ") + (label() ? label() : "profile");
    Fl_Double_Window dialog(760, 560, title.c_str());
    dialog.begin();
    Fl_Input query(15, 35, 730, 28, "Search this list");
    query.align(FL_ALIGN_TOP_LEFT);
    Fl_Hold_Browser list(15, 75, 730, 420);
    list.textfont(textfont());
    list.textsize(textsize());
    list.format_char(0);
    list.column_char(0);
    Fl_Button cancel(490, 515, 120, 30, "Cancel");
    Fl_Return_Button select(625, 515, 120, 30, "Select");
    struct Context {
      ProfileChoice *choice;
      Fl_Input *query;
      Fl_Hold_Browser *list;
      Fl_Button *select;
      std::vector<int> indices;
      int accepted = -1;
      static std::string folded(std::string s) {
        for (char &c : s)
          c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
      }
      void refresh() {
        list->clear();
        indices.clear();
        auto needle = folded(query->value());
        int current = 0;
        for (int i = 0; i < choice->size() - 1; ++i) {
          std::string text = choice->menu()[i].label();
          // Menu labels encode mnemonic ampersands; browser labels are literal.
          for (size_t pos = 0; (pos = text.find("&&", pos)) != std::string::npos; ++pos)
            text.erase(pos, 1);
          if (folded(text).find(needle) == std::string::npos)
            continue;
          list->add(text.c_str());
          indices.push_back(i);
          if (i == choice->value())
            current = list->size();
        }
        if (!current && list->size())
          current = 1;
        list->value(current);
        if (current)
          list->middleline(current);
        if (current)
          select->activate();
        else
          select->deactivate();
      }
      void accept() {
        int row = list->value();
        if (row > 0 && size_t(row) <= indices.size()) {
          accepted = indices[size_t(row - 1)];
          list->window()->hide();
        }
      }
    } context{this, &query, &list, &select, {}, -1};
    query.when(FL_WHEN_CHANGED);
    query.callback([](Fl_Widget *, void *p) { static_cast<Context *>(p)->refresh(); }, &context);
    list.callback(
        [](Fl_Widget *, void *p) {
          auto c = static_cast<Context *>(p);
          if (Fl::event_clicks() || (Fl::event() == FL_KEYBOARD && Fl::event_key() == FL_Enter))
            c->accept();
        },
        &context);
    select.callback([](Fl_Widget *, void *p) { static_cast<Context *>(p)->accept(); }, &context);
    cancel.callback([](Fl_Widget *w, void *) { w->window()->hide(); });
    dialog.end();
    dialog.resizable(list);
    dialog.size_range(480, 320);
    context.refresh();
    dialog.set_modal();
    dialog.show();
    query.take_focus();
    while (dialog.shown())
      Fl::wait();
    if (context.accepted >= 0 && context.accepted != value()) {
      value(context.accepted);
      set_changed();
      do_callback();
    }
  }
};
} // namespace slicer
