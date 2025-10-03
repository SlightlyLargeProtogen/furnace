/**
 * Furnace Tracker - multi-system chiptune tracker
 * Copyright (C) 2021-2025 tildearrow and contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

// this is the code to a new file picker using Dear ImGui.
// this replaces ImGuiFileDialog as the built-in file picker.

#include "newFilePicker.h"
#include "IconsFontAwesome4.h"
#include "misc/cpp/imgui_stdlib.h"
#include "../ta-log.h"
#include <algorithm>
#include <chrono>
#include <imgui.h>
#ifdef _WIN32
#include <windows.h>
#include <shlwapi.h>
#include "../utfutils.h"
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include "../fileutils.h"
#include <inttypes.h>
#include <time.h>

static const char* sizeSuffixes=".KMGTPEZ";

static void _fileThread(void* item) {
  ((FurnaceFilePicker*)item)->readDirectorySub();
}

static void _searchThread(void* item) {
  ((FurnaceFilePicker*)item)->searchSub("",0);
}

#ifdef _WIN32
#else
FurnaceFilePicker::FileEntry* FurnaceFilePicker::makeEntry(void* _entry, const char* prefix) {
  struct dirent* entry=(struct dirent*)_entry;
  FileEntry* newEntry=new FileEntry;
  if (prefix!=NULL) {
    if (prefix[0]=='/') {
      newEntry->name=String(&prefix[1])+"/"+entry->d_name;
    } else {
      newEntry->name=entry->d_name;
    }
  } else {
    newEntry->name=entry->d_name;
  }
  newEntry->nameLower=newEntry->name;
  newEntry->isHidden=(entry->d_name[0]=='.');
  for (char& i: newEntry->nameLower) {
    if (i>='A' && i<='Z') i+='a'-'A';
  }

  switch (entry->d_type) {
    case DT_REG:
      newEntry->type=FP_TYPE_NORMAL;
      break;
    case DT_DIR:
      newEntry->type=FP_TYPE_DIR;
      newEntry->isDir=true;
      break;
    case DT_LNK: {
      newEntry->type=FP_TYPE_LINK;
      // resolve link to determine whether this is a directory
      String readLinkPath;
      DIR* readLinkDir=NULL;
      if (path.empty()) {
        readLinkPath=newEntry->name;
      } else if (*path.rbegin()=='/') {
        readLinkPath=path+newEntry->name;
      } else {
        readLinkPath=path+'/'+newEntry->name;
      }
      // silly, but works.
      readLinkDir=opendir(readLinkPath.c_str());
      if (readLinkDir!=NULL) {
        newEntry->isDir=true;
        closedir(readLinkDir);
      }
      break;
    }
    case DT_SOCK:
      newEntry->type=FP_TYPE_SOCKET;
      break;
    case DT_FIFO:
      newEntry->type=FP_TYPE_PIPE;
      break;
    case DT_CHR:
      newEntry->type=FP_TYPE_CHARDEV;
      break;
    case DT_BLK:
      newEntry->type=FP_TYPE_BLOCKDEV;
      break;
    default:
      newEntry->type=FP_TYPE_UNKNOWN;
      break;
  }

  if (!newEntry->isDir) {
    size_t extPos=newEntry->name.rfind('.');
    if (extPos!=String::npos) {
      newEntry->ext=newEntry->name.substr(extPos);
      for (char& i: newEntry->ext) {
        if (i>='A' && i<='Z') i+='a'-'A';
      }
    }
  }

  return newEntry;
}
#endif

#ifdef _WIN32
// Windows implementation
void FurnaceFilePicker::readDirectorySub() {
  /// SPECIAL CASE: empty path returns drive list
  if (path=="") {
    /// STAGE 1: get list of drives
    unsigned int drives=GetLogicalDrives();

    for (int i=0; i<32; i++) {
      if (!(drives&(1U<<i))) continue;
      FileEntry* newEntry=new FileEntry;

      if (i>=26) {
        newEntry->name='A';
        newEntry->name+=('A'+(i-26));
      } else {
        newEntry->name+=('A'+i);
      }
      newEntry->name+=":\\";
      newEntry->nameLower=newEntry->name;
      for (char& i: newEntry->nameLower) {
        if (i>='A' && i<='Z') i+='a'-'A';
      }

      newEntry->isDir=true;
      newEntry->type=FP_TYPE_DIR;

      entries.push_back(newEntry);
    }

    haveFiles=true;

    /// STAGE 2: get drive information (size!)
    for (FileEntry* i: entries) {
      logV("get info for drive %s",i->name);
      DWORD bytes, sectors, freeClusters, clusters;
      WString nameW=utf8To16(i->name);
      if (GetDiskFreeSpaceW(nameW.c_str(),&sectors,&bytes,&freeClusters,&clusters)!=0) {
        i->size=(uint64_t)bytes*(uint64_t)sectors*(uint64_t)clusters;
	logV("SIZE: %" PRIu64,i->size);
        i->hasSize=true;
      } else {
        logE("COULD NOT..... %x",GetLastError());
      }
    }
    haveStat=true;
    return;
  }

  /// STAGE 1: get file list
  WString pathW=utf8To16(path.c_str());
  pathW+=L"\\*";
  WIN32_FIND_DATAW entry;
  SYSTEMTIME tempTM;
  HANDLE dir=FindFirstFileW(pathW.c_str(),&entry);
  if (dir==INVALID_HANDLE_VALUE) {
    wchar_t* errorStr=NULL;
    int errorSize=FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,NULL,GetLastError(),MAKELANGID(LANG_NEUTRAL,SUBLANG_DEFAULT),(wchar_t*)&errorStr,0,NULL);
    if (errorSize==0) {
      failMessage="Unknown error!";
    } else {
      failMessage=utf16To8(errorStr);
      // remove trailing new-line
      if (failMessage.size()>=2) failMessage.resize(failMessage.size()-2);
    }
    LocalFree(errorStr);

    haveFiles=true;
    haveStat=true;
    return;
  }

  do {
    if (wcscmp(entry.cFileName,L".")==0) continue;
    if (wcscmp(entry.cFileName,L"..")==0) continue;

    FileEntry* newEntry=new FileEntry;

    newEntry->name=utf16To8(entry.cFileName);
    newEntry->nameLower=newEntry->name;
    for (char& i: newEntry->nameLower) {
      if (i>='A' && i<='Z') i+='a'-'A';
    }

    newEntry->isDir=entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY;
    newEntry->isHidden=(entry.dwFileAttributes&FILE_ATTRIBUTE_HIDDEN) || (entry.dwFileAttributes&FILE_ATTRIBUTE_SYSTEM);
    newEntry->type=newEntry->isDir?FP_TYPE_DIR:FP_TYPE_NORMAL;

    if (!newEntry->isDir) {
      size_t extPos=newEntry->name.rfind('.');
      if (extPos!=String::npos) {
        newEntry->ext=newEntry->name.substr(extPos);
        for (char& i: newEntry->ext) {
          if (i>='A' && i<='Z') i+='a'-'A';
        }
      }
    }

    newEntry->size=((uint64_t)entry.nFileSizeHigh<<32)|(uint64_t)entry.nFileSizeLow;
    newEntry->hasSize=true;

    if (FileTimeToSystemTime(&entry.ftLastWriteTime,&tempTM)) {
      // we only use these
      newEntry->time.tm_year=tempTM.wYear-1900;
      newEntry->time.tm_mon=tempTM.wMonth-1;
      newEntry->time.tm_mday=tempTM.wDay;
      newEntry->time.tm_hour=tempTM.wHour;
      newEntry->time.tm_min=tempTM.wMinute;
      newEntry->time.tm_sec=tempTM.wSecond;

      newEntry->hasTime=true;
    }

    entryLock.lock();
    entries.push_back(newEntry);
    entryLock.unlock();
    if (stopReading) {
      break;
    }
  } while (FindNextFileW(dir,&entry)!=0);
  FindClose(dir);

  // on Windows, directory entries contain all the information the file picker needs.
  // no extra calls needed.
  haveFiles=true;
  haveStat=true;
}
#else
// Linux/Unix implementation
void FurnaceFilePicker::readDirectorySub() {
  /// STAGE 1: get file list
  DIR* dir=opendir(path.c_str());

  if (dir==NULL) {
    failMessage=strerror(errno);
    haveFiles=true;
    haveStat=true;
    return;
  }

  struct dirent* entry=NULL;
  while (true) {
    entry=readdir(dir);
    if (entry==NULL) break;
    if (strcmp(entry->d_name,".")==0) continue;
    if (strcmp(entry->d_name,"..")==0) continue;

    FileEntry* newEntry=makeEntry(entry,NULL);

    entryLock.lock();
    entries.push_back(newEntry);
    entryLock.unlock();
    if (stopReading) {
      break;
    }
  }
  if (closedir(dir)!=0) {
    // ?!
  }

  // we're done - this is sufficient to show a file list (and sort by name)
  haveFiles=true;

  /// STAGE 2: retrieve file information
  struct stat st;
  String filePath;
  for (FileEntry* i: entries) {
    if (stopReading) {
      return;
    }

    if (path.empty()) {
      filePath=i->name;
    } else if (*path.rbegin()=='/') {
      filePath=path+i->name;
    } else {
      filePath=path+'/'+i->name;
    }

    if (stat(filePath.c_str(),&st)<0) {
      // fall back to unknown
      continue;
    }

    // read file information
    entryLock.lock();
    struct tm* retTM=localtime_r(&st.st_mtime,&i->time);
    if (retTM!=NULL) {
      i->hasTime=true;
    }

    i->size=st.st_size;
    i->hasSize=true;
    entryLock.unlock();
  }
  haveStat=true;
}
#endif

#ifdef _WIN32
// Windows implementation
void FurnaceFilePicker::searchSub(String subPath, int depth) {
}
#else
// Linux/Unix implementation
void FurnaceFilePicker::searchSub(String subPath, int depth) {
  /// STAGE 1: get file list
  if (depth>15) logW("searchSub(%s,%d)",subPath,depth);
  String actualPath=path+subPath;
  DIR* dir=opendir(actualPath.c_str());

  if (dir==NULL) {
    if (depth==0) {
      failMessage=strerror(errno);
      haveFiles=true;
      haveStat=true;
    }
    return;
  }

  struct dirent* entry=NULL;
  while (true) {
    entry=readdir(dir);
    if (entry==NULL) break;
    if (strcmp(entry->d_name,".")==0) continue;
    if (strcmp(entry->d_name,"..")==0) continue;

    if (entry->d_type==DT_DIR) {
      //if (depth<20) {
        searchSub(subPath+String("/")+entry->d_name,depth+1);
      //}
    } else {
      String lower=entry->d_name;
      for (char& i: lower) {
        if (i>='A' && i<='Z') i+='a'-'A';
      }

      if (lower.find(searchQuery)!=String::npos) {
        FileEntry* newEntry=makeEntry(entry,subPath.c_str());
        entryLock.lock();
        entries.push_back(newEntry);
        entryLock.unlock();
      }
    }

    if (stopReading) {
      break;
    }
  }
  if (closedir(dir)!=0) {
    // ?!
  }

  if (depth==0) {
    haveFiles=true;
    haveStat=true;
    return;
  }
}
#endif

String FurnaceFilePicker::normalizePath(const String& which) {
  String ret;
#ifdef _WIN32
  wchar_t temp[4096];
  memset(temp,0,4096*sizeof(wchar_t));
#else
  char temp[PATH_MAX];
  memset(temp,0,PATH_MAX);
#endif

#ifndef _WIN32
  // don't reject the root on Linux/Unix
  if (which=="/") {
    ret=which;
    return ret;
  }
#endif

  if (which.empty()) {
    // on Windows we don't reject an empty path as it has a special meaning
#ifndef _WIN32
    if (getcwd(temp,PATH_MAX)==NULL) {
      // sorry...
      return "";
    }
    ret=temp;
#endif
  } else {
    // remove redundant directory separators
    bool alreadySep=false;
    for (const char& i: which) {
      if (i==DIR_SEPARATOR) {
        if (!alreadySep) {
          alreadySep=true;
          ret+=i;
        }
      } else {
        alreadySep=false;
        ret+=i;
      }
    }
  }

  if (!ret.empty()) {
#ifdef _WIN32
    // resolve parh
    WString retW=utf8To16(ret);
    if (GetFullPathNameW(retW.c_str(),4095,temp,NULL)!=0) {
      ret=utf16To8(temp);
    }

    // if this is the root of a drive, don't remove dir separator
    if (ret.size()>=5) {
      // remove dir separator at the end
      if (*ret.rbegin()==DIR_SEPARATOR) {
        ret.resize(ret.size()-1);
      }
    }
#else
    // resolve path
    if (realpath(ret.c_str(),temp)!=NULL) {
      ret=temp;
    }

    // remove dir separator at the end
    if (*ret.rbegin()==DIR_SEPARATOR) {
      ret.resize(ret.size()-1);
    }
#endif
  }

  return ret;
}

void FurnaceFilePicker::readDirectory(String path) {
  if (fileThread!=NULL) {
    // stop current file thread
    stopReading=true;
    fileThread->join();
    delete fileThread;
    fileThread=NULL;
  }

  // clear all entries
  sortedEntries.clear();
  for (FileEntry* i: entries) {
    delete i;
  }
  entries.clear();
  chosenEntries.clear();
  updateEntryName();
  if (!entryNameHint.empty()) {
    entryName=entryNameHint;
  }

  // start new file thread
  this->path=normalizePath(path);
  failMessage="";
  editingPath=false;
  haveFiles=false;
  haveStat=false;
  stopReading=false;
  scheduledSort=1;
  if (isSearch) {
    fileThread=new std::thread(_searchThread,this);
  } else {
    fileThread=new std::thread(_fileThread,this);
  }

  // check whether this path is bookmarked
  isPathBookmarked=false;
  for (String& i: bookmarks) {
    size_t separator=i.find('\n');
    if (separator==String::npos) continue;
    String iName=i.substr(0,separator);
    String iPath=i.substr(separator+1);

    if (this->path==iPath) {
      isPathBookmarked=true;
      break;
    }
  }
}

void FurnaceFilePicker::setHomeDir(String where) {
  homeDir=where;
}

void FurnaceFilePicker::updateEntryName() {
  if (chosenEntries.empty()) {
    entryName="";
  } else if (chosenEntries.size()>1) {
    entryName="<multiple files selected>";
  } else {
    entryName=chosenEntries[0]->name;
  }
}

// the name of this function is somewhat misleading.
// it filters files by type/hidden status, then sorts them.
void FurnaceFilePicker::sortFiles() {
  std::chrono::high_resolution_clock::time_point timeStart=std::chrono::high_resolution_clock::now();
  entryLock.lock();
  // check for "no filter"
  if (filterOptions[curFilterType+1]=="*") {
    // only filter hidden files if needed
    sortedEntries.clear();
    for (FileEntry* i: entries) {
      if (i->isHidden && !showHiddenFiles) continue;
      sortedEntries.push_back(i);
    }
  } else {
    // sort by extension
    std::vector<String> parsedSort;
    String nextType;
    for (char i: filterOptions[curFilterType+1]) {
      switch (i) {
        case '*': // ignore
          break;
        case ' ': // separator
          if (!nextType.empty()) {
            parsedSort.push_back(nextType);
            nextType="";
          }
          break;
        default: // push
          nextType.push_back(i);
          break;
      }
    }
    if (!nextType.empty()) {
      parsedSort.push_back(nextType);
      nextType="";
    }

    sortedEntries.clear();
    for (FileEntry* i: entries) {
      if (i->isHidden && !showHiddenFiles) continue;
      if (i->isDir) {
        sortedEntries.push_back(i);
        continue;
      }
      for (const String& j: parsedSort) {
        if (i->ext==j) {
          sortedEntries.push_back(i);
          break;
        }
      }
    }
  }

  // sort by name
  std::sort(sortedEntries.begin(),sortedEntries.end(),[this](const FileEntry* a, const FileEntry* b) -> bool {
    if (sortDirsFirst) {
      if (a->isDir && !b->isDir) return true;
      if (!a->isDir && b->isDir) return false;
    }

    switch (sortMode) {
      case FP_SORT_NAME: {
        // don't do anything. this is handled below.
        break;
      }
      case FP_SORT_EXT: {
        int result=a->ext.compare(b->ext);
        // only sort if extensions differ
        if (result!=0) {
          return result<0;
        }
        break;
      }
      case FP_SORT_SIZE: {
        // only sort if sizes differ
        if (a->size!=b->size) {
          return a->size<b->size;
        }
        break;
      }
      case FP_SORT_DATE: {
        // only sort if dates differ
        if (a->time.tm_year==b->time.tm_year) {
          if (a->time.tm_mon==b->time.tm_mon) {
            if (a->time.tm_mday==b->time.tm_mday) {
              if (a->time.tm_hour==b->time.tm_hour) {
                if (a->time.tm_min==b->time.tm_min) {
                  if (a->time.tm_sec==b->time.tm_sec) {
                    // fall back to sorting by name
                    return a->nameLower<b->nameLower;
                  }
                  return a->time.tm_sec<b->time.tm_sec;
                }
                return a->time.tm_min<b->time.tm_min;
              }
              return a->time.tm_hour<b->time.tm_hour;
            }
            return a->time.tm_mday<b->time.tm_mday;
          }
          return a->time.tm_mon<b->time.tm_mon;
        }
        return a->time.tm_year<b->time.tm_year;
        break;
      }
      case FP_SORT_MAX:
        // impossible
        break;
    }

    // fall back to sorting by name
    return a->nameLower<b->nameLower;
  });
  entryLock.unlock();
  std::chrono::high_resolution_clock::time_point timeEnd=std::chrono::high_resolution_clock::now();
  logV("sortFiles() took %dµs",std::chrono::duration_cast<std::chrono::microseconds>(timeEnd-timeStart).count());
}

void FurnaceFilePicker::filterFiles() {
  if (filter.empty() || isSearch) {
    filteredEntries=sortedEntries;
    return;
  }

  filteredEntries.clear();

  String lowerFilter=filter;
  for (char& i: lowerFilter) {
    if (i>='A' && i<='Z') i+='a'-'A';
  }

  for (FileEntry* i: sortedEntries) {
    if (i->nameLower.find(lowerFilter)!=String::npos) {
      filteredEntries.push_back(i);
    }
  }
}

bool FurnaceFilePicker::isPathAbsolute(const String& p) {
#ifdef _WIN32
  // TODO: test for UNC path?

  // convert to absolute path if necessary
  bool willConvert=(p.size()<3);
  if (!willConvert) {
    // test for drive letter
    if (!(((p[0]>='A' && p[0]<='Z') || (p[0]>='a' && p[0]<='z')) && p[1]==':' && p[2]=='\\')) {
      if (p.size()<4) {
        willConvert=true;
      } else {
        if (!(((p[0]>='A' && p[0]<='Z') || (p[0]>='a' && p[0]<='z')) && ((p[1]>='A' && p[1]<='Z') || (p[1]>='a' && p[1]<='z')) && p[2]==':' && p[3]=='\\')) {
          willConvert=true;
        }
      }
    }
  }

  return !willConvert;
#else
  if (p.size()<1) return false;
  return (p[0]=='/');
#endif
}

void FurnaceFilePicker::addBookmark(const String& p, String n) {
  if (p==path) isPathBookmarked=true;
  for (String& i: bookmarks) {
    size_t separator=i.find('\n');
    if (separator==String::npos) continue;
    String iName=i.substr(0,separator);
    String iPath=i.substr(separator+1);

    if (p==iPath) return;
  }
  if (n.empty()) {
    size_t lastSep=p.rfind(DIR_SEPARATOR);
    if (lastSep==String::npos) {
      String name=p;
      name+="\n";
      name+=p;

      bookmarks.push_back(name);
    } else {
      String name=p.substr(lastSep+1);
      name+="\n";
      name+=p;

      bookmarks.push_back(name);
    }
  } else {
    String name=n;
    name+="\n";
    name+=p;

    bookmarks.push_back(name);
  }
}

void FurnaceFilePicker::setSizeConstraints(const ImVec2& min, const ImVec2& max) {
  minSize=min;
  maxSize=max;
  hasSizeConstraints=true;
}

void FurnaceFilePicker::drawFileList(ImVec2& tableSize, bool& acknowledged) {
  // display a message on empty dir, no matches or error
  if (filteredEntries.empty()) {
    if (ImGui::BeginTable("NoFiles",3,ImGuiTableFlags_BordersOuter,tableSize)) {
      ImGui::TableSetupColumn("c0",ImGuiTableColumnFlags_WidthStretch,0.5f);
      ImGui::TableSetupColumn("c1",ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("c2",ImGuiTableColumnFlags_WidthStretch,0.5f);

      ImGui::TableNextRow();
      ImGui::TableNextColumn();

      ImGui::TableNextColumn();
      ImGui::SetCursorPosY(ImGui::GetCursorPosY()+(tableSize.y-ImGui::GetTextLineHeight())*0.5);
      if (haveFiles) {
        if (sortedEntries.empty()) {
          if (failMessage.empty()) {
            ImGui::Text("This directory is empty!");
          } else {
#ifdef _WIN32
            ImGui::Text("%s",failMessage.c_str());
#else
            ImGui::Text("%s!",failMessage.c_str());
#endif
          }
        } else {
          if (failMessage.empty()) {
            ImGui::Text("No results");
          } else {
#ifdef _WIN32
            ImGui::Text("%s",failMessage.c_str());
#else
            ImGui::Text("%s!",failMessage.c_str());
#endif
          }
        }
      } else {
        // don't
      }

      ImGui::TableNextColumn();
      ImGui::EndTable();
    }
  } else {
    // this is the list view. I might add other view modes in the future...
    int columns=1;
    if (displayType) columns++;
    if (displaySize) columns++;
    if (displayDate) columns++;
    if (ImGui::BeginTable("FileList",columns,ImGuiTableFlags_BordersOuter|ImGuiTableFlags_ScrollY|ImGuiTableFlags_RowBg,tableSize)) {
      float rowHeight=ImGui::GetTextLineHeight()+ImGui::GetStyle().CellPadding.y*2.0f;
      ImGui::TableSetupColumn("c0",ImGuiTableColumnFlags_WidthStretch);
      if (displayType) ImGui::TableSetupColumn("c1",ImGuiTableColumnFlags_WidthFixed,ImGui::CalcTextSize(" .eeee").x);
      if (displaySize) ImGui::TableSetupColumn("c2",ImGuiTableColumnFlags_WidthFixed,ImGui::CalcTextSize(" 999.99G").x);
      if (displayDate) ImGui::TableSetupColumn("c3",ImGuiTableColumnFlags_WidthFixed,ImGui::CalcTextSize(" 6969/69/69 04:20").x);
      ImGui::TableSetupScrollFreeze(0,1);

      // header (sort options)
      const char* nameHeader="Name##SortName";
      const char* typeHeader="Type##SortType";
      const char* sizeHeader="Size##SortSize";
      const char* dateHeader="Date##SortDate";

      switch (sortMode) {
        case FP_SORT_NAME:
          if (sortInvert[FP_SORT_NAME]) {
            nameHeader=ICON_FA_CHEVRON_UP "Name##SortName";
          } else {
            nameHeader=ICON_FA_CHEVRON_DOWN "Name##SortName";
          }
          break;
        case FP_SORT_EXT:
          if (sortInvert[FP_SORT_EXT]) {
            typeHeader=ICON_FA_CHEVRON_UP "Type##SortType";
          } else {
            typeHeader=ICON_FA_CHEVRON_DOWN "Type##SortType";
          }
          break;
        case FP_SORT_SIZE:
          if (sortInvert[FP_SORT_SIZE]) {
            sizeHeader=ICON_FA_CHEVRON_UP "Size##SortSize";
          } else {
            sizeHeader=ICON_FA_CHEVRON_DOWN "Size##SortSize";
          }
          break;
        case FP_SORT_DATE:
          if (sortInvert[FP_SORT_DATE]) {
            dateHeader=ICON_FA_CHEVRON_UP "Date##SortDate";
          } else {
            dateHeader=ICON_FA_CHEVRON_DOWN "Date##SortDate";
          }
          break;
        case FP_SORT_MAX:
          // impossible
          break;
      }

      ImGui::TableNextRow(ImGuiTableRowFlags_Headers,rowHeight);
      ImGui::TableNextColumn();
      if (ImGui::Selectable(nameHeader)) {
        if (sortMode==FP_SORT_NAME) {
          sortInvert[sortMode]=!sortInvert[sortMode];
        } else {
          sortMode=FP_SORT_NAME;
          scheduledSort=1;
        }
      }
      if (displayType) {
        ImGui::TableNextColumn();
        if (ImGui::Selectable(typeHeader)) {
          if (sortMode==FP_SORT_EXT) {
            sortInvert[sortMode]=!sortInvert[sortMode];
          } else {
            sortMode=FP_SORT_EXT;
            scheduledSort=1;
          }
        }
      }
      if (displaySize) {
        ImGui::TableNextColumn();
        if (ImGui::Selectable(sizeHeader)) {
          if (sortMode==FP_SORT_SIZE) {
            sortInvert[sortMode]=!sortInvert[sortMode];
          } else {
            sortMode=FP_SORT_SIZE;
            scheduledSort=1;
          }
        }
      }
      if (displayDate) {
        ImGui::TableNextColumn();
        if (ImGui::Selectable(dateHeader)) {
          if (sortMode==FP_SORT_DATE) {
            sortInvert[sortMode]=!sortInvert[sortMode];
          } else {
            sortMode=FP_SORT_DATE;
            scheduledSort=1;
          }
        }
      }

      // file list
      entryLock.lock();
      int index=0;
      listClipper.Begin(filteredEntries.size(),rowHeight);
      while (listClipper.Step()) {
        for (int _i=listClipper.DisplayStart; _i<listClipper.DisplayEnd; _i++) {
          FileEntry* i=filteredEntries[sortInvert[sortMode]?(filteredEntries.size()-_i-1):_i];
          FileTypeStyle* style=&defaultTypeStyle[i->type];

          // get style for this entry
          if (i->isDir) {
            style=&defaultTypeStyle[FP_TYPE_DIR];
          } else {
            if (!i->ext.empty()) {
              for (FileTypeStyle& j: fileTypeRegistry) {
                if (i->ext==j.ext) {
                  style=&j;
                  break;
                }
              }
            }
          }

          // draw
          ImGui::TableNextRow(0,rowHeight);
          // name
          ImGui::TableNextColumn();
          ImGui::PushStyleColor(ImGuiCol_Text,ImGui::GetColorU32(style->color));
          ImGui::PushID(index++);
          if (ImGui::Selectable(style->icon.c_str(),i->isSelected,ImGuiSelectableFlags_AllowDoubleClick|ImGuiSelectableFlags_SpanAllColumns|ImGuiSelectableFlags_SpanAvailWidth)) {
            bool doNotAcknowledge=false;
            if ((ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift)) && multiSelect) {
              // multiple selection
              doNotAcknowledge=true;
            } else {
              // clear selected entries
              for (FileEntry* j: chosenEntries) {
                j->isSelected=false;
              }
              chosenEntries.clear();
            }

            bool alreadySelected=false;
            for (FileEntry* j: chosenEntries) {
              if (j==i) alreadySelected=true;
            }

            if (!alreadySelected) {
              // select this entry
              chosenEntries.push_back(i);
              i->isSelected=true;
              updateEntryName();
              if (!doNotAcknowledge) {
                if (isMobile || singleClickSelect) {
                  acknowledged=true;
                } else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                  acknowledged=true;
                }
              }
            }
          }
          ImGui::PopID();
          ImGui::SameLine();
          
          ImGui::TextUnformatted(i->name.c_str());

          // type
          if (displayType) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(i->ext.c_str());
          }

          // size
          if (displaySize) {
            ImGui::TableNextColumn();
            if (i->hasSize && (i->type==FP_TYPE_NORMAL || path.empty())) {
              int sizeShift=0;
              uint64_t sizeShifted=i->size;

              while (sizeShifted && sizeShift<7) {
                sizeShifted>>=10;
                sizeShift++;
              }

              sizeShift--;

              uint64_t intPart=i->size>>(sizeShift*10);
              uint64_t fracPart=i->size&((1U<<(sizeShift*10))-1);
              // shift so we have sufficient digits for 100
              // (precision loss is negligible)
              if (sizeShift>0) {
                fracPart=(100*(fracPart>>3))>>((sizeShift*10)-3);
                if (fracPart>99) fracPart=99;
                ImGui::Text("%" PRIu64 ".%02" PRIu64 "%c",intPart,fracPart,sizeSuffixes[sizeShift&7]);
              } else {
                ImGui::Text("%" PRIu64,i->size);
              }
            }
          }

          // date
          if (displayDate) {
            ImGui::TableNextColumn();
            if (i->hasTime) {
              ImGui::Text("%d/%02d/%02d %02d:%02d",i->time.tm_year+1900,i->time.tm_mon+1,i->time.tm_mday,i->time.tm_hour,i->time.tm_min);
            }
          }

          ImGui::PopStyleColor();
        }
      }
      ImGui::EndTable();
      entryLock.unlock();
    }
  }
}

void FurnaceFilePicker::drawBookmarks(ImVec2& tableSize, String& newDir) {
  if (ImGui::BeginTable("BookmarksList",1,ImGuiTableFlags_BordersOuter|ImGuiTableFlags_ScrollY,tableSize)) {
    ImGui::TableSetupScrollFreeze(0,1);
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    ImGui::TableNextColumn();
    ImGui::Text("Bookmarks");
    ImGui::SameLine();
    float iconSize=ImGui::CalcTextSize(ICON_FA_PLUS).x;
    if (ImGui::Selectable(ICON_FA_PLUS "##AddBookmark",false,0,ImVec2(iconSize,0))) {
      newBookmarkName="New Bookmark";
      newBookmarkPath=path;
    }
    if (ImGui::BeginPopupContextItem("NewBookmark",ImGuiPopupFlags_MouseButtonLeft)) {
      ImGui::Text("Name");
      ImGui::InputText("##NameI",&newBookmarkName);

      ImGui::Text("Path:");
      ImGui::InputText("##PathI",&newBookmarkPath);

      ImGui::BeginDisabled(newBookmarkName.empty() || newBookmarkPath.empty());
      if (ImGui::Button("OK")) {
        if (newBookmarkName.empty() || newBookmarkPath.empty()) {
          // no!
        } else {
          newBookmarkPath=normalizePath(newBookmarkPath);
          addBookmark(newBookmarkPath,newBookmarkName);
        }
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndDisabled();
      ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Selectable(ICON_FA_TIMES "##CloseBookmarks",false,0,ImVec2(iconSize,0))) {
      showBookmarks=false;
    }

    int index=-1;
    int markedForRemoval=-1;
    for (String& i: bookmarks) {
      ++index;
      size_t separator=i.find('\n');
      if (separator==String::npos) continue;
      String iName=i.substr(0,separator);
      String iPath=i.substr(separator+1);

      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::PushID(index);
      if (ImGui::Selectable(iName.c_str(),iPath==path)) {
        newDir=iPath;
      }
      if (ImGui::BeginPopupContextItem("BookmarkOpts")) {
        if (ImGui::MenuItem("remove")) {
          markedForRemoval=index;
          if (iPath==path) isPathBookmarked=false;
        }
        ImGui::EndPopup();
      }
      ImGui::PopID();
    }
    if (markedForRemoval>=0) {
      bookmarks.erase(bookmarks.begin()+markedForRemoval);
    }
    ImGui::EndTable();
  }
}

bool FurnaceFilePicker::draw(ImGuiWindowFlags winFlags) {
  if (!isOpen) {
    hasSizeConstraints=false;
    return false;
  }

  String newDir;
  bool acknowledged=false;
  bool readDrives=false;
  bool wantSearch=false;

  bool began=false;

  // center the window if it is unmovable and not an embed
  if ((winFlags&ImGuiWindowFlags_NoMove) && !isEmbed) {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),ImGuiCond_Always,ImVec2(0.5f,0.5f));
  }

  if (isEmbed) {
    began=true;
  } else if (isModal) {
    ImGui::OpenPopup(windowName.c_str());
    if (hasSizeConstraints) ImGui::SetNextWindowSizeConstraints(minSize,maxSize);
    began=ImGui::BeginPopupModal(windowName.c_str(),NULL,ImGuiWindowFlags_NoScrollbar|winFlags);
  } else {
    if (hasSizeConstraints) ImGui::SetNextWindowSizeConstraints(minSize,maxSize);
    began=ImGui::Begin(windowName.c_str(),NULL,ImGuiWindowFlags_NoScrollbar|winFlags);
  }

  if (began) {
    // enforce window constraints if necessary
    if (hasSizeConstraints) {
      if (ImGui::GetWindowSize().x<minSize.x || ImGui::GetWindowSize().y<minSize.y) {
        ImGui::SetWindowSize(minSize,ImGuiCond_Always);
      }
    }

    // header bars
    if (ImGui::Button(ICON_FA_PLUS "##MakeDir")) {
      mkdirError="";
      mkdirPath="";
    }
    if (ImGui::BeginPopupContextItem("CreateDir",ImGuiPopupFlags_MouseButtonLeft)) {
      if (mkdirError.empty()) {
        ImGui::Text("Directory name:");

        ImGui::InputText("##DirName",&mkdirPath);

        ImGui::BeginDisabled(mkdirPath.empty());
        if (ImGui::Button("OK")) {
          if (mkdirPath.empty()) {
            mkdirError="Maybe try that again under better circumstances...";
          } else {
#ifdef _WIN32
            if (!isPathAbsolute(mkdirPath)) {
              if (path.empty()) {
                // error out in the drives view
                mkdirError="Trying to create a directory in the drives list";
              } else if (*path.rbegin()=='\\') {
                mkdirPath=path+mkdirPath;
              } else {
                mkdirPath=path+'\\'+mkdirPath;
              }
            }

            // create directory
            if (mkdirError.empty()) {
              WString mkdirPathW=utf8To16(mkdirPath);
              if (CreateDirectoryW(mkdirPathW.c_str(),NULL)==0) {
                wchar_t* errorStr=NULL;
                int errorSize=FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,NULL,GetLastError(),MAKELANGID(LANG_NEUTRAL,SUBLANG_DEFAULT),(wchar_t*)&errorStr,0,NULL);
                if (errorSize==0) {
                  mkdirError="Unknown error";
                } else {
                  mkdirError=utf16To8(errorStr);
                  // remove trailing new-line
                  if (mkdirError.size()>=2) mkdirError.resize(mkdirError.size()-2);
                }
                LocalFree(errorStr);
              } else {
                newDir=mkdirPath;
                ImGui::CloseCurrentPopup();
              }
            }
#else
            // convert to absolute path if necessary
            if (!isPathAbsolute(mkdirPath)) {
              if (!path.empty()) {
                if (*path.rbegin()=='/') {
                  mkdirPath=path+mkdirPath;
                } else {
                  mkdirPath=path+'/'+mkdirPath;
                }
              }
            }

            // create directory
            int result=mkdir(mkdirPath.c_str(),0755);
            if (result!=0) {
              mkdirError=strerror(errno);
            } else {
              newDir=mkdirPath;
              ImGui::CloseCurrentPopup();
            }
#endif
          }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
          ImGui::CloseCurrentPopup();
        }
      } else {
        ImGui::Text("I can't! (%s)\nCheck whether the path is correct and you have access to it.",mkdirError.c_str());
        if (ImGui::Button("Back")) {
          mkdirError="";
        }
      }
      ImGui::EndPopup();
    }

    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();

    if (ImGui::Button(ICON_FA_HOME "##HomeDir")) {
      newDir=homeDir;
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_CHEVRON_UP "##ParentDir")) {
      logV("Parent dir......");
      size_t pos=path.rfind(DIR_SEPARATOR);
#ifdef _WIN32
      if (pos==2 || pos==3) {
        if (path.size()<5) {
          newDir="";
          readDrives=true;
        } else {
          newDir=path.substr(0,pos+1);
        }
      } else if (pos!=String::npos) {
        newDir=path.substr(0,pos);
        if (newDir.empty()) readDrives=true;
      }
#else
      // stop at the root
      if (pos!=String::npos && path!="/") {
        newDir=path.substr(0,pos);
        if (newDir.empty()) newDir="/";
      }
#endif
    }
#ifdef _WIN32
    // drives button only on Windows
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_HDD_O "##DriveList")) {
      newDir="";
      readDrives=true;
    }
#endif
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_PENCIL "##EditPath")) {
      editablePath=path;
      editingPath=true;
    }

    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);

    if (editingPath) {
      ImGui::SameLine();
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x-(ImGui::GetStyle().ItemSpacing.x+ImGui::GetStyle().FramePadding.x*2.0f+ImGui::CalcTextSize("OK").x));
      ImGui::InputText("##EditablePath",&editablePath);
      ImGui::SameLine();
      if (ImGui::Button("OK##AcceptPath")) {
        newDir=editablePath;
      }
    } else {
      // explode path into buttons
      String pathLeading=path;
      if (!path.empty()) {
        if (*path.rbegin()!=DIR_SEPARATOR) pathLeading+=DIR_SEPARATOR_STR;
      }
#ifdef _WIN32
      String nextButton;
#else
      String nextButton="/";
#endif
      String pathAsOfNow;
      int pathLevel=0x10000000;
      for (char i: pathLeading) {
        pathAsOfNow+=i;
        if (i==DIR_SEPARATOR) {
          // create button
          ImGui::PushID(pathLevel);
          ImGui::SameLine();
          if (ImGui::Button(nextButton.c_str())) {
            newDir=pathAsOfNow;
          }
          pathLevel++;
          ImGui::PopID();
          nextButton="";
        } else {
          nextButton+=i;
        }
      }
    }

    // search bar
    if (ImGui::Button(isPathBookmarked?(ICON_FA_BOOKMARK "##Bookmarks"):(ICON_FA_BOOKMARK_O "##Bookmarks"))) {
      if (isPathBookmarked && showBookmarks) {
        for (size_t i=0; i<bookmarks.size(); i++) {
          size_t separator=bookmarks[i].find('\n');
          if (separator==String::npos) continue;
          String iName=bookmarks[i].substr(0,separator);
          String iPath=bookmarks[i].substr(separator+1);

          if (iPath==path) {
            bookmarks.erase(bookmarks.begin()+i);
            break;
          }
        }
        isPathBookmarked=false;
      } else {
        addBookmark(path);
      }
      showBookmarks=true;
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_COG "##Settings")) {
    }
    if (ImGui::BeginPopupContextItem("FilePickerSettings",ImGuiPopupFlags_MouseButtonLeft)) {
      ImGui::Checkbox("Show bookmarks",&showBookmarks);
      if (ImGui::Checkbox("Show hidden files",&showHiddenFiles)) {
        scheduledSort=1;
      }
      ImGui::Checkbox("Single click selects entries",&singleClickSelect);
      ImGui::Checkbox("Clear search query when changing directory",&clearSearchOnDirChange);
      if (ImGui::Checkbox("Sort directories first",&sortDirsFirst)) {
        scheduledSort=1;
      }
      ImGui::Text("Columns to display:");
      ImGui::Indent();
      ImGui::Checkbox("Type",&displayType);
      ImGui::Checkbox("Size",&displaySize);
      ImGui::Checkbox("Date",&displayDate);
      ImGui::Unindent();
      ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x-(ImGui::GetStyle().ItemSpacing.x+ImGui::GetStyle().FramePadding.x*2.0f+ImGui::CalcTextSize(ICON_FA_SEARCH).x));
    if (ImGui::InputTextWithHint("##Filter","Search",&filter)) {
      filterFiles();
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_SEARCH "##Recurse")) {
      newDir=path;
      searchQuery=filter;
      for (char& i: searchQuery) {
        if (i>='A' && i<='Z') i+='a'-'A';
      }
      wantSearch=true;
    }

    if (scheduledSort) {
      if (haveStat) {
        scheduledSort=0;
      }
      sortFiles();
      filterFiles();
    }

    ImVec2 tableSize=ImGui::GetContentRegionAvail();
    tableSize.y-=ImGui::GetFrameHeightWithSpacing()*2.0f;

    // bookmarks view, if open
    if (showBookmarks) {
      ImVec2 oldCellPadding=ImGui::GetStyle().CellPadding;
      ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,ImVec2(oldCellPadding.x,0));
      if (ImGui::BeginTable("BMPanel",2,ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("left",ImGuiTableColumnFlags_WidthStretch,0.2f);
        ImGui::TableSetupColumn("right",ImGuiTableColumnFlags_WidthStretch,0.8f);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImVec2 bookmarksSize=tableSize;
        bookmarksSize.x=ImGui::GetContentRegionAvail().x;
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,oldCellPadding);
        drawBookmarks(bookmarksSize,newDir);
        ImGui::PopStyleVar();
        ImGui::TableNextColumn();
        tableSize.x=ImGui::GetContentRegionAvail().x;
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,oldCellPadding);
        drawFileList(tableSize,acknowledged);
        ImGui::PopStyleVar();
        ImGui::EndTable();
      }
      ImGui::PopStyleVar();

    } else {
      // file list
      drawFileList(tableSize,acknowledged);
    }

    // file name input
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Name: ");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x*0.68f);
    if (ImGui::InputText("##EntryName",&entryName)) {
      for (FileEntry* j: chosenEntries) {
        j->isSelected=false;
      }
      chosenEntries.clear();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::BeginCombo("##FilterType",filterOptions[curFilterType].c_str())) {
      for (size_t i=0; i<filterOptions.size(); i+=2) {
        if (ImGui::Selectable(filterOptions[i].c_str(),curFilterType==i)) {
          curFilterType=i;
          scheduledSort=1;
        }
      }
      ImGui::EndCombo();
    }

    // OK/Cancel buttons
    ImGui::BeginDisabled(entryName.empty() && chosenEntries.empty());
    if (ImGui::Button("OK")) {
      // accept entry
      acknowledged=true;
    }
    ImGui::EndDisabled();
    if (!noClose) {
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        curStatus=FP_STATUS_CLOSED;
        isOpen=false;
      }
    }
    ImGui::SameLine();
    if (!haveFiles) {
      ImGui::Text("Loading...");
    } else if (!haveStat) {
      ImGui::Text("Loading... (stat)");
    }

    if (acknowledged) {
      if (!chosenEntries.empty()) {
        if (chosenEntries.size()==1 && chosenEntries[0]->isDir) {
          if (path.empty()) {
            newDir=chosenEntries[0]->name;
          } else {
            // go there unless we've been required to select a directory
            if (*path.rbegin()==DIR_SEPARATOR) {
              newDir=path+chosenEntries[0]->name;
            } else {
              newDir=path+DIR_SEPARATOR+chosenEntries[0]->name;
            }
          }
        } else {
          // select this entry
          finalSelection.clear();
          for (FileEntry* i: chosenEntries) {
            if (path.empty()) {
              finalSelection.push_back(i->name);
            } else if (*path.rbegin()==DIR_SEPARATOR) {
              finalSelection.push_back(path+i->name);
            } else {
              finalSelection.push_back(path+DIR_SEPARATOR_STR+i->name);
            }
          }

          // if we ought to confirm overwrite, stop and do so
          if (confirmOverwrite) {
            ImGui::OpenPopup("Warning##ConfirmOverwrite");
            logV("confirm overwrite");
          } else {
            curStatus=FP_STATUS_ACCEPTED;
            if (noClose) {
              for (FileEntry* j: chosenEntries) {
                j->isSelected=false;
              }
              chosenEntries.clear();
              updateEntryName();
            } else {
              isOpen=false;
            }
          }
        }
      } else {
        // return the user-provided entry
        finalSelection.clear();
        if (!entryName.empty()) {
          String dirCheckPath;
          if (isPathAbsolute(entryName)) {
            dirCheckPath=entryName;
          } else {
            if (path.empty()) {
              dirCheckPath=entryName;
            } else if (*path.rbegin()==DIR_SEPARATOR) {
              dirCheckPath=path+entryName;
            } else {
              dirCheckPath=path+DIR_SEPARATOR_STR+entryName;
            }
          }

          // check whether this is a directory
          bool isDir=false;
#ifdef _WIN32
          WString dirCheckPathW=utf8To16(dirCheckPath);
          isDir=PathIsDirectoryW(dirCheckPathW.c_str());
#else
          // again, silly but works.
          DIR* checkDir=opendir(dirCheckPath.c_str());
          int dirError=0;
          if (checkDir!=NULL) {
            isDir=true;
            closedir(checkDir);
          } else {
            dirError=errno;
          }
#endif

          if (isDir) {
            // go to directory
            newDir=dirCheckPath;
          } else {
            bool extCheck=false;
            if (confirmOverwrite) {
              // check whether the file may exist with an extension
              std::vector<String> parsedExtensions;
              String nextType;
              for (char i: filterOptions[curFilterType+1]) {
                switch (i) {
                  case '*': // ignore
                    break;
                  case ' ': // separator
                    if (!nextType.empty()) {
                      parsedExtensions.push_back(nextType);
                      nextType="";
                    }
                    break;
                  default: // push
                    nextType.push_back(i);
                    break;
                }
              }
              if (!nextType.empty()) {
                parsedExtensions.push_back(nextType);
                nextType="";
              }
              for (String& i: parsedExtensions) {
                String fileWithExt=dirCheckPath+i;
                logV("testing %s",fileWithExt);
                if (fileExists(fileWithExt.c_str())) {
                  extCheck=true;
                  break;
                }
              }
            }

            // return now unless we gotta confirm overwrite
            if (confirmOverwrite && (dirError==ENOTDIR || extCheck)) {
              ImGui::OpenPopup("Warning##ConfirmOverwrite");
              logV("confirm overwrite");
            } else {
              finalSelection.push_back(dirCheckPath);
              curStatus=FP_STATUS_ACCEPTED;
              if (noClose) {
                for (FileEntry* j: chosenEntries) {
                  j->isSelected=false;
                }
                chosenEntries.clear();
                updateEntryName();
              } else {
                isOpen=false;
              }
            }
          }
        }
      }
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),ImGuiCond_Always,ImVec2(0.5,0.5));
    if (ImGui::BeginPopupModal("Warning##ConfirmOverwrite",NULL,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoSavedSettings)) {
      ImGui::Text("The file you selected already exists! Would you like to overwrite it?");
      if (ImGui::Button("Yes")) {
        curStatus=FP_STATUS_ACCEPTED;
        if (noClose) {
          for (FileEntry* j: chosenEntries) {
            j->isSelected=false;
          }
          chosenEntries.clear();
          updateEntryName();
        } else {
          isOpen=false;
        }
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("No") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    } else if (isModal && !noClose && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      curStatus=FP_STATUS_CLOSED;
      isOpen=false;
    }
  }
  if (!isEmbed) {
    if (isModal && began) {
      if (!isOpen) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    } else {
      ImGui::End();
    }
  }
  

  hasSizeConstraints=false;

  if (!newDir.empty() || readDrives) {
    // change directory
    if (clearSearchOnDirChange) filter="";
    isSearch=wantSearch;
    readDirectory(newDir);
  }
  return (curStatus!=FP_STATUS_WAITING);
}

bool FurnaceFilePicker::isOpened() {
  return isOpen;
}

bool FurnaceFilePicker::open(String name, String pa, String hint, int flags, const std::vector<String>& filter) {
  if (isOpen) return false;
  if (filter.size()&1) {
    logE("invalid filter data! it should be an even-sized vector with even elements containing names and odd ones being the filters.");
    return false;
  }

  isModal=(flags&FP_FLAGS_MODAL);
  noClose=(flags&FP_FLAGS_NO_CLOSE);
  confirmOverwrite=(flags&FP_FLAGS_SAVE);
  multiSelect=(flags&FP_FLAGS_MULTI_SELECT);
  dirSelect=(flags&FP_FLAGS_DIR_SELECT);
  isEmbed=(flags&FP_FLAGS_EMBEDDABLE);

  filterOptions=filter;

  if (filterOptions.size()<2) {
    filterOptions.push_back("all files");
    filterOptions.push_back("*");
  }
  curFilterType=0;

  if (isSearch) this->filter="";
  isSearch=false;
  readDirectory(pa);
  windowName=name;
  hint=entryNameHint;
  isOpen=true;
  return true;
}

void FurnaceFilePicker::close() {
  isOpen=false;
}

void FurnaceFilePicker::setMobile(bool val) {
  isMobile=val;
}

FilePickerStatus FurnaceFilePicker::getStatus() {
  FilePickerStatus retStatus=curStatus;
  curStatus=FP_STATUS_WAITING;
  return retStatus;
}

void FurnaceFilePicker::loadSettings(DivConfig& conf) {
  showBookmarks=conf.getBool(configPrefix+"showBookmarks",true);
  showHiddenFiles=conf.getBool(configPrefix+"showHiddenFiles",true);
  singleClickSelect=conf.getBool(configPrefix+"singleClickSelect",false);
  clearSearchOnDirChange=conf.getBool(configPrefix+"clearSearchOnDirChange",false);
  sortDirsFirst=conf.getBool(configPrefix+"sortDirsFirst",true);
  displayType=conf.getBool(configPrefix+"displayType",true);
  displaySize=conf.getBool(configPrefix+"displaySize",true);
  displayDate=conf.getBool(configPrefix+"displayDate",true);
  bookmarks=conf.getStringList(configPrefix+"bookmarks",{});
}

void FurnaceFilePicker::saveSettings(DivConfig& conf) {
  conf.set(configPrefix+"showBookmarks",showBookmarks);
  conf.set(configPrefix+"showHiddenFiles",showHiddenFiles);
  conf.set(configPrefix+"singleClickSelect",singleClickSelect);
  conf.set(configPrefix+"clearSearchOnDirChange",clearSearchOnDirChange);
  conf.set(configPrefix+"sortDirsFirst",sortDirsFirst);
  conf.set(configPrefix+"displayType",displayType);
  conf.set(configPrefix+"displaySize",displaySize);
  conf.set(configPrefix+"displayDate",displayDate);
  conf.set(configPrefix+"bookmarks",bookmarks);
}

void FurnaceFilePicker::setConfigPrefix(String prefix) {
  configPrefix=prefix;
}

const String& FurnaceFilePicker::getPath() {
  return path;
}

const String& FurnaceFilePicker::getEntryName() {
  return entryName;
}

const std::vector<String>& FurnaceFilePicker::getSelected() {
  return finalSelection;
}

void FurnaceFilePicker::setTypeStyle(FileType type, ImVec4 color, String icon) {
  // "##File" is appended here for performance.
  defaultTypeStyle[type].icon=icon+"##File";
  defaultTypeStyle[type].color=color;
}

void FurnaceFilePicker::registerType(String ext, ImVec4 color, String icon) {
  FileTypeStyle t;
  t.ext=ext;
  // "##File" is appended here for performance.
  t.icon=icon+"##File";
  t.color=color;
  fileTypeRegistry.push_back(t);
}

void FurnaceFilePicker::clearTypes() {
  fileTypeRegistry.clear();
}

FurnaceFilePicker::FurnaceFilePicker():
  fileThread(NULL),
  haveFiles(false),
  haveStat(false),
  stopReading(false),
  isOpen(false),
  isMobile(false),
  multiSelect(false),
  confirmOverwrite(false),
  dirSelect(false),
  noClose(false),
  isModal(false),
  isEmbed(false),
  hasSizeConstraints(false),
  isPathBookmarked(false),
  isSearch(false),
  scheduledSort(0),
  curFilterType(0),
  sortMode(FP_SORT_NAME),
  curStatus(FP_STATUS_WAITING),
  editingPath(false),
  showBookmarks(true),
  showHiddenFiles(true),
  singleClickSelect(false),
  clearSearchOnDirChange(false),
  sortDirsFirst(true),
  displayType(true),
  displaySize(true),
  displayDate(true) {
  memset(sortInvert,0,FP_SORT_MAX*sizeof(bool));
  sortInvert[FP_SORT_SIZE]=true;
  sortInvert[FP_SORT_DATE]=true;
  for (int i=0; i<FP_TYPE_MAX; i++) {
    // "##File" is appended here for performance.
    defaultTypeStyle[i].icon=ICON_FA_QUESTION "##File";
  }
}
