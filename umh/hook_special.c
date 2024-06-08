/*
Cuckoo Sandbox - Automated Malware Analysis
Copyright (C) 2010-2015 Cuckoo Sandbox Developers, Optiv, Inc. (brad.spengler@optiv.com

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <initguid.h>
#include <cguid.h>
#include "ntapi.h"
#include "hooking.h"
#include "log.h"
#include "pipe.h"
#include "hook_sleep.h"
#include "misc.h"
#include "config.h"
#include "CAPE\CAPE.h"

extern void DebugOutput(_In_ LPCTSTR lpOutputString, ...);
extern int DoProcessDump(PVOID CallerBase);
extern ULONG_PTR base_of_dll_of_interest;
extern void CreateProcessHandler(LPWSTR lpApplicationName, LPWSTR lpCommandLine, LPPROCESS_INFORMATION lpProcessInformation);
extern void ProcessMessage(DWORD ProcessId, DWORD ThreadId);
extern void set_hooks();
extern void notify_successful_load(void);
extern BOOL ProcessDumped;

static int wmi_sent = 0;
static int bits_sent = 0;
static int tasksched_sent = 0;
static int interop_sent = 0;
static int shell_sent = 0;

HOOKDEF_NOTAIL(WINAPI, LdrLoadDll,
	__in_opt	PWCHAR PathToFile,
	__in_opt	PULONG Flags,
	__in		PUNICODE_STRING ModuleFileName,
	__out	   PHANDLE ModuleHandle
) {

	//
	// In the event that loading this dll results in loading another dll as
	// well, then the unicode string (which is located in the TEB) will be
	// overwritten, therefore we make a copy of it for our own use.
	//
	NTSTATUS ret = 1;
	lasterror_t lasterror;

	COPY_UNICODE_STRING(library, ModuleFileName);

	get_lasterrors(&lasterror);

	if (!wcsncmp(library.Buffer, g_config.dllpath, wcslen(g_config.dllpath))) {
		if (g_config.tlsdump) {
			// lsass injected a second time - switch to 'normal' mode
			g_config.tlsdump = 0;
			if (read_config()) {
				log_init(g_config.debug || g_config.standalone);
				set_hooks();
				notify_successful_load();
			}
		}
		if (g_config.interactive) {
			// explorer injected by malware - switch to 'normal' mode
			g_config.interactive = 2;
			g_config.minhook = 0;
			if (read_config()) {
				set_hooks();
				notify_successful_load();
			}
		}
		// Don't log attempts to load monitor twice
		ret = 0;
	}
	else if (!g_config.tlsdump && !called_by_hook()) {
		if (g_config.file_of_interest && g_config.suspend_logging) {
			wchar_t *absolutename = malloc(32768 * sizeof(wchar_t));
			ensure_absolute_unicode_path(absolutename, library.Buffer);
			if (!wcsicmp(absolutename, g_config.file_of_interest))
				g_config.suspend_logging = FALSE;
			free(absolutename);
		}

		if (library.Buffer[1] != L':') {
			WCHAR newlib[MAX_PATH] = { 0 };
			DWORD concatlen = MIN((DWORD)wcslen(library.Buffer), MAX_PATH - 21);
			wcscpy(newlib, L"c:\\windows\\system32\\");
			wcsncat(newlib, library.Buffer, concatlen);
			if (GetFileAttributesW(newlib) == INVALID_FILE_ATTRIBUTES)
				ret = 0;
		}
	}

	set_lasterrors(&lasterror);

	return ret;
}

HOOKDEF_ALT(NTSTATUS, WINAPI, LdrLoadDll,
	__in_opt	PWCHAR PathToFile,
	__in_opt	PULONG Flags,
	__in		PUNICODE_STRING ModuleFileName,
	__out	   PHANDLE ModuleHandle
) {
	NTSTATUS ret;

	COPY_UNICODE_STRING(library, ModuleFileName);

	hook_info_t saved_hookinfo;

	memcpy(&saved_hookinfo, hook_info(), sizeof(saved_hookinfo));
	ret = Old_LdrLoadDll(PathToFile, Flags, ModuleFileName, ModuleHandle);
	memcpy(hook_info(), &saved_hookinfo, sizeof(saved_hookinfo));

	if (!wcsncmp(library.Buffer, L"\\??\\", 4) || library.Buffer[1] == L':')
		LOQ_ntstatus("system", "HFP", "Flags", Flags, "FileName", library.Buffer,
		"BaseAddress", ModuleHandle);
	else
		LOQ_ntstatus("system", "HoP", "Flags", Flags, "FileName", &library,
		"BaseAddress", ModuleHandle);

	disable_tail_call_optimization();
	return ret;
}

extern void revalidate_all_hooks(void);

HOOKDEF_NOTAIL(WINAPI, LdrUnloadDll,
	PVOID DllImageBase
) {
	if (DllImageBase && DllImageBase == (PVOID)base_of_dll_of_interest && g_config.procdump && !ProcessDumped)
	{
		if (VerifyCodeSection(DllImageBase, g_config.file_of_interest) < 1)
		{
			DebugOutput("Target DLL unloading from 0x%p: code modification detected, dumping.\n", DllImageBase);
			CapeMetaData->DumpType = PROCDUMP;
			if (g_config.import_reconstruction)
				ProcessDumped = DumpImageInCurrentProcessFixImports(DllImageBase, 0);
			else
				ProcessDumped = DumpImageInCurrentProcess(DllImageBase);
		}
		else
		{
			DebugOutput("Target DLL unloading from 0x%p: Skipping dump as code is identical on disk.", DllImageBase);
			ProcessDumped = TRUE;
		}
	}

	return 0;
}

HOOKDEF(BOOL, WINAPI, CreateProcessInternalW,
	__in_opt	LPVOID lpUnknown1,
	__in_opt	LPWSTR lpApplicationName,
	__inout_opt LPWSTR lpCommandLine,
	__in_opt	LPSECURITY_ATTRIBUTES lpProcessAttributes,
	__in_opt	LPSECURITY_ATTRIBUTES lpThreadAttributes,
	__in		BOOL bInheritHandles,
	__in		DWORD dwCreationFlags,
	__in_opt	LPVOID lpEnvironment,
	__in_opt	LPWSTR lpCurrentDirectory,
	__in		LPSTARTUPINFOW lpStartupInfo,
	__out	   LPPROCESS_INFORMATION lpProcessInformation,
	__in_opt	LPVOID lpUnknown2
) {
	BOOL ret;
	hook_info_t saved_hookinfo;

	memcpy(&saved_hookinfo, hook_info(), sizeof(saved_hookinfo));
	ret = Old_CreateProcessInternalW(lpUnknown1, lpApplicationName,
		lpCommandLine, lpProcessAttributes, lpThreadAttributes,
		bInheritHandles, dwCreationFlags | CREATE_SUSPENDED, lpEnvironment,
		lpCurrentDirectory, lpStartupInfo, lpProcessInformation, lpUnknown2);
	memcpy(hook_info(), &saved_hookinfo, sizeof(saved_hookinfo));

	if (ret != FALSE) {
		CreateProcessHandler(lpApplicationName, lpCommandLine, lpProcessInformation);
		ProcessMessage(lpProcessInformation->dwProcessId, lpProcessInformation->dwThreadId);

		// if the CREATE_SUSPENDED flag was not set, then we have to resume the main thread ourself
		if ((dwCreationFlags & CREATE_SUSPENDED) == 0) {
			ResumeThread(lpProcessInformation->hThread);
		}

		disable_sleep_skip();
	}

	if (dwCreationFlags & EXTENDED_STARTUPINFO_PRESENT && lpStartupInfo->cb == sizeof(STARTUPINFOEXW)) {
		HANDLE ParentHandle = (HANDLE)-1;
		unsigned int i;
		LPSTARTUPINFOEXW lpExtStartupInfo = (LPSTARTUPINFOEXW)lpStartupInfo;
		if (lpExtStartupInfo->lpAttributeList) {
			for (i = 0; i < lpExtStartupInfo->lpAttributeList->Count; i++)
				if (lpExtStartupInfo->lpAttributeList->Entries[i].Attribute == PROC_THREAD_ATTRIBUTE_PARENT_PROCESS)
					ParentHandle = *(HANDLE *)lpExtStartupInfo->lpAttributeList->Entries[i].lpValue;
		}
		LOQ_bool("process", "uuhiippps", "ApplicationName", lpApplicationName,
			"CommandLine", lpCommandLine, "CreationFlags", dwCreationFlags,
			"ProcessId", lpProcessInformation->dwProcessId,
			"ThreadId", lpProcessInformation->dwThreadId,
			"ParentHandle", ParentHandle,
			"ProcessHandle", lpProcessInformation->hProcess,
			"ThreadHandle", lpProcessInformation->hThread, "StackPivoted", is_stack_pivoted() ? "yes" : "no");
	}
	else {
		LOQ_bool("process", "uuhiipps", "ApplicationName", lpApplicationName,
			"CommandLine", lpCommandLine, "CreationFlags", dwCreationFlags,
			"ProcessId", lpProcessInformation->dwProcessId,
			"ThreadId", lpProcessInformation->dwThreadId,
			"ProcessHandle", lpProcessInformation->hProcess,
			"ThreadHandle", lpProcessInformation->hThread, "StackPivoted", is_stack_pivoted() ? "yes" : "no");
	}

	return ret;
}

static _CoTaskMemFree pCoTaskMemFree;
static _ProgIDFromCLSID pProgIDFromCLSID;

/* 4991D34B-80A1-4291-83B6-3328366B9097 */ DEFINE_GUID(CLSID_BITSControlClass_v1_0, 0x4991D34B, 0x80A1, 0x4291, 0x83, 0xB6, 0x33, 0x28, 0x36, 0x6B, 0x90, 0x97);
/* 5CE34C0D-0DC9-4C1F-897C-100000000003 */ DEFINE_GUID(CLSID_BITS_Unknown, 0x5CE34C0D, 0x0DC9, 0x4C1F, 0x89, 0x7C, 0x10, 0x00, 0x00, 0x00, 0x00, 0x03); // Is this GUID correct?
/* 69AD4AEE-51BE-439B-A92C-86AE490E8B30 */ DEFINE_GUID(CLSID_BITS_LegacyControlClass, 0x69AD4AEE, 0x51BE, 0x439B, 0xA9, 0x2C, 0x86, 0xAE, 0x49, 0x0E, 0x8B, 0x30);
/* 37668D37-507E-4160-9316-26306D150B12 */ DEFINE_GUID(CLSID_BITS_IBackgroundCopyJob, 0x37668D37,0x507E, 0x4160, 0x93, 0x16, 0x26, 0x30, 0x6D, 0x15, 0x0B, 0x12);
/* 5CE34C0D-0DC9-4C1F-897C-DAA1B78CEE7C */ DEFINE_GUID(CLSID_BITS_IBackgroundCopyManager, 0x5CE34C0D, 0x0DC9, 0x4C1F, 0x89, 0x7C, 0xDA, 0xA1, 0xB7, 0x8C, 0xEE, 0x7C);
/* 01B7BD23-FB88-4A77-8490-5891D3E4653A */ DEFINE_GUID(CLSID_BITS_IBackgroundCopyFile, 0x01B7BD23, 0xFB88, 0x4A77, 0x84, 0x90, 0x58, 0x91, 0xD3, 0xE4, 0x65, 0x3A);

/* 0F87369F-A4E5-4CFC-BD3E-73E6154572DD */ DEFINE_GUID(CLSID_TaskScheduler, 0x0F87369F, 0xA4E5, 0x4CFC, 0xBD, 0x3E, 0x73, 0xE6, 0x15, 0x45, 0x72, 0xDD);
/* 0F87369F-A4E5-4CFC-BD3E-5529CE8784B0 */ DEFINE_GUID(CLSID_TaskScheduler_Unknown, 0x0F87369F, 0xA4E5, 0x4CFC, 0xBD, 0x3E, 0x55, 0x29, 0xCE, 0x87, 0x84, 0xB0);
/* 148BD52A-A2AB-11CE-B11F-00AA00530503 */ DEFINE_GUID(CLSID_SchedulingAgentServiceClass, 0x148BD52A, 0xA2AB, 0x11CE, 0xB1, 0x1F, 0x00, 0xAA, 0x00, 0x53, 0x05, 0x03);

/* 4590F811-1D3A-11D0-891F-00AA004B2E24 */ DEFINE_GUID(CLSID_WbemLocator, 0x4590F811, 0x1D3A, 0x11D0, 0x89, 0x1F, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24);
/* 4590F812-1D3A-11D0-891F-00AA004B2E24 */ DEFINE_GUID(CLSID_WbemClassObject, 0x4590F812, 0x1D3A, 0x11D0, 0x89, 0x1F, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24); // Unmarshaler CLSID
/* 172BDDF8-CEEA-11D1-8B05-00600806D9B6 */ DEFINE_GUID(CLSID_winmgmts, 0x172BDDF8, 0xCEEA, 0x11D1, 0x8B, 0x05, 0x00, 0x60, 0x08, 0x06, 0xD9, 0xB6);
/* CF4CC405-E2C5-4DDD-B3CE-5E7582D8C9FA */ DEFINE_GUID(CLSID_WbemDefaultPathParser, 0xCF4CC405, 0xE2C5, 0x4DDD, 0xB3, 0xCE, 0x5E, 0x75, 0x82, 0xD8, 0xC9, 0xFA);

/* 000209FF-0000-0000-C000-000000000046 */ DEFINE_OLEGUID(CLSID_WordObjectLibrary, 0x000209FF, 0, 0);
/* 00024500-0000-0000-C000-000000000046 */ DEFINE_OLEGUID(CLSID_ExcelObjectLibrary, 0x00024500, 0, 0);
/* 000246FF-0000-0000-C000-000000000046 */ DEFINE_OLEGUID(CLSID_Unknown246FF, 0x000246FF, 0, 0); // ?
/* 0006F03A-0000-0000-C000-000000000046 */ DEFINE_OLEGUID(CLSID_OutlookObjectLibrary, 0x0006F03A, 0, 0);
/* 0002CE02-0000-0000-C000-000000000046 */ DEFINE_OLEGUID(CLSID_Equation2, 0x0002CE02, 0, 0);
/* 0002DF01-0000-0000-C000-000000000046 */ DEFINE_OLEGUID(CLSID_InternetExplorer, 0x0002DF01, 0, 0);
/* 000C101C-0000-0000-C000-000000000046 */ DEFINE_OLEGUID(CLSID_MsiInstallServer, 0x000C101C, 0, 0);
/* 00000323-0000-0000-C000-000000000046 */ DEFINE_OLEGUID(CLSID_StdGlobalInterfaceTable, 0x00000323, 0, 0);
/* 0000032a-0000-0000-C000-000000000046 */ DEFINE_OLEGUID(CLSID_RpcHelper, 0x0000032a, 0, 0);
/* 00000339-0000-0000-C000-000000000046 */ DEFINE_OLEGUID(CLSID_ActivationProperties, 0x00000339, 0, 0);
/* 00000346-0000-0000-C000-000000000046 */ DEFINE_OLEGUID(CLSID_COMCatalog, 0x00000346, 0, 0);
/* 91493441-5A91-11CF-8700-00AA0060263B */ DEFINE_GUID(CLSID_PowerPointObjectLibrary, 0x91493441, 0x5A91, 0x11CF, 0x87, 0x00, 0x00, 0xAA, 0x00, 0x60, 0x26, 0x3B);
/* 75DFF2B7-6936-4C06-A8BB-676A7B00B24B */ DEFINE_GUID(CLSID_SeparateMultipleProcessExplorerHost, 0x75DFF2B7, 0x6936, 0x4C06, 0xA8, 0xBB, 0x67, 0x6A, 0x7B, 0x00, 0xB2, 0x4B);
/* C08AFD90-F2A1-11D1-8455-00A0C91F3880 */ DEFINE_GUID(CLSID_ShellBrowserWindow, 0xC08AFD90, 0xF2A1, 0x11D1, 0x84, 0x55, 0x00, 0xA0, 0xC9, 0x1F, 0x38, 0x80);
/* 9BA05972-F6A8-11CF-A442-00A0C90A8F39 */ DEFINE_GUID(CLSID_ShellWindows, 0x9BA05972, 0xF6A8, 0x11CF, 0xA4, 0x42, 0x00, 0xA0, 0xC9, 0x0A, 0x8F, 0x39);

void inspect_clsid(REFCLSID rclsid) {
	if (!bits_sent && IsEqualCLSID(rclsid, &CLSID_BITSControlClass_v1_0) || IsEqualCLSID(rclsid, &CLSID_BITS_Unknown) || IsEqualCLSID(rclsid, &CLSID_BITS_LegacyControlClass)) {
		bits_sent = 1;
		pipe("BITS:");
	}
	if (!tasksched_sent && IsEqualCLSID(rclsid, &CLSID_TaskScheduler) || IsEqualCLSID(rclsid, &CLSID_TaskScheduler_Unknown) || IsEqualCLSID(rclsid, &CLSID_SchedulingAgentServiceClass)) {
		tasksched_sent = 1;
		pipe("TASKSCHED:");
	}
	if (!wmi_sent && IsEqualCLSID(rclsid, &CLSID_WbemLocator) || IsEqualCLSID(rclsid, &CLSID_WbemClassObject) || IsEqualCLSID(rclsid, &CLSID_winmgmts)
		|| IsEqualCLSID(rclsid, &CLSID_WbemDefaultPathParser)) {
		wmi_sent = 1;
		pipe("WMI:");
	}
	if (!interop_sent && IsEqualCLSID(rclsid, &CLSID_WordObjectLibrary) || IsEqualCLSID(rclsid, &CLSID_ExcelObjectLibrary) || IsEqualCLSID(rclsid, &CLSID_Unknown246FF)
		|| IsEqualCLSID(rclsid, &CLSID_OutlookObjectLibrary) || IsEqualCLSID(rclsid, &CLSID_Equation2) || IsEqualCLSID(rclsid, &CLSID_InternetExplorer)
		|| IsEqualCLSID(rclsid, &CLSID_MsiInstallServer) || IsEqualCLSID(rclsid, &CLSID_StdGlobalInterfaceTable) || IsEqualCLSID(rclsid, &CLSID_PowerPointObjectLibrary)
		|| IsEqualCLSID(rclsid, &CLSID_SeparateMultipleProcessExplorerHost) || IsEqualCLSID(rclsid, &CLSID_ShellBrowserWindow)) {
		interop_sent = 1;
		pipe("INTEROP:");
	}
	if (!shell_sent && IsEqualCLSID(rclsid, &CLSID_ShellWindows)) {
		shell_sent = 1;
		pipe("SHELL:");
	}
}

HOOKDEF(HRESULT, WINAPI, CoCreateInstance,
	__in	REFCLSID rclsid,
	__in	LPUNKNOWN pUnkOuter,
	__in	DWORD dwClsContext,
	__in	REFIID riid,
	__out	LPVOID *ppv
) {
	IID id1 = CLSID_NULL;
	IID id2 = CLSID_NULL;
	char idbuf1[40];
	char idbuf2[40];
	lasterror_t lasterror;
	HRESULT ret;
	hook_info_t saved_hookinfo;
	OLECHAR *resolv = NULL;

	get_lasterrors(&lasterror);

	if (!pCoTaskMemFree)
		pCoTaskMemFree = (_CoTaskMemFree)GetProcAddress(GetModuleHandleA("ole32"), "CoTaskMemFree");
	if (!pProgIDFromCLSID)
		pProgIDFromCLSID = (_ProgIDFromCLSID)GetProcAddress(GetModuleHandleA("ole32"), "ProgIDFromCLSID");

	if (is_valid_address_range((ULONG_PTR)rclsid, 16))
			memcpy(&id1, rclsid, sizeof(id1));
		if (is_valid_address_range((ULONG_PTR)riid, 16))
			memcpy(&id2, riid, sizeof(id2));
	sprintf(idbuf1, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", id1.Data1, id1.Data2, id1.Data3,
		id1.Data4[0], id1.Data4[1], id1.Data4[2], id1.Data4[3], id1.Data4[4], id1.Data4[5], id1.Data4[6], id1.Data4[7]);
	sprintf(idbuf2, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", id2.Data1, id2.Data2, id2.Data3,
		id2.Data4[0], id2.Data4[1], id2.Data4[2], id2.Data4[3], id2.Data4[4], id2.Data4[5], id2.Data4[6], id2.Data4[7]);

	if (!called_by_hook()) {
		inspect_clsid(&id1);
	}

	disable_sleep_skip();

	set_lasterrors(&lasterror);

	memcpy(&saved_hookinfo, hook_info(), sizeof(saved_hookinfo));
	ret = Old_CoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv);
	memcpy(hook_info(), &saved_hookinfo, sizeof(saved_hookinfo));

	get_lasterrors(&lasterror);

	pProgIDFromCLSID(&id1, &resolv);

	LOQ_hresult("com", "shsu", "rclsid", idbuf1, "ClsContext", dwClsContext, "riid", idbuf2, "ProgID", resolv);

	if (resolv)
		pCoTaskMemFree(resolv);

	set_lasterrors(&lasterror);

	return ret;
}

HOOKDEF(HRESULT, WINAPI, CoCreateInstanceEx,
	__in	REFCLSID rclsid,
	__in	LPUNKNOWN pUnkOuter,
	__in	DWORD dwClsContext,
	_In_	COSERVERINFO *pServerInfo,
	_In_	DWORD		dwCount,
	_Inout_ MULTI_QI	 *pResults
	) {
	IID id1 = CLSID_NULL;
	char idbuf1[40];
	lasterror_t lasterror;
	HRESULT ret;
	hook_info_t saved_hookinfo;
	OLECHAR *resolv = NULL;

	get_lasterrors(&lasterror);

	if (!pCoTaskMemFree)
		pCoTaskMemFree = (_CoTaskMemFree)GetProcAddress(GetModuleHandleA("ole32"), "CoTaskMemFree");
	if (!pProgIDFromCLSID)
		pProgIDFromCLSID = (_ProgIDFromCLSID)GetProcAddress(GetModuleHandleA("ole32"), "ProgIDFromCLSID");

	if (is_valid_address_range((ULONG_PTR)rclsid, 16))
			memcpy(&id1, rclsid, sizeof(id1));
	sprintf(idbuf1, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", id1.Data1, id1.Data2, id1.Data3,
		id1.Data4[0], id1.Data4[1], id1.Data4[2], id1.Data4[3], id1.Data4[4], id1.Data4[5], id1.Data4[6], id1.Data4[7]);

	if (!called_by_hook()) {
		inspect_clsid(&id1);
	}

	disable_sleep_skip();

	set_lasterrors(&lasterror);

	memcpy(&saved_hookinfo, hook_info(), sizeof(saved_hookinfo));
	ret = Old_CoCreateInstanceEx(rclsid, pUnkOuter, dwClsContext, pServerInfo, dwCount, pResults);
	memcpy(hook_info(), &saved_hookinfo, sizeof(saved_hookinfo));


	if (!called_by_hook()) {
		get_lasterrors(&lasterror);
		pProgIDFromCLSID(&id1, &resolv);

		LOQ_hresult("com", "shuu", "rclsid", idbuf1, "ClsContext", dwClsContext, "ServerName", pServerInfo ? pServerInfo->pwszName : NULL, "ProgID", resolv);

		if (resolv)
			pCoTaskMemFree(resolv);
		set_lasterrors(&lasterror);
	}

	return ret;
}

HOOKDEF(HRESULT, WINAPI, CoGetClassObject,
	_In_	 REFCLSID	 rclsid,
	_In_	 DWORD		dwClsContext,
	_In_opt_ COSERVERINFO *pServerInfo,
	_In_	 REFIID	   riid,
	_Out_	LPVOID	   *ppv
) {
	HRESULT ret;
	lasterror_t lasterror;
	IID id1 = CLSID_NULL;
	IID id2 = CLSID_NULL;
	char idbuf1[40];
	char idbuf2[40];
	hook_info_t saved_hookinfo;
	OLECHAR *resolv = NULL;

	get_lasterrors(&lasterror);

	if (!pCoTaskMemFree)
		pCoTaskMemFree = (_CoTaskMemFree)GetProcAddress(GetModuleHandleA("ole32"), "CoTaskMemFree");
	if (!pProgIDFromCLSID)
		pProgIDFromCLSID = (_ProgIDFromCLSID)GetProcAddress(GetModuleHandleA("ole32"), "ProgIDFromCLSID");

	if (is_valid_address_range((ULONG_PTR)rclsid, 16))
			memcpy(&id1, rclsid, sizeof(id1));
		if (is_valid_address_range((ULONG_PTR)riid, 16))
		memcpy(&id2, riid, sizeof(id2));
	sprintf(idbuf1, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", id1.Data1, id1.Data2, id1.Data3,
		id1.Data4[0], id1.Data4[1], id1.Data4[2], id1.Data4[3], id1.Data4[4], id1.Data4[5], id1.Data4[6], id1.Data4[7]);
	sprintf(idbuf2, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", id2.Data1, id2.Data2, id2.Data3,
		id2.Data4[0], id2.Data4[1], id2.Data4[2], id2.Data4[3], id2.Data4[4], id2.Data4[5], id2.Data4[6], id2.Data4[7]);

	set_lasterrors(&lasterror);

	memcpy(&saved_hookinfo, hook_info(), sizeof(saved_hookinfo));
	ret = Old_CoGetClassObject(rclsid, dwClsContext, pServerInfo, riid, ppv);
	memcpy(hook_info(), &saved_hookinfo, sizeof(saved_hookinfo));

	get_lasterrors(&lasterror);

	pProgIDFromCLSID(&id1, &resolv);

	LOQ_hresult("com", "shsu", "rclsid", idbuf1, "ClsContext", dwClsContext, "riid", idbuf2, "ProgID", resolv);

	if (resolv)
		pCoTaskMemFree(resolv);

	set_lasterrors(&lasterror);

	return ret;
}

HOOKDEF_NOTAIL(WINAPI, JsEval,
	PVOID Arg1,
	PVOID Arg2,
	PVOID Arg3,
	int Index,
	DWORD *scriptobj
) {
#ifndef _WIN64
	PWCHAR jsbuf;
	PUCHAR p;
#endif
	int ret = 0;

	/* TODO: 64-bit support*/
#ifdef _WIN64
	return ret;
#else

	p = (PUCHAR)scriptobj[4 * Index - 2];
	jsbuf = *(PWCHAR *)(p + 8);
	if (jsbuf)
		LOQ_ntstatus("browser", "u", "Javascript", jsbuf);

	return ret;
#endif
}

HOOKDEF(int, WINAPI, COleScript_ParseScriptText,
	PVOID Arg1,
	PWCHAR ScriptBuf,
	PVOID Arg3,
	PVOID Arg4,
	PVOID Arg5,
	PVOID Arg6,
	PVOID Arg7,
	PVOID Arg8,
	PVOID Arg9,
	PVOID Arg10
) {
	int ret = Old_COleScript_ParseScriptText(Arg1, ScriptBuf, Arg3, Arg4, Arg5, Arg6, Arg7, Arg8, Arg9, Arg10);
	LOQ_ntstatus("browser", "u", "Script", ScriptBuf);
	return ret;
}

HOOKDEF(PVOID, WINAPI, JsParseScript,
	const wchar_t *script,
	PVOID SourceContext,
	const wchar_t *sourceUrl,
	PVOID *result
) {
	PVOID ret = Old_JsParseScript(script, SourceContext, sourceUrl, result);

	LOQ_zero("browser", "uu", "Script", script, "Source", sourceUrl);

	return ret;
}

HOOKDEF_NOTAIL(WINAPI, JsRunScript,
	const wchar_t *script,
	PVOID SourceContext,
	const wchar_t *sourceUrl,
	PVOID *result
) {
	int ret = 0;

	LOQ_zero("browser", "uu", "Script", script, "Source", sourceUrl);
	return ret;
}

// based on code by Stephan Chenette and Moti Joseph of Websense, Inc. released under the GPLv3
// http://securitylabs.websense.com/content/Blogs/3198.aspx

HOOKDEF(int, WINAPI, CDocument_write,
	PVOID this,
	SAFEARRAY *psa
) {
	DWORD i;
	PWCHAR buf;
	int ret = Old_CDocument_write(this, psa);
	VARIANT *pvars = (VARIANT *)psa->pvData;
	unsigned int buflen = 0;
	unsigned int offset = 0;
	for (i = 0; i < psa->rgsabound[0].cElements; i++) {
		if (pvars[i].vt == VT_BSTR)
			buflen += (unsigned int)wcslen((const wchar_t *)pvars[i].pbstrVal) + 8;
	}
	buf = calloc(1, (buflen + 1) * sizeof(wchar_t));
	if (buf == NULL)
		return ret;

	for (i = 0; i < psa->rgsabound[0].cElements; i++) {
		if (pvars[i].vt == VT_BSTR) {
			wcscpy(buf + offset, (const wchar_t *)pvars[i].pbstrVal);
			offset += (unsigned int)wcslen((const wchar_t *)pvars[i].pbstrVal);
			wcscpy(buf + offset, L"\r\n||||\r\n");
			offset += 8;
		}
	}

	LOQ_ntstatus("browser", "u", "Buffer", buf);

	return ret;
}

HOOKDEF(HRESULT, WINAPI, IsValidURL,
	_In_       LPBC    pBC,
	_In_       LPCWSTR szURL,
	_Reserved_ DWORD   dwReserved
)
{
	HRESULT ret = Old_IsValidURL(pBC, szURL, dwReserved);
	LOQ_hresult("network", "u", "URL", szURL);
	return ret;
}