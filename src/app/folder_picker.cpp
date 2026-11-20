#include "app/batch.hpp"

#include <windows.h>

#include <shobjidl.h>

#include <filesystem>
#include <string>

namespace coding {
namespace app {

bool pick_folder(const std::wstring &title,
                 const std::filesystem::path &initial,
                 std::filesystem::path &out, std::string &error) {
  out.clear();

  IFileOpenDialog *dialog = nullptr;
  HRESULT hr = ::CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
  if (FAILED(hr) || dialog == nullptr) {
    error = "cannot create the folder picker";
    return false;
  }

  DWORD options = 0;
  if (SUCCEEDED(dialog->GetOptions(&options))) {
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                       FOS_PATHMUSTEXIST);
  }
  dialog->SetTitle(title.c_str());

  if (!initial.empty()) {
    IShellItem *start = nullptr;
    if (SUCCEEDED(::SHCreateItemFromParsingName(initial.c_str(), nullptr,
                                                IID_PPV_ARGS(&start)))) {
      dialog->SetFolder(start);
      start->Release();
    }
  }

  hr = dialog->Show(::GetConsoleWindow());
  if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
    dialog->Release();
    return false;
  }
  if (FAILED(hr)) {
    dialog->Release();
    error = "the folder picker failed";
    return false;
  }

  IShellItem *result = nullptr;
  if (FAILED(dialog->GetResult(&result)) || result == nullptr) {
    dialog->Release();
    error = "no folder was selected";
    return false;
  }

  PWSTR path = nullptr;
  if (FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)) ||
      path == nullptr) {
    result->Release();
    dialog->Release();
    error = "cannot read the selected folder";
    return false;
  }

  out = std::filesystem::path(path);
  ::CoTaskMemFree(path);
  result->Release();
  dialog->Release();
  return true;
}

} // namespace app
} // namespace coding
