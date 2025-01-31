#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>

EXTERN_C
_Success_(return == true)
bool reflink(_In_z_ PCWSTR oldpath, _In_z_ PCWSTR newpath);

EXTERN_C
_Success_(return == true)
bool recursive_reflink(_In_z_ PCWSTR old_folder_path, _In_z_ PCWSTR new_folder_path);