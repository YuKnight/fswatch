/*
 * Copyright (c) 2014-2015 Enrico M. Crisostomo
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifdef HAVE_CONFIG_H
#  include "libfswatch_config.h"
#endif

#ifdef HAVE_WINDOWS

#  include "gettext_defs.h"
#  include "windows_monitor.hpp"
#  include "libfswatch_map.hpp"
#  include "libfswatch_set.hpp"
#  include "libfswatch_exception.hpp"
#  include "../c/libfswatch_log.h"
#  include "path_utils.hpp"
#  include <set>
#  include <iostream>
#  include <memory>
#  include <sys/types.h>
#  include <cstdlib>
#  include <cstring>
#  include <ctime>
#  include <cstdio>
#  include <cmath>
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/cygwin.h>
#  include <windows.h>

using namespace std;

namespace fsw
{
  REGISTER_MONITOR_IMPL(windows_monitor, windows_monitor_type);

  class WinErrorMessage
  {
  public:
    static WinErrorMessage current()
    {
      WinErrorMessage current;
      return std::move(current);
    }

    WinErrorMessage(DWORD errCode) : errCode{errCode}{}
    WinErrorMessage() : errCode{GetLastError()}{}

    wstring get_message() const
    {
      if (initialized) return msg;
      initialized = true;

      LPWSTR pTemp = nullptr;
      DWORD retSize = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                     NULL,
                                     errCode,
                                     0,
                                     (LPWSTR)&pTemp,
                                     0,
                                     nullptr);

      if (retSize > 0)
      {
        msg = pTemp;
        LocalFree(pTemp);
      }
      else
      {
        msg = L"The system error message could not be formatted.";
      }

      return msg;
    }

    operator wstring() const { return get_message(); }

  private:
    mutable bool initialized = false;
    mutable wstring msg;
    DWORD errCode;
  };

  class CHandle
  {
  public:
    static bool is_valid(const HANDLE & handle)
    {
      return (handle != INVALID_HANDLE_VALUE && handle != nullptr);
    }

    CHandle() : h(INVALID_HANDLE_VALUE){}
    CHandle(HANDLE handle) : h(handle){}

    ~CHandle()
    {
      if (is_valid())
      {
        libfsw_logv(_("CHandle::~Chandle(): Closing handle: %d.\n"), h);
        CloseHandle(h);
      }
    }

    operator HANDLE() const { return h; }

    bool is_valid() const
    {
      return CHandle::is_valid(h);
    }

    CHandle(const CHandle&) = delete;
    CHandle& operator=(const CHandle&) = delete;

    CHandle& operator=(const HANDLE& handle)
    {
      if (is_valid()) ::CloseHandle(h);

      h = handle;

      return *this;
    }

    CHandle(CHandle&& other) noexcept
    {
      h = other.h;
      other.h = INVALID_HANDLE_VALUE;
    }

    CHandle& operator=(CHandle&& other) noexcept
    {
      if (this == &other) return *this;

      if (is_valid()) ::CloseHandle(h);

      h = other.h;
      other.h = INVALID_HANDLE_VALUE;

      return *this;
    }

  private:
    HANDLE h;
  };

  typedef struct DirectoryChangeEvent
  {
    CHandle handle;
    size_t buffer_size;
    DWORD bytes_returned;
    unique_ptr<void, decltype(::free)*> buffer = {nullptr, ::free};
    unique_ptr<OVERLAPPED, decltype(::free)*> overlapped = {static_cast<OVERLAPPED *>(::malloc(sizeof(OVERLAPPED))), ::free};

    DirectoryChangeEvent(size_t buffer_length = 16) : handle{INVALID_HANDLE_VALUE},
                                                      buffer_size{sizeof(FILE_NOTIFY_INFORMATION) * buffer_length},
                                                      bytes_returned{}
    {
      buffer.reset(::malloc(buffer_size));
      if (buffer.get() == nullptr) throw libfsw_exception(_("::malloc failed."));
      if (overlapped.get() == nullptr) throw libfsw_exception(_("::malloc failed."));
    }
  } DirectoryChangeEvent;

  struct windows_monitor_load
  {
    fsw_hash_set<wstring> win_paths;
    fsw_hash_map<wstring, DirectoryChangeEvent> dce_by_path;
    fsw_hash_map<wstring, CHandle> event_by_path;
  };

  struct WindowsFlagType
  {
    DWORD action;
    vector<fsw_event_flag> types;
  };

  static vector<WindowsFlagType> create_flag_type_vector()
  {
    vector<WindowsFlagType> flags;
    flags.push_back({FILE_ACTION_ADDED,            {fsw_event_flag::Created}});
    flags.push_back({FILE_ACTION_REMOVED,          {fsw_event_flag::Removed}});
    flags.push_back({FILE_ACTION_MODIFIED,         {fsw_event_flag::Updated}});
    flags.push_back({FILE_ACTION_RENAMED_OLD_NAME, {fsw_event_flag::MovedFrom, fsw_event_flag::Renamed}});
    flags.push_back({FILE_ACTION_RENAMED_NEW_NAME, {fsw_event_flag::MovedTo, fsw_event_flag::Renamed}});

    return flags;
  }

  static const vector<WindowsFlagType> event_flag_type = create_flag_type_vector();

  static vector<fsw_event_flag> decode_flags(DWORD flag)
  {
    set<fsw_event_flag> evt_flags_set;

    for (const WindowsFlagType & event_type : event_flag_type)
    {
      if (flag == event_type.action)
      {
        for (const auto & type : event_type.types) evt_flags_set.insert(type);
      }
    }

    return vector<fsw_event_flag>(evt_flags_set.begin(), evt_flags_set.end());
  }

  windows_monitor::windows_monitor(vector<string> paths_to_monitor,
                                   FSW_EVENT_CALLBACK * callback,
                                   void * context) :
    monitor(paths_to_monitor, callback, context), load(new windows_monitor_load())
  {
  }

  windows_monitor::~windows_monitor()
  {
    delete load;
  }

  void windows_monitor::initialize_windows_path_list()
  {
    for (const auto & path : paths)
    {
      void * raw_path = ::cygwin_create_path(CCP_POSIX_TO_WIN_W, path.c_str());
      if (raw_path == nullptr) throw libfsw_exception(_("cygwin_create_path could not allocate memory."));

      load->win_paths.insert(wstring(static_cast<wchar_t *>(raw_path)));

      ::free(raw_path);
    }
  }

  static bool read_directory_changes(DirectoryChangeEvent & dce)
  {
    libfsw_logv(_("read_directory_changes: %p\n"), &dce);

    return ReadDirectoryChangesW((HANDLE)dce.handle,
                                 dce.buffer.get(),
                                 dce.buffer_size,
                                 TRUE,
                                 FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                 FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_LAST_ACCESS | FILE_NOTIFY_CHANGE_CREATION,
                                 &dce.bytes_returned,
                                 dce.overlapped.get(),
                                 nullptr);
  }

  void windows_monitor::initialize_events()
  {
    for (const wstring & path : load->win_paths)
    {
      libfsw_logv(_("initialize_events: creating event for %S\n"), path.c_str());

      HANDLE hEvent = ::CreateEvent(nullptr,
                                    TRUE,
                                    FALSE,
                                    nullptr);

      if (hEvent == NULL) throw libfsw_exception(_("CreateEvent failed."));

      libfsw_logv(_("initialize_events: event %d created for %S\n"), hEvent, path.c_str());

      load->event_by_path.emplace(path, hEvent);
    }
  }

  bool windows_monitor::init_search_for_path(const wstring path)
  {
    libfsw_logv(_("init_search_for_path: %S\n"), path.c_str());

    HANDLE h = ::CreateFileW(path.c_str(),
                             GENERIC_READ,
                             FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                             nullptr);

    if (!CHandle::is_valid(h))
    {
      // To do: format error message
      wcerr << L"Invalid handle when opening " << path << endl;
      return false;
    }

    libfsw_logv(_("init_search_for_path: file handle: %d\n"), h);

    DirectoryChangeEvent dce(128);
    dce.handle = h;
    dce.overlapped.get()->hEvent = load->event_by_path[path];

    if (!read_directory_changes(dce))
    {
      // TODO: this error should be logged only in verbose mode.
      wcerr << L"ReadDirectoryChangesW: " << (wstring)WinErrorMessage::current() << endl;
      return false;
    }

    load->dce_by_path[path] = std::move(dce);

    return true;
  }

  void windows_monitor::stop_search_for_path(const wstring path)
  {
    load->dce_by_path.erase(path);
  }

  void windows_monitor::run()
  {
    // Since the file handles are open with FILE_SHARE_DELETE, it may happen
    // that file is deleted when a handle to it is being used.  A call to
    // either ReadDirectoryChangesW or GetOverlappedResult will return with
    // an error if the file system object being observed is deleted.
    // Unfortunately, the error reported by Windows is `Access denied',
    // preventing fswatch to report better messages to the user.

    SetConsoleOutputCP(CP_UTF8);

    initialize_windows_path_list();
    initialize_events();

    while (true)
    {
      ::sleep(latency);

      for (const auto & path : load->win_paths)
      {
        libfsw_logv(_("run: processing %S\n"), path.c_str());

        // If the path is not currently watched, then initialize the search
        // structures.  If the initalization fails, skip the path altogether
        // until the next iteration.
        auto it = load->dce_by_path.find(path);
        if (it == load->dce_by_path.end())
        {
          libfsw_logv(_("run: initializing search structures for %S\n"), path.c_str());
          if (!init_search_for_path(path)) continue;
        }

        it = load->dce_by_path.find(path);
        if (it == load->dce_by_path.end()) throw libfsw_exception(_("Initialization failed."));

        DirectoryChangeEvent & dce = it->second;

        if(!GetOverlappedResult(dce.handle, dce.overlapped.get(), &dce.bytes_returned, FALSE))
        {
          DWORD err = GetLastError();
          if (err == ERROR_IO_INCOMPLETE)
          {
            libfsw_logv(_("run: I/O incomplete.\n"));
            continue;
          }
          else if (err == ERROR_NOTIFY_ENUM_DIR)
          {
            cerr << "Overflow." << endl;
          }

          // TODO: this error should be logged only in verbose mode.
          wcerr << L"GetOverlappedResult: " << (wstring)WinErrorMessage(err) << endl;
          stop_search_for_path(path);
          continue;
        }

        libfsw_logv(_("run: GetOverlappedResult returned %d bytes\n"), dce.bytes_returned);

        if(dce.bytes_returned == 0)
        {
          cerr << _("The current buffer is too small.") << endl;
        }
        else
        {
          char * curr_entry = static_cast<char *>(dce.buffer.get());

          while (curr_entry != nullptr)
          {
            FILE_NOTIFY_INFORMATION * currEntry = reinterpret_cast<FILE_NOTIFY_INFORMATION *>(curr_entry);

            if (currEntry->FileNameLength > 0)
            {
              // The FileName member of the FILE_NOTIFY_INFORMATION structure
              // has the following characteristics:
              //
              //   * It's not NUL terminated.
              //
              //   * Its length is specified in bytes.
              wstring abs_path = path + L"\\" + wstring(currEntry->FileName, currEntry->FileNameLength/sizeof(wchar_t));

              int bs = WideCharToMultiByte(CP_UTF8, 0, abs_path.c_str(), -1, NULL, 0, NULL, NULL);

              if (bs == 0)
              {
                wcerr << L"WideCharToMultiByte: " << (wstring)WinErrorMessage::current() << endl;
                throw libfsw_exception(_("WideCharToMultiByte failed."));
              }

              char u_string[bs];
              WideCharToMultiByte(CP_UTF8, 0, abs_path.c_str(), -1, u_string, bs, NULL, NULL);

              cout << u_string << endl;
            }

            curr_entry = (currEntry->NextEntryOffset == 0) ? nullptr : curr_entry + currEntry->NextEntryOffset;
          }
        }

        if (!ResetEvent(dce.overlapped.get()->hEvent)) throw libfsw_exception(_("::ResetEvent failed."));
        else libfsw_logv(_("run: event %d reset\n"), dce.overlapped.get()->hEvent);

        if (!read_directory_changes(dce))
        {
          // TODO: this error should be logged only in verbose mode.
          wcerr << L"ReadDirectoryChangesW: " << (wstring)WinErrorMessage::current() << endl;
          stop_search_for_path(path);
          continue;
        }
      }
    }
  }
}

#endif  /* HAVE_WINDOWS */
