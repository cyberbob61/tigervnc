/* Copyright 2011 Pierre Ossman <ossman@cendio.se> for Cendio AB
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

#ifndef __SERVERDIALOG_H__
#define __SERVERDIALOG_H__

#include <FL/Fl_Window.H>
#include <string>

class Fl_Widget;
class Fl_Input;
class Fl_Input_Choice;
class Fl_Tree;
class Fl_Tree_Item;

class ServerDialog : public Fl_Window {
protected:
  ServerDialog();
  ~ServerDialog();

public:
  static std::string run(const char* servername);

protected:
  static void handleOptions(Fl_Widget *widget, void *data);
  static void handleLoad(Fl_Widget *widget, void *data);
  static void handleSaveAs(Fl_Widget *widget, void *data);
  static void handleAbout(Fl_Widget *widget, void *data);
  static void handleCancel(Fl_Widget *widget, void *data);
  static void handleConnect(Fl_Widget *widget, void *data);
  static void handleFilter(Fl_Widget *widget, void *data);
  static void handleSession(Fl_Widget *widget, void *data);

private:
  void updateUsedDir(const char* filename);
  void loadSessions(const std::string& subdir = "");
  bool loadSession(Fl_Tree_Item* item);
  void sessionMenu(Fl_Tree_Item* item);
  void moveSession(const Fl_Tree_Item* item, const Fl_Tree_Item* target);
  void newFolder(const Fl_Tree_Item* folder);
  void renameFolder(const Fl_Tree_Item* folder);
  void editSession(const Fl_Tree_Item* folder, const Fl_Tree_Item* item);

protected:
  Fl_Input *filter;
  Fl_Tree *sessions;
  std::string sessionsDir;
  // Server of the selected session, what Connect connects to
  std::string serverName;
  std::string usedDir;
};

#endif
