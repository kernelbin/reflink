#define WIN32_LEAN_AND_MEAN
#define STRICT_GS_ENABLED
#define _ATL_NO_AUTOMATIC_NAMESPACE
#define _ATL_NO_DEFAULT_LIBS
#define _ATL_NO_WIN_SUPPORT
#define _CRTDBG_MAP_ALLOC
#include <atlbase.h>
#include <windows.h>
#include <winioctl.h>
#include <algorithm>
#include "reflink.h"
#include <crtdbg.h>
#include <pathcch.h>
#include <atlcoll.h>

#pragma comment(lib, "pathcch.lib")
constexpr LONG64 inline ROUNDUP(LONG64 file_size, ULONG cluster_size) noexcept
{
	return (file_size + cluster_size - 1) / cluster_size * cluster_size;
}
static_assert(ROUNDUP(5678, 4 * 1024) == 8 * 1024);
_Success_(return == true)
bool reflink(_In_z_ PCWSTR oldpath, _In_z_ PCWSTR newpath)
{
	ATL::CHandle source(CreateFileW(oldpath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
	if (source == INVALID_HANDLE_VALUE)
	{
		source.Detach();
		return false;
	}

	ULONG fs_flags;
	if (!GetVolumeInformationByHandleW(source, nullptr, 0, nullptr, nullptr, &fs_flags, nullptr, 0))
	{
		return false;
	}
	if (!(fs_flags & FILE_SUPPORTS_BLOCK_REFCOUNTING))
	{
		SetLastError(ERROR_NOT_CAPABLE);
		return false;
	}

	FILE_END_OF_FILE_INFO file_size;
	if (!GetFileSizeEx(source, &file_size.EndOfFile))
	{
		return false;
	}
	FILE_BASIC_INFO file_basic;
	if (!GetFileInformationByHandleEx(source, FileBasicInfo, &file_basic, sizeof file_basic))
	{
		return false;
	}
	ULONG junk;
	FSCTL_GET_INTEGRITY_INFORMATION_BUFFER get_integrity;
	if (!DeviceIoControl(source, FSCTL_GET_INTEGRITY_INFORMATION, nullptr, 0, &get_integrity, sizeof get_integrity, &junk, nullptr))
	{
		return false;
	}

#ifdef _DEBUG
	SetFileAttributesW(newpath, FILE_ATTRIBUTE_NORMAL);
	ATL::CHandle destination(CreateFileW(newpath, GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, CREATE_ALWAYS, 0, source));
#else
	ATL::CHandle destination(CreateFileW(newpath, GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW, 0, source));
#endif
	if (destination == INVALID_HANDLE_VALUE)
	{
		destination.Detach();
		return false;
	}
	FILE_DISPOSITION_INFO dispose = { TRUE };
	if (!SetFileInformationByHandle(destination, FileDispositionInfo, &dispose, sizeof dispose))
	{
		return false;
	}

	if (!DeviceIoControl(destination, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &junk, nullptr))
	{
		return false;
	}
	FSCTL_SET_INTEGRITY_INFORMATION_BUFFER set_integrity = { get_integrity.ChecksumAlgorithm, get_integrity.Reserved, get_integrity.Flags };
	if (!DeviceIoControl(destination, FSCTL_SET_INTEGRITY_INFORMATION, &set_integrity, sizeof set_integrity, nullptr, 0, nullptr, nullptr))
	{
		return false;
	}
	if (!SetFileInformationByHandle(destination, FileEndOfFileInfo, &file_size, sizeof file_size))
	{
		return false;
	}

	const LONG64 split_threshold = (1LL << 32) - get_integrity.ClusterSizeInBytes;

	DUPLICATE_EXTENTS_DATA dup_extent;
	dup_extent.FileHandle = source;
	for (LONG64 offset = 0, remain = ROUNDUP(file_size.EndOfFile.QuadPart, get_integrity.ClusterSizeInBytes); remain > 0; offset += split_threshold, remain -= split_threshold)
	{
		dup_extent.SourceFileOffset.QuadPart = dup_extent.TargetFileOffset.QuadPart = offset;
		dup_extent.ByteCount.QuadPart = (std::min)(split_threshold, remain);
		_ASSERTE(dup_extent.SourceFileOffset.QuadPart % get_integrity.ClusterSizeInBytes == 0);
		_ASSERTE(dup_extent.ByteCount.QuadPart % get_integrity.ClusterSizeInBytes == 0);
		_ASSERTE(dup_extent.ByteCount.QuadPart <= UINT32_MAX);
		_RPT3(_CRT_WARN, "Remain=%llx\nOffset=%llx\nLength=%llx\n\n", remain, dup_extent.SourceFileOffset.QuadPart, dup_extent.ByteCount.QuadPart);
		if (!DeviceIoControl(destination, FSCTL_DUPLICATE_EXTENTS_TO_FILE, &dup_extent, sizeof dup_extent, nullptr, 0, &junk, nullptr))
		{
			_CrtDbgBreak();
			return false;
		}
	}

	if (!(file_basic.FileAttributes & FILE_ATTRIBUTE_SPARSE_FILE))
	{
		FILE_SET_SPARSE_BUFFER set_sparse = { FALSE };
		if (!DeviceIoControl(destination, FSCTL_SET_SPARSE, &set_sparse, sizeof set_sparse, nullptr, 0, &junk, nullptr))
		{
			return false;
		}
	}

	file_basic.CreationTime.QuadPart = 0;
	if (!SetFileInformationByHandle(destination, FileBasicInfo, &file_basic, sizeof file_basic))
	{
		return false;
	}
	if (!FlushFileBuffers(destination))
	{
		return false;
	}
	dispose = { FALSE };
	return !!SetFileInformationByHandle(destination, FileDispositionInfo, &dispose, sizeof dispose);
}


bool reflink_relative_path(_In_z_ PCWSTR old_base, _In_z_ PCWSTR new_base, _In_z_ PCWSTR relative_path)
{
	bool bsuccess = true;
	PWSTR oldpath = nullptr, newpath = nullptr;
	if (FAILED(PathAllocCombine(old_base, relative_path, PATHCCH_ALLOW_LONG_PATHS, &oldpath)))
	{
		bsuccess = false;
		goto leave;
	}
	if (FAILED(PathAllocCombine(new_base, relative_path, PATHCCH_ALLOW_LONG_PATHS, &newpath)))
	{
		bsuccess = false;
		goto leave;
	}
	bsuccess = reflink(oldpath, newpath);
leave:
	if (oldpath) LocalFree(oldpath);
	if (newpath) LocalFree(newpath);
	return bsuccess;
}

// creating dir1\dir2\dir3 will create dir1 and dir2 if they do not already exist.
// TODO: this implement is a little bit memory wasting...
BOOL CreateDirectoryRecursively(_In_z_ LPCWSTR path)
{
	LPWSTR new_path = nullptr;
	BOOL ret = TRUE;

	int buffer_len = (lstrlenW(path) + 1);
	new_path = (LPWSTR)HeapAlloc(GetProcessHeap(), 0, sizeof(WCHAR) * buffer_len);
	if (!new_path)
		return FALSE;
	lstrcpyW(new_path, path);

	PathCchRemoveBackslash(new_path, buffer_len);

	if (PathCchIsRoot(path))
	{
		return TRUE;
	}
	while (!CreateDirectoryW(path, NULL))
	{
		DWORD last_error = GetLastError();
		if (last_error == ERROR_ALREADY_EXISTS)
			break;

		if (last_error != ERROR_PATH_NOT_FOUND)
		{
			ret = FALSE;
			break;
		}

		if (PathCchRemoveFileSpec(new_path, buffer_len) != S_OK)
		{
			ret = FALSE;
			break;
		}

		if (!CreateDirectoryRecursively(new_path))
		{
			ret = FALSE;
			break;
		}
	}
	HeapFree(GetProcessHeap(), 0, new_path);
	return ret;
}

BOOL CreateRelativeDirectoryEx(_In_z_ LPCWSTR base_path, _In_z_ LPCWSTR template_base, _In_z_ LPCWSTR relative_path)
{
	BOOL bSuccess = TRUE;
	LPWSTR template_path = nullptr;
	LPWSTR newdirectory_path = nullptr;

	if (FAILED(PathAllocCombine(base_path, relative_path, PATHCCH_ALLOW_LONG_PATHS, &template_path)))
	{
		bSuccess = FALSE;
		goto leave;
	}
	if (FAILED(PathAllocCombine(template_base, relative_path, PATHCCH_ALLOW_LONG_PATHS, &newdirectory_path)))
	{
		bSuccess = FALSE;
		goto leave;
	}

	bSuccess = CreateDirectoryExW(template_path, newdirectory_path, NULL);
leave:
	if (template_path) LocalFree(template_path);
	if (newdirectory_path) LocalFree(newdirectory_path);
	return bSuccess;
}

_Success_(return != INVALID_HANDLE_VALUE)
HANDLE
WINAPI
FindFirstFileInDirectory(
	_In_ LPCWSTR lpBasePath,
	_In_ LPCWSTR lpRelativePath,
	_In_ FINDEX_INFO_LEVELS fInfoLevelId,
	_Out_writes_bytes_(sizeof(WIN32_FIND_DATAW)) LPVOID lpFindFileData,
	_In_ FINDEX_SEARCH_OPS fSearchOp,
	_Reserved_ LPVOID lpSearchFilter,
	_In_ DWORD dwAdditionalFlags
)
{
	HANDLE Ret = INVALID_HANDLE_VALUE;

	LPWSTR AbsoluteDirPath = nullptr;
	LPWSTR SearchPattern = nullptr;
	if (FAILED(PathAllocCombine(lpBasePath, lpRelativePath, PATHCCH_ALLOW_LONG_PATHS, &AbsoluteDirPath)))
	{
		goto leave;
	}
	if (FAILED(PathAllocCombine(AbsoluteDirPath, L"*", PATHCCH_ALLOW_LONG_PATHS, &SearchPattern)))
	{
		goto leave;
	}

	Ret = FindFirstFileExW(SearchPattern, fInfoLevelId, lpFindFileData, fSearchOp, lpSearchFilter, dwAdditionalFlags);

leave:
	if (AbsoluteDirPath)
	{
		LocalFree(AbsoluteDirPath);
	}
	if (SearchPattern)
	{
		LocalFree(SearchPattern);
	}
	return Ret;
}

// by bfs
EXTERN_C
_Success_(return == true)
bool recursive_reflink(_In_z_ PCWSTR old_folder_path, _In_z_ PCWSTR new_folder_path)
{
	UNREFERENCED_PARAMETER(new_folder_path);
	WIN32_FIND_DATAW find_data;
	ATL::CAtlList<PWSTR> paths_to_search;
	PWSTR search_path = nullptr;
	PWSTR relative_path = nullptr;
	bool bSuccess = true;

	// Ensure parent of new_folder_path exists.
	SIZE_T new_folder_parent_bufsz = wcslen(new_folder_path) + 1;
	LPWSTR new_folder_parent = (LPWSTR)HeapAlloc(GetProcessHeap(), 0, sizeof(WCHAR) * new_folder_parent_bufsz);
	if (!new_folder_parent) goto leave;

	lstrcpyW(new_folder_parent, new_folder_path);
	switch (PathCchRemoveFileSpec(new_folder_parent, new_folder_parent_bufsz))
	{
	case S_OK:
		// TODO: perhaps expand path fully based on cwd? so user can specify path like ".."
		if (new_folder_parent[0] && !CreateDirectoryRecursively(new_folder_parent) && GetLastError() != ERROR_ALREADY_EXISTS)
		{
			goto leave;
		}
		[[fallthrough]];
	case S_FALSE:
		break;
	default:
		// TODO: handle HRESULT error code
		goto leave;
	}

	paths_to_search.AddTail((LPWSTR)LocalAlloc(LMEM_ZEROINIT, 1));

	while (!paths_to_search.IsEmpty())
	{
		search_path = paths_to_search.RemoveHead();

		if (!CreateRelativeDirectoryEx(old_folder_path, new_folder_path, search_path))
		{
			// TODO: log error here?
			continue;
		}
		HANDLE hFind = FindFirstFileInDirectory(old_folder_path, search_path, FindExInfoBasic, (LPVOID)&find_data, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
		if (hFind == INVALID_HANDLE_VALUE)
		{
			bSuccess = false;
			goto leave;
		}

		do
		{
			if (find_data.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)
				continue;
			if (find_data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
				continue; // TODO: this is a temporary workaround
			if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			{
				if (find_data.cFileName[0] == L'.' && (find_data.cFileName[1] == L'\0' || (find_data.cFileName[1] == L'.' && find_data.cFileName[2] == L'\0')))
					continue;
				if (FAILED(PathAllocCombine(search_path, find_data.cFileName, PATHCCH_ALLOW_LONG_PATHS, &relative_path)))
				{
					bSuccess = false;
					goto leave;
				}
				paths_to_search.AddTail(relative_path);
				relative_path = nullptr;
			}
			else
			{
				if (FAILED(PathAllocCombine(search_path, find_data.cFileName, PATHCCH_ALLOW_LONG_PATHS, &relative_path)))
				{
					bSuccess = false;
					goto leave;
				}
				reflink_relative_path(old_folder_path, new_folder_path, relative_path);
				LocalFree(relative_path);
				relative_path = nullptr;
			}
		} while (FindNextFileW(hFind, &find_data));

		FindClose(hFind);

		if (GetLastError() != ERROR_NO_MORE_FILES)
		{
			bSuccess = false;
			goto leave;
		}
		LocalFree(search_path);
		search_path = nullptr;
	}

leave:
	if (search_path)
	{
		LocalFree(search_path);
	}
	if (relative_path)
	{
		LocalFree(relative_path);
	}
	while (!paths_to_search.IsEmpty())
	{
		LocalFree(paths_to_search.RemoveHead());
	}
	if (new_folder_parent)
	{
		HeapFree(GetProcessHeap(), 0, new_folder_parent);
	}
	return bSuccess;
}