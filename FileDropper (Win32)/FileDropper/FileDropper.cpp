/*

 MIT License

 Copyright © 2020 Samuel Venable

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in all
 copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 SOFTWARE.

*/

#include "FileDropper.h"

#include <windows.h>

#include <algorithm>
#include <sstream>

#include <cwchar>

#include <string>
#include <vector>

#ifndef WM_COPYGLOBALDATA
#define WM_COPYGLOBALDATA 0x0049
#endif

#ifndef MSGFLT_ADD
#define MSGFLT_ADD 1
#endif

using std::basic_string;

using std::wstring;
using std::string;

using std::vector;
using std::size_t;

EXTERN_C IMAGE_DOS_HEADER __ImageBase;
#pragma warning(disable: 4047)
HINSTANCE hInstance = (HINSTANCE)&__ImageBase;
#pragma warning(default: 4047)

static HHOOK hook = NULL;
static WNDPROC oldProc = NULL;
static HWND window_handle = NULL;
static bool file_dnd_enabled = false;
static HDROP hDrop = NULL;
static string fname;

static string def_pattern;
static bool def_allowfiles = true;
static bool def_allowdirs = true;
static bool def_allowmulti = true;

static wstring widen(string str) {
  size_t wchar_count = str.size() + 1;
  vector<wchar_t> buf(wchar_count);
  return wstring{ buf.data(), (size_t)MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, buf.data(), (int)wchar_count) };
}

static string shorten(wstring str) {
  int nbytes = WideCharToMultiByte(CP_UTF8, 0, str.c_str(), (int)str.length(), NULL, 0, NULL, NULL);
  vector<char> buf(nbytes);
  return string{ buf.data(), (size_t)WideCharToMultiByte(CP_UTF8, 0, str.c_str(), (int)str.length(), buf.data(), nbytes, NULL, NULL) };
}

static bool file_exists(string fname) {
  DWORD file_attr;
  wstring tstr_fname = widen(fname);
  file_attr = GetFileAttributesW(tstr_fname.c_str());
  if (file_attr != INVALID_FILE_ATTRIBUTES &&
    !(file_attr & FILE_ATTRIBUTE_DIRECTORY))
    return true;

  return false;
}

static bool directory_exists(string dname) {
  DWORD file_attr;
  wstring tstr_dname = widen(dname);
  file_attr = GetFileAttributesW(tstr_dname.c_str());
  if (file_attr != INVALID_FILE_ATTRIBUTES &&
    (file_attr & FILE_ATTRIBUTE_DIRECTORY))
    return true;

  return false;
}

static string filename_name(string fname) {
  size_t fp = fname.find_last_of("/\\");
  return fname.substr(fp + 1);
}

static string filename_ext(string fname){
  fname = filename_name(fname);
  size_t fp = fname.find_last_of(".");
  if (fp == string::npos)
    return "";
  return fname.substr(fp);
}

static string string_replace_all(string str, string substr, string newstr) {
  size_t pos = 0;
  const size_t sublen = substr.length(), newlen = newstr.length();
  while ((pos = str.find(substr, pos)) != string::npos) {
    str.replace(pos, sublen, newstr);
    pos += newlen;
  }
  return str;
}

static std::vector<string> string_split(const string &str, char delimiter) {
  std::vector<string> vec;
  std::stringstream sstr(str);
  string tmp;
  while (std::getline(sstr, tmp, delimiter))
    vec.push_back(tmp);
  return vec;
}

static void UnInstallHook(HHOOK Hook) {
  UnhookWindowsHookEx(Hook);
}

static LRESULT CALLBACK HookWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  LRESULT rc = CallWindowProc(oldProc, hWnd, uMsg, wParam, lParam);
  if (uMsg == WM_DROPFILES) {
    if (!def_allowmulti) fname = "";
    hDrop = (HDROP)wParam;
    UINT nNumOfFiles = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, NULL);
    if (nNumOfFiles > 0) {
      for (UINT i = 0; i < nNumOfFiles; i += 1) {
        UINT nBufSize = DragQueryFileW(hDrop, i, NULL, 0) + 1;
        wchar_t *fName = new wchar_t[nBufSize];
        DragQueryFileW(hDrop, i, fName, nBufSize);
        if (fname != "") fname += "\n";
        fname += shorten(fName);
        delete[] fName;
      }
    }
    std::vector<string> nameVec = string_split(fname, '\n');
    sort(nameVec.begin(), nameVec.end());
    nameVec.erase(unique(nameVec.begin(), nameVec.end()), nameVec.end());
    fname = "";
    std::vector<string>::size_type sz = nameVec.size();
    for (std::vector<string>::size_type i = 0; i < sz; i += 1) {
      if (fname != "") fname += "\n";
      fname += nameVec[i];
    }
    DragFinish(hDrop); DWORD dwProcessId;
    GetWindowThreadProcessId(window_handle, &dwProcessId);
    AllowSetForegroundWindow(dwProcessId);
    SetForegroundWindow(window_handle);
  }
  return rc;
}

static LRESULT CALLBACK SetHook(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode == HC_ACTION) {
    CWPSTRUCT *pwp = (CWPSTRUCT *)lParam;
    if (pwp->message == WM_KILLFOCUS) {
      oldProc = (WNDPROC)SetWindowLongPtrW(window_handle, GWLP_WNDPROC, (LONG_PTR)HookWndProc);
      UnInstallHook(hook);
    }
  }
  return CallNextHookEx(hook, nCode, wParam, lParam);
}

static HHOOK InstallHook() {
  // Windows Vista
  #if (_WIN32_WINNT == _WIN32_WINNT_VISTA)
  ChangeWindowMessageFilter(WM_DROPFILES, MSGFLT_ADD);
  ChangeWindowMessageFilter(WM_COPYDATA, MSGFLT_ADD);
  ChangeWindowMessageFilter(WM_COPYGLOBALDATA, MSGFLT_ADD);
  #endif

  // Windows 7 and newer
  #if (_WIN32_WINNT > _WIN32_WINNT_VISTA)
  ChangeWindowMessageFilterEx(window_handle, WM_DROPFILES, MSGFLT_ADD, NULL);
  ChangeWindowMessageFilterEx(window_handle, WM_COPYDATA, MSGFLT_ADD, NULL);
  ChangeWindowMessageFilterEx(window_handle, WM_COPYGLOBALDATA, MSGFLT_ADD, NULL);
  #endif

  hook = SetWindowsHookExW(WH_CALLWNDPROC, (HOOKPROC)SetHook, NULL, GetWindowThreadProcessId(window_handle, NULL));
  return hook;
}

static void file_dnd_apply_filter(string pattern, bool allowfiles, bool allowdirs, bool allowmulti) {
  if (pattern == "") { pattern = "."; }
  pattern = string_replace_all(pattern, " ", "");
  pattern = string_replace_all(pattern, "*", "");
  std::vector<string> extVec = string_split(pattern, ';');
  std::vector<string> nameVec = string_split(fname, '\n');
  std::vector<string>::size_type sz1 = nameVec.size();
  std::vector<string>::size_type sz2 = extVec.size();
  fname = "";
  for (std::vector<string>::size_type i2 = 0; i2 < sz2; i2 += 1) {
    for (std::vector<string>::size_type i1 = 0; i1 < sz1; i1 += 1) {
      if (extVec[i2] == "." || extVec[i2] == filename_ext(nameVec[i1])) {
      if (fname != "") fname += "\n";
        fname += nameVec[i1];
      }
    }
  }
  nameVec = string_split(fname, '\n');
  sz1 = nameVec.size();
  fname = "";
  if (allowmulti) {
    for (std::vector<string>::size_type i = 0; i < sz1; i += 1) {
      if (allowfiles && file_exists(nameVec[i])) {
        if (fname != "") fname += "\n";
        fname += nameVec[i];
      } else if (allowdirs && directory_exists(nameVec[i])) {
        if (fname != "") fname += "\n";
        fname += nameVec[i];
      }
    }
  } else {
    if (!nameVec.empty()) {
      if (allowfiles && file_exists(nameVec[0])) {
        if (fname != "") fname += "\n";
        fname += nameVec[0];
      } else if (allowdirs && directory_exists(nameVec[0])) {
        if (fname != "") fname += "\n";
        fname += nameVec[0];
      }
    }
  }
}

double file_dnd_get_enabled() {
  return file_dnd_enabled;
}

double file_dnd_set_enabled(double enable) {
  if (window_handle != NULL) {
    file_dnd_enabled = (bool)enable;
    DragAcceptFiles(window_handle, file_dnd_enabled);
    if (file_dnd_enabled && hook == NULL) 
    InstallHook(); else fname = "";
  }
  return 0;
}

char *file_dnd_get_files() {
  if (fname != "")
    file_dnd_apply_filter(def_pattern, def_allowfiles, def_allowdirs, def_allowmulti);
  while (fname.back() == '\n')
    fname.pop_back();
  return (char *)fname.c_str();
}

double file_dnd_set_files(char *pattern, double allowfiles, double allowdirs, double allowmulti) {
  def_pattern = pattern;
  def_allowfiles = allowfiles;
  def_allowdirs = allowdirs;
  def_allowmulti = allowmulti;
  return 0;
}

void *file_dnd_get_hwnd() {
  return (void *)window_handle;
}

double file_dnd_set_hwnd(void *hwnd) {
  window_handle = (HWND)hwnd;
  return 0;
}
