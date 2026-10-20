/* Copyright 2011 Pierre Ossman <ossman@cendio.se> for Cendio AB
 * Copyright 2012 Samuel Mannehed <samuel@cendio.se> for Cendio AB
 * 
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <algorithm>
#include <libgen.h>
#include <sys/stat.h>

// FIXME: Workaround for FLTK including windows.h
#ifdef WIN32
#include <winsock2.h>
#endif

#include <FL/Fl.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Input_Choice.H>
#include <FL/Fl_Int_Input.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Return_Button.H>
#include <FL/fl_draw.H>
#include <FL/fl_ask.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Pixmap.H>
#include <FL/Fl_Tree.H>
#include <FL/filename.H>

#include <core/Exception.h>
#include <core/LogWriter.h>
#include <core/i18n.h>
#include <core/string.h>
#include <core/xdgdirs.h>

#include <network/TcpSocket.h>

#include "fltk/layout.h"
#include "fltk/util.h"
#include "fltk/Fl_Password_Input.h"
#include "ServerDialog.h"
#include "OptionsDialog.h"
#include "vncviewer.h"
#include "parameters.h"
#ifdef __APPLE__
#include "cocoa.h"
#endif

static core::LogWriter vlog("ServerDialog");

// xpm:s predate const, so they have invalid definitions
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wwrite-strings"
#include "fltk/folder.xpm"
#pragma GCC diagnostic pop

static Fl_Pixmap folderIcon(folder_xpm);

// Path below the "Sessions" root, as laid out on disk
static std::string sessionPath(const Fl_Tree_Item* item)
{
  std::string path;

  for (; item != nullptr && !item->is_root(); item = item->parent())
    path = path.empty() ? item->label() : item->label() + ("/" + path);

  return path;
}

static std::string toLower(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(), ::tolower);
  return s;
}

// Folders carry the folder icon, which also covers empty ones
static bool isFolder(const Fl_Tree_Item* item)
{
  return item->is_root() || item->usericon() == &folderIcon;
}

// What a new entry inside this folder starts its name with
static std::string folderPrefix(const Fl_Tree_Item* folder)
{
  std::string path = sessionPath(folder);
  return path.empty() ? path : path + "/";
}

static bool validName(const char* name)
{
  return name != nullptr && name[0] != '\0' && name[0] != '/' &&
         name[strlen(name) - 1] != '/';
}

// Makes room for a new session file or folder, never over an existing one
static bool claimPath(const std::string& path)
{
  struct stat st;

  if (stat(path.c_str(), &st) == 0) {
    fl_alert(_("%s already exists."), path.c_str());
    return false;
  }

  core::mkdir_p(path.substr(0, path.rfind('/')).c_str(), 0755);
  return true;
}

static bool moveEntry(const std::string& from, const std::string& to)
{
  if (!claimPath(to))
    return false;

  if (rename(from.c_str(), to.c_str()) != 0) {
    fl_alert(_("Unable to rename:\n\n%s"), strerror(errno));
    return false;
  }

  return true;
}

// FLTK 1.3 gives no callback when a selected item is clicked again, so
// report double clicks ourselves
class SessionTree : public Fl_Tree {
public:
  SessionTree(int X, int Y, int W, int H)
    : Fl_Tree(X, Y, W, H), dragged(nullptr), dropTarget(nullptr) {}

  // Set only during the callback for a session dropped onto an item
  Fl_Tree_Item* dragged;
  Fl_Tree_Item* dropTarget;

  int handle(int event) override {
    // Let the dialog show its menu instead of selecting
    if (event == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE) {
      do_callback();
      return 1;
    }

    // Only sessions move, and only once the mouse has really moved
    if (event == FL_PUSH) {
      dragged = find_clicked();
      if (dragged != nullptr && isFolder(dragged))
        dragged = nullptr;
    }
    if (dragged != nullptr && !Fl::event_is_click()) {
      if (event == FL_DRAG) {
        window()->cursor(FL_CURSOR_MOVE);
        return 1;
      }
      if (event == FL_RELEASE) {
        window()->cursor(FL_CURSOR_DEFAULT);
        dropTarget = find_clicked();
        if (dropTarget != nullptr)
          do_callback();
        dragged = dropTarget = nullptr;
        return 1;
      }
    }

    int ret = Fl_Tree::handle(event);
    if (event == FL_PUSH && Fl::event_clicks() > 0)
      do_callback();
    return ret;
  }
};

ServerDialog::ServerDialog()
  : Fl_Window(450, 0, "TigerVNC")
{
  int x, y, x2;
  Fl_Button *button;
  Fl_Box *divider;

  x = OUTER_MARGIN;
  y = OUTER_MARGIN;

  filter = new Fl_Input(LBLLEFT(x, y, w() - OUTER_MARGIN*2, INPUT_HEIGHT, _("Filter:")));
  filter->when(FL_WHEN_CHANGED);
  filter->callback(this->handleFilter, this);

  y += INPUT_HEIGHT + INNER_MARGIN;

  sessions = new SessionTree(x, y, w() - OUTER_MARGIN*2, 250);
  sessions->root_label(_("Sessions"));
  sessions->callback(this->handleSession, this);

  y += sessions->h() + INNER_MARGIN;

  const char* configDir = core::getvncconfigdir();
  if (configDir != nullptr) {
    sessionsDir = (std::string)configDir + "/sessions";
    core::mkdir_p(sessionsDir.c_str(), 0755);
    usedDir = sessionsDir;
    loadSessions();
  }

  x2 = x;

  button = new Fl_Button(x2, y, BUTTON_WIDTH, BUTTON_HEIGHT, _("Options..."));
  button->callback(this->handleOptions, this);
  x2 += BUTTON_WIDTH + INNER_MARGIN;

  button = new Fl_Button(x2, y, BUTTON_WIDTH, BUTTON_HEIGHT, _("Load..."));
  button->callback(this->handleLoad, this);
  x2 += BUTTON_WIDTH + INNER_MARGIN;

  button = new Fl_Button(x2, y, BUTTON_WIDTH, BUTTON_HEIGHT, _("Save as..."));
  button->callback(this->handleSaveAs, this);
  x2 += BUTTON_WIDTH + INNER_MARGIN;

  y += BUTTON_HEIGHT + INNER_MARGIN;

  divider = new Fl_Box(0, y, w(), 2);
  divider->box(FL_THIN_DOWN_FRAME);

  y += divider->h() + INNER_MARGIN;

  // Symmetric margin around bottom button bar
  y += OUTER_MARGIN - INNER_MARGIN;

  button = new Fl_Button(x, y, BUTTON_WIDTH, BUTTON_HEIGHT, _("About..."));
  button->callback(this->handleAbout, this);

  x2 = w() - OUTER_MARGIN - BUTTON_WIDTH*2 - INNER_MARGIN*1;

  button = new Fl_Button(x2, y, BUTTON_WIDTH, BUTTON_HEIGHT, _("Cancel"));
  button->callback(this->handleCancel, this);
  x2 += BUTTON_WIDTH + INNER_MARGIN;

  button = new Fl_Return_Button(x2, y, BUTTON_WIDTH, BUTTON_HEIGHT, _("Connect"));
  button->callback(this->handleConnect, this);
  x2 += BUTTON_WIDTH + INNER_MARGIN;

  y += BUTTON_HEIGHT + INNER_MARGIN;

  /* Needed for resize to work sanely */
  resizable(nullptr);
  h(y-INNER_MARGIN+OUTER_MARGIN);

  callback(this->handleCancel, this);
}


ServerDialog::~ServerDialog()
{
}


std::string ServerDialog::run(const char* /*servername*/)
{
  ServerDialog dialog;

  dialog.show();

  while (dialog.shown()) Fl::wait();

  return dialog.serverName;
}

void ServerDialog::handleOptions(Fl_Widget* /*widget*/, void* /*data*/)
{
  OptionsDialog::showDialog();
}


void ServerDialog::handleLoad(Fl_Widget* /*widget*/, void* data)
{
  ServerDialog *dialog = (ServerDialog*)data;

  if (dialog->usedDir.empty())
    dialog->usedDir = core::getuserhomedir();

  Fl_File_Chooser* file_chooser = new Fl_File_Chooser(dialog->usedDir.c_str(),
                                                      _("TigerVNC configuration (*.tigervnc)"),
                                                      Fl_File_Chooser::SINGLE,
                                                      _("Select a TigerVNC configuration file"));
  file_chooser->preview(0);
  file_chooser->previewButton->hide();
  file_chooser->show();
  
  // Block until user picks something.
  while(file_chooser->shown())
    Fl::wait();
  
  // Did the user hit cancel?
  if (file_chooser->value() == nullptr) {
    delete(file_chooser);
    return;
  }
  
  const char* filename = file_chooser->value();
  dialog->updateUsedDir(filename);

  try {
    dialog->serverName = loadViewerParameters(filename);
  } catch (std::exception& e) {
    vlog.error("%s", e.what());
    fl_alert(_("Unable to load the specified configuration file:\n\n%s"),
             e.what());
  }

  delete(file_chooser);
}


void ServerDialog::handleSaveAs(Fl_Widget* /*widget*/, void* data)
{ 
  ServerDialog *dialog = (ServerDialog*)data;
  const char* servername = dialog->serverName.c_str();
  const char* filename;
  if (dialog->usedDir.empty())
    dialog->usedDir = core::getuserhomedir();
  
  Fl_File_Chooser* file_chooser = new Fl_File_Chooser(dialog->usedDir.c_str(),
                                                      _("TigerVNC configuration (*.tigervnc)"),
                                                      Fl_File_Chooser::CREATE,
                                                      _("Save the TigerVNC configuration to file"));
  
  file_chooser->preview(0);
  file_chooser->previewButton->hide();
  file_chooser->show();
  
  while(1) {
    
    // Block until user picks something.
    while(file_chooser->shown())
      Fl::wait();
    
    // Did the user hit cancel?
    if (file_chooser->value() == nullptr) {
      delete(file_chooser);
      return;
    }
    
    filename = file_chooser->value();
    dialog->updateUsedDir(filename);
    
    FILE* f = fopen(filename, "r");
    if (f) {

      // The file already exists.
      fclose(f);
      int overwrite_choice = fl_choice(_("%s already exists. Do you want to overwrite?"), 
                                       nullptr, _("No"), _("Overwrite"), filename);
      if (overwrite_choice != 2) {

        // If the user doesn't want to overwrite:
        file_chooser->show();
        continue;
      }
    }

    break;
  }
  
  try {
    saveViewerParameters(filename, servername);
  } catch (std::exception& e) {
    vlog.error("%s", e.what());
    fl_alert(_("Unable to save the specified configuration "
               "file:\n\n%s"), e.what());
  }

  delete(file_chooser);

  if (!dialog->sessionsDir.empty())
    dialog->loadSessions();
}


void ServerDialog::handleFilter(Fl_Widget* /*widget*/, void* data)
{
  ServerDialog *dialog = (ServerDialog*)data;

  dialog->loadSessions();
  dialog->serverName.clear();

  // Pick the first match so that Enter connects to it
  if (dialog->filter->size() == 0)
    return;
  for (Fl_Tree_Item* item = dialog->sessions->first(); item;
       item = dialog->sessions->next(item)) {
    if (!isFolder(item)) {
      dialog->sessions->select(item);
      break;
    }
  }
}


void ServerDialog::handleSession(Fl_Widget* /*widget*/, void* data)
{
  ServerDialog *dialog = (ServerDialog*)data;
  SessionTree *tree = (SessionTree*)dialog->sessions;

  if (Fl::event() == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE) {
    dialog->sessionMenu(dialog->sessions->find_clicked());
    return;
  }

  if (tree->dropTarget != nullptr) {
    dialog->moveSession(tree->dragged, tree->dropTarget);
    return;
  }

  // A second callback for one double click is a no-op
  if (!dialog->shown())
    return;

  if (!dialog->loadSession(dialog->sessions->first_selected_item())) {
    // A folder or nothing is selected, so there is nothing to connect to
    dialog->serverName.clear();
    return;
  }

  if (Fl::event() == FL_PUSH && Fl::event_clicks() > 0)
    handleConnect(nullptr, dialog);
}


void ServerDialog::handleAbout(Fl_Widget* /*widget*/, void* /*data*/)
{
  about_vncviewer();
}


void ServerDialog::handleCancel(Fl_Widget* /*widget*/, void* data)
{
  ServerDialog *dialog = (ServerDialog*)data;

  dialog->serverName.clear();
  dialog->hide();
}


void ServerDialog::handleConnect(Fl_Widget* /*widget*/, void *data)
{
  ServerDialog *dialog = (ServerDialog*)data;
  const char* servername = dialog->serverName.c_str();

  // Nothing to connect to until a session is picked
  if (servername[0] == '\0')
    return;

  dialog->hide();

#ifdef __APPLE__
  // CConn::getUserPasswd() takes these instead of asking
  std::string user, password;
  if (cocoa_get_login((std::string("vnc:") + servername).c_str(),
                      &user, &password)) {
    if (!user.empty())
      setenv("VNC_USERNAME", user.c_str(), 1);
    setenv("VNC_PASSWORD", password.c_str(), 1);
  }
#endif

  try {
    saveViewerParameters(nullptr, servername);
  } catch (std::exception& e) {
    vlog.error(_("Unable to save the default configuration: %s"),
               e.what());
  }
}


void ServerDialog::updateUsedDir(const char* filename)
{
  char * name = strdup(filename);
  usedDir = dirname(name);
  free(name);
}

void ServerDialog::loadSessions(const std::string& subdir)
{
  struct dirent** list;
  int count;
  std::string match = toLower(filter->value());

  if (subdir.empty())
    sessions->clear_children(sessions->root());

  count = fl_filename_list((sessionsDir + "/" + subdir).c_str(), &list,
                           fl_casenumericsort);
  for (int i = 0; i < count; i++) {
    std::string name = list[i]->d_name;
    const std::string ext = ".tigervnc";

    if (name[0] == '.')
      continue;

    // Directories come back with a trailing slash
    if (name.back() == '/') {
      std::string path = subdir + name.substr(0, name.size() - 1);
      if (match.empty())
        sessions->add(path.c_str())->usericon(&folderIcon);
      loadSessions(subdir + name);
    } else if (name.size() > ext.size() &&
               name.compare(name.size() - ext.size(), ext.size(), ext) == 0) {
      std::string path = subdir + name.substr(0, name.size() - ext.size());
      if (toLower(path).find(match) != std::string::npos)
        sessions->add(path.c_str());
    }
  }
  if (count > 0)
    fl_filename_free_list(&list, count);

  if (!subdir.empty())
    return;

  for (Fl_Tree_Item* item = sessions->first(); item;
       item = sessions->next(item)) {
    if (item->is_root() || item->has_children())
      item->usericon(&folderIcon);
  }

  sessions->redraw();
}

bool ServerDialog::loadSession(Fl_Tree_Item* item)
{
  // Folders only fold
  if (item == nullptr || isFolder(item))
    return false;

  try {
    std::string filename = sessionsDir + "/" + sessionPath(item) +
                           ".tigervnc";
    serverName = loadViewerParameters(filename.c_str());
  } catch (std::exception& e) {
    vlog.error("%s", e.what());
    fl_alert(_("Unable to load the specified configuration file:\n\n%s"),
             e.what());
    return false;
  }

  return true;
}

void ServerDialog::sessionMenu(Fl_Tree_Item* item)
{
  Fl_Menu_Item menu[4] = {};
  const Fl_Menu_Item* picked;
  Fl_Tree_Item* folder;

  if (item == nullptr)
    item = sessions->root();

  // Right click picks a session just like a left click does
  if (loadSession(item))
    sessions->select_only(item, 0);

  if (!isFolder(item)) {
    menu[0].label(_("Connect"));
    menu[1].label(_("Settings..."));

    picked = menu->popup(Fl::event_x(), Fl::event_y());
    if (picked == &menu[0])
      handleConnect(nullptr, this);
    else if (picked == &menu[1])
      editSession(item->parent(), item);
    return;
  }

  folder = item;

  menu[0].label(_("New connection..."));
  menu[1].label(_("New folder..."));
  // The "Sessions" root itself stays as it is
  if (!item->is_root())
    menu[2].label(_("Rename folder..."));

  picked = menu->popup(Fl::event_x(), Fl::event_y());
  if (picked == &menu[0])
    editSession(folder, nullptr);
  else if (picked == &menu[1])
    newFolder(folder);
  else if (picked == &menu[2])
    renameFolder(item);
}

// Into the dropped-on folder, or the folder of the dropped-on session
void ServerDialog::moveSession(const Fl_Tree_Item* item,
                               const Fl_Tree_Item* target)
{
  const Fl_Tree_Item* folder = isFolder(target) ? target : target->parent();

  if (folder == item->parent())
    return;

  if (!moveEntry(sessionsDir + "/" + sessionPath(item) + ".tigervnc",
                 sessionsDir + "/" + folderPrefix(folder) + item->label() +
                 ".tigervnc"))
    return;

  loadSessions();
}

void ServerDialog::newFolder(const Fl_Tree_Item* folder)
{
  const char* input;
  std::string path;

  input = fl_input(_("Folder name (use / for subfolders):"),
                   folderPrefix(folder).c_str());
  if (!validName(input))
    return;

  path = sessionsDir + "/" + input;
  if (!claimPath(path))
    return;

  if (core::mkdir_p(path.c_str(), 0755) == -1) {
    fl_alert(_("Unable to create the folder:\n\n%s"), strerror(errno));
    return;
  }

  loadSessions();
}

void ServerDialog::renameFolder(const Fl_Tree_Item* folder)
{
  std::string oldName = sessionPath(folder);
  const char* input;

  input = fl_input(_("New folder name (use / for subfolders):"),
                   oldName.c_str());
  if (!validName(input) || oldName == input)
    return;

  if (!moveEntry(sessionsDir + "/" + oldName, sessionsDir + "/" + input))
    return;

  loadSessions();
}

// Settings of an existing session, or a new one when item is null
void ServerDialog::editSession(const Fl_Tree_Item* folder,
                               const Fl_Tree_Item* item)
{
  const char* labels[] = { _("Name:"), _("IP:"), _("Port:"),
                           _("Username:"), _("Password:") };
  std::string oldName, newName, server, host, user, password, filename;
  int port = 5900;
  int x, y, w;
  bool accepted;

  if (item != nullptr) {
    oldName = sessionPath(item);
    // sessionMenu() has just loaded it
    server = serverName;
    try {
      network::getHostAndPort(server.c_str(), &host, &port);
    } catch (std::exception& e) {
      host = server;
    }
#ifdef __APPLE__
    cocoa_get_login(("vnc:" + server).c_str(), &user, &password);
#endif
  }

  Fl_Window win(400, 0, item ? _("Connection settings") : _("New connection"));

  x = 0;
  for (const char* label : labels)
    x = std::max(x, gui_str_len(label));
  x += OUTER_MARGIN + TEXT_PADDING;
  w = win.w() - x - OUTER_MARGIN;
  y = OUTER_MARGIN;

  Fl_Input* nameIn = new Fl_Input(x, y, w, INPUT_HEIGHT, labels[0]);
  nameIn->value(item ? oldName.c_str() : folderPrefix(folder).c_str());
  y += INPUT_HEIGHT + INNER_MARGIN;

  Fl_Input* hostIn = new Fl_Input(x, y, w, INPUT_HEIGHT, labels[1]);
  hostIn->value(host.c_str());
  y += INPUT_HEIGHT + INNER_MARGIN;

  Fl_Int_Input* portIn = new Fl_Int_Input(x, y, 80, INPUT_HEIGHT, labels[2]);
  portIn->value(std::to_string(port).c_str());
  y += INPUT_HEIGHT + INNER_MARGIN;

  Fl_Input* userIn = new Fl_Input(x, y, w, INPUT_HEIGHT, labels[3]);
  userIn->value(user.c_str());
  y += INPUT_HEIGHT + INNER_MARGIN;

  Fl_Password_Input* passIn = new Fl_Password_Input(x, y, w, INPUT_HEIGHT,
                                                    labels[4]);
  passIn->value(password.c_str());
  y += INPUT_HEIGHT + INNER_MARGIN;

  y += OUTER_MARGIN - INNER_MARGIN;

  Fl_Button* cancel = new Fl_Button(win.w() - OUTER_MARGIN - BUTTON_WIDTH*2 -
                                    INNER_MARGIN, y,
                                    BUTTON_WIDTH, BUTTON_HEIGHT, _("Cancel"));
  cancel->callback([](Fl_Widget* b, void*) { b->window()->hide(); });

  Fl_Return_Button* ok = new Fl_Return_Button(win.w() - OUTER_MARGIN -
                                              BUTTON_WIDTH, y,
                                              BUTTON_WIDTH, BUTTON_HEIGHT,
                                              _("OK"));
  ok->callback([](Fl_Widget* b, void* data) {
    *(bool*)data = true;
    b->window()->hide();
  }, &accepted);

  y += BUTTON_HEIGHT + OUTER_MARGIN;

  win.end();
  win.size(win.w(), y);
  win.set_modal();

  // Show again until the input is usable, keeping what was typed
  while (true) {
    accepted = false;
    win.show();
    while (win.shown())
      Fl::wait();
    if (!accepted)
      return;

    newName = nameIn->value();
    host = hostIn->value();
    port = atoi(portIn->value());

    if (!validName(newName.c_str()))
      fl_alert(_("Please enter a name."));
    else if (host.empty() || (port <= 0) || (port > 65535))
      fl_alert(_("Please enter the IP and a port between 1 and 65535."));
    else
      break;
  }

  if (host.find(':') != std::string::npos)
    host = "[" + host + "]";
  // A port below 100 after a single colon would mean a display number
  server = core::format(port < 100 ? "%s::%d" : "%s:%d", host.c_str(), port);

  filename = sessionsDir + "/" + newName + ".tigervnc";
  if (item == nullptr) {
    if (!claimPath(filename))
      return;
  } else if (newName != oldName) {
    if (!moveEntry(sessionsDir + "/" + oldName + ".tigervnc", filename))
      return;
  }

  try {
    saveViewerParameters(filename.c_str(), server.c_str());
  } catch (std::exception& e) {
    vlog.error("%s", e.what());
    fl_alert(_("Unable to save the specified configuration "
               "file:\n\n%s"), e.what());
    return;
  }

#ifdef __APPLE__
  if ((passIn->value()[0] != '\0') &&
      !cocoa_set_login(("vnc:" + server).c_str(), userIn->value(),
                       passIn->value()))
    fl_alert(_("Unable to save the login to the keychain"));
#else
  (void)userIn;
#endif

  serverName = server;
  loadSessions();
}
