/*
Cuckoo Sandbox - Automated Malware Analysis
Copyright (C) 2010-2015 Cuckoo Sandbox Developers, Optiv, Inc. (brad.spengler@optiv.com)

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
#include "ntapi.h"
#include "config.h"
#include "misc.h"
#include "log.h"
#include "hooking.h"
#include "hook_sleep.h"
#include "Shlwapi.h"
#include "CAPE\CAPE.h"

#define SINGLE_STEP_LIMIT 0x4000  // default unless specified in web ui
#define DROPPED_LIMIT 100

#define BP_EXEC		0x00
#define BP_WRITE	   0x01
#define BP_RESERVED	0x02
#define BP_READWRITE   0x03
#define DoClearZeroFlag 1
#define DoSetZeroFlag   2
#define PrintEAX		3

extern void DebugOutput(_In_ LPCTSTR lpOutputString, ...);
extern char *our_dll_path;
extern char *our_process_name;
extern int path_is_system(const wchar_t *path_w);
extern int path_is_program_files(const wchar_t *path_w);
extern BOOL PatchByte(LPVOID Address, BYTE Byte);
extern wchar_t *our_process_path_w;
extern int EntryPointRegister;
extern unsigned int TraceDepthLimit, StepLimit, Type0, Type1, Type2;
extern char Action0[MAX_PATH], Action1[MAX_PATH], Action2[MAX_PATH], Action3[MAX_PATH];
extern char *Instruction0, *Instruction1, *Instruction2, *Instruction3;
extern char *procname0;
extern char DumpSizeString[MAX_PATH];
extern BOOL ImageBaseRemapped;
extern DWORD ExportAddress;
extern SIZE_T DumpSize;

void parse_config_line(char* line)
{
	unsigned int i;
	unsigned int vallen;

	// split key=value
	char *p = strchr(line, '=');
	if (p != NULL) {
		const char *key = line;
		char *value = p + 1;
		if (value[0] == '$')
			return;
		*p = 0;
		vallen = (unsigned int)strlen(value);
		if (!strcmp(key, "pipe")) {
			for (i = 0; i < vallen; i++)
				g_config.pipe_name[i] = (wchar_t)(unsigned short)value[i];
		}
		else if (!strcmp(key, "logserver")) {
			strncpy(g_config.logserver, value,
				ARRAYSIZE(g_config.logserver));
		}
		else if (!strcmp(key, "results")) {
			memset(g_config.results, 0, MAX_PATH);
			strncpy(g_config.results, value, ARRAYSIZE(g_config.results) - 1);
			for (i = 0; i < ARRAYSIZE(g_config.results); i++)
				g_config.w_results[i] = (wchar_t)(unsigned short)g_config.results[i];
		}
		else if (!strcmp(key, "pythonpath")) {
			strncpy(g_config.pythonpath, value,
				ARRAYSIZE(g_config.pythonpath) - 1);
			for (i = 0; i < ARRAYSIZE(g_config.pythonpath); i++)
				g_config.w_pythonpath[i] = (wchar_t)(unsigned short)g_config.pythonpath[i];
			DebugOutput("Python path set to '%ws'.\n", g_config.w_pythonpath);
		}
		else if (!strcmp(key, "file-of-interest")) {
			unsigned int len = (unsigned int)strlen(value);
			if (len > 1) {
				if (value[1] == ':') {
					// is a file
					char *tmp = calloc(1, MAX_PATH);
					ensure_absolute_ascii_path(tmp, value);
					g_config.file_of_interest = ascii_to_unicode_dup(tmp);
					free(tmp);
				}
				else
					// is a URL
					g_config.url_of_interest = ascii_to_unicode_dup(value);
			}
		}
		else if (!strcmp(key, "referrer")) { //Enable/Disable use of a fake referrer when performing URL analyses
			g_config.w_referrer = ascii_to_unicode_dup(value);
			g_config.referrer = strdup(value);
		}
		else if (!strcmp(key, "analyzer")) {
			strncpy(g_config.analyzer, value, ARRAYSIZE(g_config.analyzer)-1);
			for (i = 0; i < ARRAYSIZE(g_config.analyzer); i++)
				g_config.w_analyzer[i] = (wchar_t)(unsigned short)g_config.analyzer[i];
			wcscpy(g_config.dllpath, g_config.w_analyzer);
			if (wcslen(g_config.dllpath) < ARRAYSIZE(g_config.dllpath) - 5)
				wcscat(g_config.dllpath, L"\\dll\\");
		}
		else if (!strcmp(key, "shutdown-mutex")) {
			strncpy(g_config.shutdown_mutex, value, ARRAYSIZE(g_config.shutdown_mutex));
		}
		else if (!strcmp(key, "first-process")) {
			g_config.first_process = value[0] == '1';
		}
		else if (!strcmp(key, "startup-time")) {
			g_config.startup_time = atoi(value);
		}
		else if (!strcmp(key, "debug")) { // Set to 1 to enable reporting of critical exceptions occurring during analysis, set to 2 to enable reporting of all exceptions.
			g_config.debug = atoi(value);
		}
		else if (!strcmp(key, "hook-type")) { //Valid for 32-bit analyses only. Specifies the hook type to use: direct, indirect, or safe. Safe attempts a Detours-style hook.
#ifndef _WIN64
			if (!strcmp(value, "direct"))
				g_config.hook_type = HOOK_JMP_DIRECT;
			else if (!strcmp(value, "indirect"))
				g_config.hook_type = HOOK_JMP_INDIRECT;
			else if (!strcmp(value, "safe"))
				g_config.hook_type = HOOK_SAFEST;
#endif
		}
		else if (!strcmp(key, "disable_hook_content")) { //Set to 1 to remove functionality of all hooks except those critical for monitoring other processes. Set to 2 to apply to all hooks.
			g_config.disable_hook_content = atoi(value);
		}
		/*
		else if (!strcmp(key, "host-ip")) {
			g_config.host_ip = inet_addr(value);
		}
		else if (!strcmp(key, "host-port")) {
			g_config.host_port = atoi(value);
		}
		*/
		else if (!strcmp(key, "force-sleepskip")) { // Override default sleep skipping behavior: 0 disables all sleep skipping, 1 skips all sleeps.
			g_config.force_sleepskip = value[0] == '1';
		}
		else if (!strcmp(key, "serial")) { //Spoof the serial of the system volume as the provided hex value
			g_config.serial_number = (unsigned int)strtoul(value, NULL, 16);
		}
		else if (!strcmp(key, "sysvol_ctimelow")) {
			g_config.sysvol_ctime.dwLowDateTime = (unsigned int)strtoul(value, NULL, 16);
		}
		else if (!strcmp(key, "sysvol_ctimehigh")) {
			g_config.sysvol_ctime.dwHighDateTime = (unsigned int)strtoul(value, NULL, 16);
		}
		else if (!strcmp(key, "sys32_ctimelow")) {
			g_config.sys32_ctime.dwLowDateTime = (unsigned int)strtoul(value, NULL, 16);
		}
		else if (!strcmp(key, "sys32_ctimehigh")) {
			g_config.sys32_ctime.dwHighDateTime = (unsigned int)strtoul(value, NULL, 16);
		}
		else if (!strcmp(key, "full-logs")) { //By default, logs prior to network activity for URL analyses and prior to access of the file in question for non-executable formats are suppressed. Set to 1 to disable log suppression.
			g_config.full_logs = value[0] == '1';
		}
		else if (!strcmp(key, "force-flush")) { //For performance reasons, logs are buffered before being sent back to the result server. We make every attempt to flush the buffer at critical points including when exceptions occur, but in some rare termination scenarios, logs may be lost. Set to 1 to force flushing of the log buffers after any non-duplicate API is called, set to 2 to force flushing of every log.
			g_config.force_flush = atoi(value);
		}
		else if (!strcmp(key, "terminate-event")) {
			strncpy(g_config.terminate_event_name, value, ARRAYSIZE(g_config.terminate_event_name));
		}
		else if (!strcmp(key, "no-stealth")) { // Set to 1 to disable anti-anti-VM/sandbox code enabled by default.
			g_config.no_stealth = value[0] == '1';
		}
		else if (!strcmp(key, "buffer-max")) {
			buffer_log_max = (unsigned int)strtoul(value, NULL, 10);
		}
		else if (!strcmp(key, "large-buffer-max")) {
			large_buffer_log_max = (unsigned int)strtoul(value, NULL, 10);
		}
		else if (!stricmp(key, "log-exceptions")) {
			g_config.log_exceptions = atoi(value);
			if (g_config.log_exceptions)
				DebugOutput("Exception logging enabled.\n");
			else
				DebugOutput("Exception logging disabled.\n");
		}
		else if (!stricmp(key, "log-breakpoints") || !stricmp(key, "log-bps")) {
			g_config.log_breakpoints = value[0] == '1';
			if (g_config.log_breakpoints)
				DebugOutput("Breakpoint logging to behavior log enabled.\n");
			else
				DebugOutput("Breakpoint logging to behavior log disabled.\n");
		}
		else if (!strcmp(key, "dropped-limit")) { //Override the default dropped file limit of 100 files
			g_config.dropped_limit = (unsigned int)strtoul(value, NULL, 10);
			DebugOutput("Dropped file limit set to %d.\n", g_config.dropped_limit);
		}
		else if (!strcmp(key, "ntdll-protect")) {
			g_config.ntdll_protect = (unsigned int)strtoul(value, NULL, 10);
            if (g_config.ntdll_protect)
                DebugOutput("Config: ntdll write protection enabled.");
            else
                DebugOutput("Config: ntdll write protection disabled.");
		}
		else if (!strcmp(key, "ntdll-remap")) {
			g_config.ntdll_remap = (unsigned int)strtoul(value, NULL, 10);
            if (g_config.ntdll_remap)
                DebugOutput("Config: ntdll remap protection enabled.");
            else
                DebugOutput("Config: ntdll remap protection disabled.");
		}
		else if (!strcmp(key, "standalone")) {
			g_config.standalone = value[0] == '1';
		}
		else if (!strcmp(key, "exclude-apis")) { //Exclude the colon-separated list of APIs from being hooked
			unsigned int x = 0;
			char *p2;
			p = value;
			while (p && x < EXCLUSION_MAX) {
				p2 = strchr(p, ':');
				if (p2) {
					*p2 = '\0';
				}
				g_config.excluded_apinames[x++] = strdup(p);
				if (p2 == NULL)
					break;
				p = p2 + 1;
			}
		}
		else if (!strcmp(key, "exclude-dlls")) { //Exclude the colon-separated list of DLLs from being hooked
			unsigned int x = 0;
			char *p2;
			p = value;
			while (p && x < EXCLUSION_MAX) {
				p2 = strchr(p, ':');
				if (p2) {
					*p2 = '\0';
				}
				g_config.excluded_dllnames[x++] = ascii_to_unicode_dup(p);
				if (p2 == NULL)
					break;
				p = p2 + 1;
			}
		}
		else if (!strcmp(key, "base-on-api")) { //Sets the base address to which breakpoints will be applied (and sets breakpoints)
			unsigned int x = 0;
			char *p2;
			p = value;
			while (p && x < EXCLUSION_MAX) {
				p2 = strchr(p, ':');
				if (p2) {
					*p2 = '\0';
				}
				g_config.base_on_apiname[x++] = strdup(p);
				DebugOutput("Added '%s' to base-on-API list.\n", p);
				if (p2 == NULL)
					break;
				p = p2 + 1;
			}
		}
		else if (!strcmp(key, "dump-on-api")) { //Dump the calling module when a function from the colon-separated list of APIs is used
			unsigned int x = 0;
			char *p2;
			p = value;
			while (p && x < EXCLUSION_MAX) {
				p2 = strchr(p, ':');
				if (p2) {
					*p2 = '\0';
				}
				g_config.dump_on_apinames[x++] = strdup(p);
				DebugOutput("Added '%s' to dump-on-API list.\n", p);
				if (p2 == NULL)
					break;
				p = p2 + 1;
			}
		}
		else if (!strcmp(key, "coverage-modules")) {
			unsigned int x = 0;
			char *p2;
			p = value;
			while (p && x < EXCLUSION_MAX) {
				p2 = strchr(p, ':');
				if (p2) {
					*p2 = '\0';
				}
				g_config.coverage_modules[x++] = ascii_to_unicode_dup(p);
				DebugOutput("Added '%s' to coverage-modules list.\n", p);
				if (p2 == NULL)
					break;
				p = p2 + 1;
			}
		}
		else if (!strcmp(key, "dump-on-api-type")) {
			g_config.dump_on_api_type = (unsigned int)strtoul(value, NULL, 0);
		}
		else if (!strcmp(key, "file-offsets")) { //Breakpoints in bp0-bp3 will be interpreted as PE file offsets rather than RVAs
			g_config.file_offsets = value[0] == '1';
			if (g_config.file_offsets)
				DebugOutput("Breakpoints interpreted as file offsets.\n");
		}
		else if (!stricmp(key, "export")) {
			ExportAddress = strtoul(value, NULL, 0);
			DebugOutput("Config: Export address set to 0x%x", ExportAddress);
		}
		else if (!stricmp(key, "patch")) {
			p = strchr(value, ':');
			if (p) {
				*p = '\0';
				char *p2 = p+1;
				unsigned int byte = strtoul(value, NULL, 0);
				int delta=0;
				p = strchr(p2, '+');
				if (p) {
					delta = strtoul(p+1, NULL, 0);
					DebugOutput("Config: Delta 0x%x.\n", delta);
					*p = '\0';
				}
				else {
					p = strchr(p2, '-');
					if (p) {
						delta = - (int)strtoul(p+1, NULL, 0);
						DebugOutput("Config: Delta 0x%x.\n", delta);
						*p = '\0';
					}
				}
				PVOID address = (PVOID)(DWORD_PTR)strtoul(p2, NULL, 0);
				if (address) {
					DebugOutput("Config: patching address 0x%p with byte 0x%x", address, byte);
					PatchByte(address, (BYTE)byte);
				}
				else
					DebugOutput("Config: patch address missing invalid: %s", value);
			}
			else
				DebugOutput("Config: patch byte missing");
		}
		else if (!stricmp(key, "bp")) {
			unsigned int x = 0;
			char *p2;
			p = value;
			while (p && x < BREAKPOINT_MAX) {
				p2 = strchr(p, ':');
				if (p2) {
					*p2 = '\0';
				}
				int delta=0;
				p2 = strchr(value, '+');
				if (p2) {
					delta = strtoul(p2+1, NULL, 0);
					DebugOutput("Config: Delta 0x%x.\n", delta);
					*p2 = '\0';
				}
				else {
					p2 = strchr(value, '-');
					if (p2) {
						delta = - (int)strtoul(p2+1, NULL, 0);
						DebugOutput("Config: Delta 0x%x.\n", delta);
						*p2 = '\0';
					}
				}
				g_config.bp[x++] = (PVOID)((PUCHAR)(DWORD_PTR)strtoul(p, NULL, 0) + delta);
				if (g_config.bp[x-1]) {
					DebugOutput("Config: Added 0x%p to breakpoint list.\n", g_config.bp[x-1]);
					g_config.debugger = 1;
				}
				if (p2 == NULL)
					break;
				p = p2 + 1;
			}
		}
		else if (!stricmp(key, "action")) {
			unsigned int x = 0;
			char *p2;
			p = value;
			while (p && x < BREAKPOINT_MAX) {
				p2 = strchr(p, '|');
				if (p2) {
					*p2 = '\0';
				}
				g_config.action[x++] = strdup(p);
				DebugOutput("Config: Action %d set to %s.", x-1, g_config.action[x-1]);
				if (p2 == NULL)
					break;
				p = p2 + 1;
			}
		}
		else if (!stricmp(key, "bp0")) {
			p = strchr(value, ':');
			if (p && *(p+1) == ':') {
				g_config.bp0 = 0;
				*p = '\0';
				*(p+1) = '\0';
				HANDLE Module = NULL;
				if (!stricmp(value, "capemon"))
					Module = (HANDLE)g_our_dll_base;
				else
					Module = GetModuleHandle(value);
				g_config.break_on_apiname = strdup(p+2);
				g_config.break_on_modname = strdup(value);
				if (Module)
					g_config.bp0 = GetProcAddress(Module, p+2);
				else
					DebugOutput("Config: Failed to get base for module (%s).\n", g_config.break_on_modname);
				if (g_config.bp0) {
					g_config.debugger = 1;
					g_config.bpva0 = 1;
					DebugOutput("Config: bp0 set to 0x%p (%s::%s).\n", g_config.bp0, g_config.break_on_modname, g_config.break_on_apiname);
				}
				else if (Module) {
					unsigned int delta = strtoul(p+2, NULL, 0);
					if (delta) {
						g_config.bp0 = (PBYTE)Module + delta;
						g_config.debugger = 1;
						g_config.bpva0 = 1;
						DebugOutput("Config: bp0 set to 0x%p (%s::%s).\n", g_config.bp0, g_config.break_on_modname, g_config.break_on_apiname);
					}
					else
						DebugOutput("Config: Failed to get address for function %s::%s\n", g_config.break_on_modname, p+2);
				}
			}
			else if (!_strnicmp(value, "zero", 4)) {
				DebugOutput("Config: bp0 set to zero.\n");
				g_config.zerobp0 = TRUE;
			}
			else if (!_strnicmp(value, "ep", 2) || !_strnicmp(value, "entrypoint", 10)) {
				DebugOutput("Config: bp0 set to entry point.\n", g_config.bp0);
				EntryPointRegister = 1;
				g_config.debugger = 1;
			}
			else {
				int delta=0;
				p = strchr(value, '+');
				if (p) {
					delta = strtoul(p+1, NULL, 0);
					DebugOutput("Config: Delta 0x%x.\n", delta);
					*p = '\0';
				}
				else {
					p = strchr(value, '-');
					if (p) {
						delta = - (int)strtoul(p+1, NULL, 0);
						DebugOutput("Config: Delta 0x%x.\n", delta);
						*p = '\0';
					}
				}
				PVOID bp = (PVOID)(DWORD_PTR)strtoul(value, NULL, 0);
				if (bp != g_config.bp0 && bp != g_config.bp1 && bp != g_config.bp2 && bp != g_config.bp3) {
					g_config.bp0 = bp;
					g_config.debugger = 1;
					if (g_config.bp0 == (PVOID)(DWORD_PTR)ULONG_MAX)
						g_config.bp0 = (PVOID)_strtoui64(value, NULL, 0);
					if (delta) {
						DebugOutput("Config: bp0 was 0x%p.\n", g_config.bp0);
						g_config.bp0 = (PVOID)(DWORD_PTR)((PUCHAR)g_config.bp0 + delta);
					}
					DebugOutput("Config: bp0 set to 0x%p.\n", g_config.bp0);
				}
			}
		}
		else if (!stricmp(key, "bp1")) {
			p = strchr(value, ':');
			if (p && *(p+1) == ':') {
				g_config.bp1 = 0;
				*p = '\0';
				*(p+1) = '\0';
				HANDLE Module = NULL;
				if (!stricmp(value, "capemon"))
					Module = (HANDLE)g_our_dll_base;
				else
					Module = GetModuleHandle(value);
				g_config.break_on_apiname = strdup(p+2);
				g_config.break_on_modname = strdup(value);
				if (Module)
					g_config.bp1 = GetProcAddress(Module, p+2);
				else
					DebugOutput("Config: Failed to get base for module (%s).\n", g_config.break_on_modname);
				if (g_config.bp1) {
					g_config.debugger = 1;
					g_config.bpva1 = 1;
					DebugOutput("Config: bp1 set to 0x%p (%s::%s).\n", g_config.bp1, g_config.break_on_modname, g_config.break_on_apiname);
				}
				else if (Module) {
					unsigned int delta = strtoul(p+2, NULL, 0);
					if (delta) {
						g_config.bp1 = (PBYTE)Module + delta;
						g_config.debugger = 1;
						g_config.bpva1 = 1;
						DebugOutput("Config: bp1 set to 0x%p (%s::%s).\n", g_config.bp1, g_config.break_on_modname, g_config.break_on_apiname);
					}
					else
						DebugOutput("Config: Failed to get address for function %s::%s\n", g_config.break_on_modname, p+2);
				}
			}
			else if (!_strnicmp(value, "zero", 4)) {
				DebugOutput("Config: bp1 set to zero.\n");
				g_config.zerobp1 = TRUE;
			}
			else if (!_strnicmp(value, "ep", 2) || !_strnicmp(value, "entrypoint", 10)) {
				DebugOutput("Config: bp1 set to entry point.\n", g_config.bp1);
				EntryPointRegister = 1;
				g_config.debugger = 1;
			}
			else {
				int delta=0;
				p = strchr(value, '+');
				if (p) {
					delta = strtoul(p+1, NULL, 0);
					DebugOutput("Config: Delta 0x%x.\n", delta);
					*p = '\0';
				}
				else {
					p = strchr(value, '-');
					if (p) {
						delta = - (int)strtoul(p+1, NULL, 0);
						DebugOutput("Config: Delta 0x%x.\n", delta);
						*p = '\0';
					}
				}
				PVOID bp = (PVOID)(DWORD_PTR)strtoul(value, NULL, 0);
				if (bp != g_config.bp0 && bp != g_config.bp1 && bp != g_config.bp2 && bp != g_config.bp3) {
					g_config.bp1 = bp;
					g_config.debugger = 1;
					if (g_config.bp1 == (PVOID)(DWORD_PTR)ULONG_MAX)
						g_config.bp1 = (PVOID)_strtoui64(value, NULL, 0);
					if (delta) {
						DebugOutput("Config: bp1 was 0x%p.\n", g_config.bp1);
						g_config.bp1 = (PVOID)(DWORD_PTR)((PUCHAR)g_config.bp1 + delta);
					}
					DebugOutput("Config: bp1 set to 0x%p.\n", g_config.bp1);
				}
			}
		}
		else if (!stricmp(key, "bp2")) {
			p = strchr(value, ':');
			if (p && *(p+1) == ':') {
				g_config.bp2 = 0;
				*p = '\0';
				*(p+1) = '\0';
				HANDLE Module = NULL;
				if (!stricmp(value, "capemon"))
					Module = (HANDLE)g_our_dll_base;
				else
					Module = GetModuleHandle(value);
				g_config.break_on_apiname = strdup(p+2);
				g_config.break_on_modname = strdup(value);
				if (Module)
					g_config.bp2 = GetProcAddress(Module, p+2);
				else
					DebugOutput("Config: Failed to get base for module (%s).\n", g_config.break_on_modname);
				if (g_config.bp2) {
					g_config.debugger = 1;
					g_config.bpva2 = 1;
					DebugOutput("Config: bp2 set to 0x%p (%s::%s).\n", g_config.bp2, g_config.break_on_modname, g_config.break_on_apiname);
				}
				else if (Module) {
					unsigned int delta = strtoul(p+2, NULL, 0);
					if (delta) {
						g_config.bp2 = (PBYTE)Module + delta;
						g_config.debugger = 1;
						g_config.bpva2 = 1;
						DebugOutput("Config: bp2 set to 0x%p (%s::%s).\n", g_config.bp2, g_config.break_on_modname, g_config.break_on_apiname);
					}
					else
						DebugOutput("Config: Failed to get address for function %s::%s\n", g_config.break_on_modname, p+2);
				}
			}
			else if (!_strnicmp(value, "zero", 4)) {
				DebugOutput("Config: bp2 set to zero.\n");
				g_config.zerobp2 = TRUE;
			}
			else if (!_strnicmp(value, "ep", 2) || !_strnicmp(value, "entrypoint", 10)) {
				DebugOutput("Config: bp2 set to entry point.\n", g_config.bp2);
				EntryPointRegister = 1;
				g_config.debugger = 1;
			}
			else {
				int delta=0;
				p = strchr(value, '+');
				if (p) {
					delta = strtoul(p+1, NULL, 0);
					DebugOutput("Config: Delta 0x%x.\n", delta);
					*p = '\0';
				}
				else {
					p = strchr(value, '-');
					if (p) {
						delta = - (int)strtoul(p+1, NULL, 0);
						DebugOutput("Config: Delta 0x%x.\n", delta);
						*p = '\0';
					}
				}
				PVOID bp = (PVOID)(DWORD_PTR)strtoul(value, NULL, 0);
				if (bp != g_config.bp0 && bp != g_config.bp1 && bp != g_config.bp2 && bp != g_config.bp3) {
					g_config.bp2 = bp;
					g_config.debugger = 1;
					if (g_config.bp2 == (PVOID)(DWORD_PTR)ULONG_MAX)
						g_config.bp2 = (PVOID)_strtoui64(value, NULL, 0);
					if (delta) {
						DebugOutput("Config: bp2 was 0x%p.\n", g_config.bp2);
						g_config.bp2 = (PVOID)(DWORD_PTR)((PUCHAR)g_config.bp2 + delta);
					}
					DebugOutput("Config: bp2 set to 0x%p.\n", g_config.bp2);
				}
			}
		}
		else if (!stricmp(key, "bp3")) {
			p = strchr(value, ':');
			if (p && *(p+1) == ':') {
				g_config.bp3 = 0;
				*p = '\0';
				*(p+1) = '\0';
				HANDLE Module = NULL;
				if (!stricmp(value, "capemon"))
					Module = (HANDLE)g_our_dll_base;
				else
					Module = GetModuleHandle(value);
				g_config.break_on_apiname = strdup(p+2);
				g_config.break_on_modname = strdup(value);
				if (Module)
					g_config.bp3 = GetProcAddress(Module, p+2);
				else
					DebugOutput("Config: Failed to get base for module (%s).\n", g_config.break_on_modname);
				if (g_config.bp3) {
					g_config.debugger = 1;
					g_config.bpva3 = 1;
					DebugOutput("Config: bp3 set to 0x%p (%s::%s).\n", g_config.bp3, g_config.break_on_modname, g_config.break_on_apiname);
				}
				else if (Module) {
					unsigned int delta = strtoul(p+2, NULL, 0);
					if (delta) {
						g_config.bp3= (PBYTE)Module + delta;
						g_config.debugger = 1;
						g_config.bpva3 = 1;
						DebugOutput("Config: bp3 set to 0x%p (%s::%s).\n", g_config.bp3, g_config.break_on_modname, g_config.break_on_apiname);
					}
					else
						DebugOutput("Config: Failed to get address for function %s::%s\n", g_config.break_on_modname, p+2);
				}
			}
			else if (!_strnicmp(value, "zero", 4)) {
				DebugOutput("Config: bp3 set to zero.\n");
				g_config.zerobp3 = TRUE;
			}
			else if (!_strnicmp(value, "ep", 2) || !_strnicmp(value, "entrypoint", 10)) {
				DebugOutput("Config: bp3 set to entry point.\n", g_config.bp3);
				EntryPointRegister = 1;
				g_config.debugger = 1;
			}
			else {
				int delta=0;
				p = strchr(value, '+');
				if (p) {
					delta = strtoul(p+1, NULL, 0);
					DebugOutput("Config: Delta 0x%x.\n", delta);
					*p = '\0';
				}
				else {
					p = strchr(value, '-');
					if (p) {
						delta = - (int)strtoul(p+1, NULL, 0);
						DebugOutput("Config: Delta 0x%x.\n", delta);
						*p = '\0';
					}
				}
				PVOID bp = (PVOID)(DWORD_PTR)strtoul(value, NULL, 0);
				if (bp != g_config.bp0 && bp != g_config.bp1 && bp != g_config.bp2 && bp != g_config.bp3) {
					g_config.bp3 = bp;
					g_config.debugger = 1;
					if (g_config.bp3 == (PVOID)(DWORD_PTR)ULONG_MAX)
						g_config.bp3 = (PVOID)_strtoui64(value, NULL, 0);
					if (delta) {
						DebugOutput("Config: bp3 was 0x%p.\n", g_config.bp3);
						g_config.bp3 = (PVOID)(DWORD_PTR)((PUCHAR)g_config.bp3 + delta);
					}
					DebugOutput("Config: bp3 set to 0x%p.\n", g_config.bp3);
				}
			}
		}
		else if (!stricmp(key, "br0")) {
			p = strchr(value, ':');
			if (p && *(p+1) == ':') {
				g_config.br0 = 0;
				*p = '\0';
				*(p+1) = '\0';
				HANDLE Module = NULL;
				if (!stricmp(value, "capemon"))
					Module = (HANDLE)g_our_dll_base;
				else
					Module = GetModuleHandle(value);
				g_config.break_on_apiname = strdup(p+2);
				g_config.break_on_modname = strdup(value);
				if (Module)
					g_config.br0 = GetProcAddress(Module, p+2);
				else
					DebugOutput("Config: Failed to get base for module (%s).\n", g_config.break_on_modname);
				if (g_config.br0) {
					g_config.debugger = 1;
					g_config.bpva0 = 1;
					DebugOutput("Config: br0 set to 0x%p (%s::%s).\n", g_config.br0, g_config.break_on_modname, g_config.break_on_apiname);
				}
				else if (Module) {
					unsigned int delta = strtoul(p+2, NULL, 0);
					if (delta) {
						g_config.br0 = (PBYTE)Module + delta;
						g_config.debugger = 1;
						g_config.bpva0 = 1;
						DebugOutput("Config: br0 set to 0x%p (%s::%s).\n", g_config.br0, g_config.break_on_modname, g_config.break_on_apiname);
					}
					else
						DebugOutput("Config: Failed to get address for function %s::%s\n", g_config.break_on_modname, p+2);
				}
			}
			else {
				int delta=0;
				p = strchr(value, '+');
				if (p) {
					delta = strtoul(p+1, NULL, 0);
					DebugOutput("Config: Delta 0x%x.\n", delta);
					*p = '\0';
				}
				else {
					p = strchr(value, '-');
					if (p) {
						delta = - (int)strtoul(p+1, NULL, 0);
						DebugOutput("Config: Delta 0x%x.\n", delta);
						*p = '\0';
					}
				}
				g_config.br0 = (PVOID)(DWORD_PTR)strtoul(value, NULL, 0);
				if (g_config.br0) {
					g_config.debugger = 1;
					if (delta) {
						DebugOutput("Config: br0 was 0x%x (delta 0x%x).\n", g_config.br0, delta);
						g_config.br0 = (PVOID)(DWORD_PTR)((PUCHAR)g_config.br0 + delta);
					}
					DebugOutput("Config: br0 set to 0x%x (break-on-return)\n", g_config.br0);
				}
			}
		}
		else if (!stricmp(key, "br1")) {
			int delta=0;
			p = strchr(value, '+');
			if (p) {
				delta = strtoul(p+1, NULL, 0);
				DebugOutput("Config: Delta 0x%x.\n", delta);
				*p = '\0';
			}
			else {
				p = strchr(value, '-');
				if (p) {
					delta = - (int)strtoul(p+1, NULL, 0);
					DebugOutput("Config: Delta 0x%x.\n", delta);
					*p = '\0';
				}
			}
			g_config.br1 = (PVOID)(DWORD_PTR)strtoul(value, NULL, 0);
			if (g_config.br1) {
				g_config.debugger = 1;
				if (delta) {
					DebugOutput("Config: br1 was 0x%x (delta 0x%x).\n", g_config.br1, delta);
					g_config.br1 = (PVOID)(DWORD_PTR)((PUCHAR)g_config.br1 + delta);
				}
				DebugOutput("Config: br1 set to 0x%x (break-on-return)\n", g_config.br1);
			}
		}
		else if (!stricmp(key, "br2")) {
			g_config.br2 = (PVOID)(DWORD_PTR)strtoul(value, NULL, 0);
			if (g_config.br2) {
				g_config.debugger = 1;
				DebugOutput("Config: br2 set to 0x%x (break-on-return)\n", g_config.br2);
			}
		}
		else if (!stricmp(key, "br3")) {
			g_config.br3 = (PVOID)(DWORD_PTR)strtoul(value, NULL, 0);
			if (g_config.br3) {
				g_config.debugger = 1;
				DebugOutput("Config: br3 set to 0x%x (break-on-return)\n", g_config.br3);
			}
		}
		else if (!stricmp(key, "sysbp")) {
			unsigned int x = 0;
			char *p2;
			p = value;
			while (p && x < SYSBP_MAX) {
				p2 = strchr(p, ':');
				if (p2) {
					*p2 = '\0';
				}
				int delta=0;
				p2 = strchr(value, '+');
				if (p2) {
					delta = strtoul(p2+1, NULL, 0);
					DebugOutput("Config: Delta 0x%x.\n", delta);
					*p2 = '\0';
				}
				else {
					p2 = strchr(value, '-');
					if (p2) {
						delta = - (int)strtoul(p2+1, NULL, 0);
						DebugOutput("Config: Delta 0x%x.\n", delta);
						*p2 = '\0';
					}
				}
				for (unsigned int i = 0; i < ARRAYSIZE(g_config.sysbp); i++) {
					if (g_config.sysbp[x])
						x++;
					else
						break;
				}
				if (x < SYSBP_MAX) {
					PVOID address = (PVOID)((PUCHAR)(DWORD_PTR)strtoul(p, NULL, 0) + delta);
					g_config.sysbp[x] = address;
					//DebugOutput("Config: Set syscall breakpoint at 0x%p\n", address);
					g_config.debugger = 1;
				}
				if (p2 == NULL)
					break;
				p = p2 + 1;
			}
		}
		else if (!stricmp(key, "sysbpmode")) {
			g_config.sysbpmode = (unsigned int)strtoul(value, NULL, 10);
			DebugOutput("Syscall breakpoint mode set to %d.\n", g_config.sysbpmode);
		}
		else if (!stricmp(key, "count0")) {
			g_config.count0 = (unsigned int)(DWORD_PTR)strtoul(value, NULL, 0);
			DebugOutput("Config: Count for breakpoint 0 set to %d\n", g_config.count0);
		}
		else if (!stricmp(key, "count1")) {
			g_config.count1 = (unsigned int)(DWORD_PTR)strtoul(value, NULL, 0);
			DebugOutput("Config: Count for breakpoint 1 set to %d\n", g_config.count1);
		}
		else if (!stricmp(key, "count2")) {
			g_config.count2 = (unsigned int)(DWORD_PTR)strtoul(value, NULL, 0);
			DebugOutput("Config: Count for breakpoint 2 set to %d\n", g_config.count2);
		}
		else if (!stricmp(key, "count3")) {
			g_config.count3 = (unsigned int)(DWORD_PTR)strtoul(value, NULL, 0);
			DebugOutput("Config: Count for breakpoint 3 set to %d\n", g_config.count3);
		}
		else if (!stricmp(key, "hc0")) {
			g_config.hc0 = (unsigned int)(DWORD_PTR)strtoul(value, NULL, 0);
			DebugOutput("Config: Hit count for breakpoint 0 set to %d\n", g_config.hc0);
		}
		else if (!stricmp(key, "hc1")) {
			g_config.hc1 = (unsigned int)(DWORD_PTR)strtoul(value, NULL, 0);
			DebugOutput("Config: Hit count for breakpoint 1 set to %d\n", g_config.hc1);
		}
		else if (!stricmp(key, "hc2")) {
			g_config.hc2 = (unsigned int)(DWORD_PTR)strtoul(value, NULL, 0);
			DebugOutput("Config: Hit count for breakpoint 2 set to %d\n", g_config.hc2);
		}
		else if (!stricmp(key, "hc3")) {
			g_config.hc3 = (unsigned int)(DWORD_PTR)strtoul(value, NULL, 0);
			DebugOutput("Config: Hit count for breakpoint 3 set to %d\n", g_config.hc3);
		}
		else if (!stricmp(key, "depth")) {
			if (!_strnicmp(value, "all", 3)) {
				TraceDepthLimit = 0x7FFFFFFF;
				DebugOutput("Config: Trace depth set to all");
			}
			else {
				TraceDepthLimit = (unsigned int)strtoul(value, NULL, 10);
				DebugOutput("Config: Trace depth set to 0x%x", TraceDepthLimit);
			}
		}
		else if (!stricmp(key, "count")) {
			if (!_strnicmp(value, "all", 3)) {
				StepLimit = 0x7FFFFFFF;
				DebugOutput("Config: Trace instruction count set to all");
			}
			else {
				StepLimit = (unsigned int)strtoul(value, NULL, 10);
				DebugOutput("Config: Trace instruction count set to 0x%x", StepLimit);
			}
		}
		else if (!stricmp(key, "step-out")) {
			g_config.debugger = 1;
			g_config.bp0 = (PVOID)(DWORD_PTR)strtoul(value, NULL, 0);
			if (g_config.bp0) {
				g_config.step_out = '1';
				DebugOutput("Config: Step-out breakpoint set to 0x%x.\n", g_config.bp0);
			}
		}
		else if (!stricmp(key, "dumpsize")) {
			DumpSize = (SIZE_T)strtoul(value, NULL, 0);
			if (DumpSize)
				DebugOutput("Config: DumpSize set to 0x%x", DumpSize);
			else {
				strncpy(DumpSizeString, value, strlen(value));
				DebugOutput("Config: DumpSize set to string \"%s\".", DumpSizeString);
			}
		}
		else if (!stricmp(key, "action0")) {
			memset(Action0, 0, MAX_PATH);
			strncpy(Action0, value, strlen(value));
			DebugOutput("Config: Action0 set to %s.", Action0);
		}
		else if (!stricmp(key, "action1")) {
			memset(Action1, 0, MAX_PATH);
			strncpy(Action1, value, strlen(value));
			DebugOutput("Config: Action1 set to %s.", Action1);
		}
		else if (!stricmp(key, "action2")) {
			memset(Action2, 0, MAX_PATH);
			strncpy(Action2, value, strlen(value));
			DebugOutput("Config: Action2 set to %s.", Action2);
		}
		else if (!stricmp(key, "action3")) {
			memset(Action3, 0, MAX_PATH);
			strncpy(Action3, value, strlen(value));
			DebugOutput("Config: Action3 set to %s.", Action3);
		}
		else if (!stricmp(key, "instruction0") || !stricmp(key, "instr0")) {
			Instruction0 = calloc(1, MAX_PATH);
			strncpy(Instruction0, value, strlen(value));
			DebugOutput("Config: Instruction0 set to %s.", value);
		}
		else if (!stricmp(key, "instruction1") || !stricmp(key, "instr1")) {
			Instruction1 = calloc(1, MAX_PATH);
			strncpy(Instruction1, value, strlen(value));
			DebugOutput("Config: Instruction1 set to %s.", value);
		}
		else if (!stricmp(key, "instruction2") || !stricmp(key, "instr2")) {
			Instruction1 = calloc(1, MAX_PATH);
			strncpy(Instruction2, value, strlen(value));
			DebugOutput("Config: Instruction2 set to %s.", value);
		}
		else if (!stricmp(key, "instruction3") || !stricmp(key, "instr3")) {
			Instruction1 = calloc(1, MAX_PATH);
			strncpy(Instruction3, value, strlen(value));
			DebugOutput("Config: Instruction3 set to %s.", value);
		}
		else if (!stricmp(key, "procname0")) {
			procname0 = calloc(1, MAX_PATH);
			strncpy(procname0, value, strlen(value));
			DebugOutput("Config: procname0 set to %s.", value);
		}
		else if (!stricmp(key, "break-on-return")) { //Sets breakpoints on the return address(es) from a colon-separated list of APIs
			g_config.debugger = 1;
			strncpy(g_config.break_on_return, value, ARRAYSIZE(g_config.break_on_return));
			DebugOutput("Config: Break-on-return set to %s.", g_config.break_on_return);
			g_config.break_on_return_set = TRUE;
		}
		else if (!stricmp(key, "trace-all")) {
			g_config.debugger = 1;
			g_config.trace_all = (unsigned int)strtoul(value, NULL, 10);
			if (g_config.trace_all)
				DebugOutput("Config: Trace all enabled.\n");
		}
		else if (!stricmp(key, "trace-into-api")) {
			unsigned int x = 0;
			char *p2;
			p = value;
			while (p && x < EXCLUSION_MAX) {
				p2 = strchr(p, ':');
				if (p2) {
					*p2 = '\0';
				}
				g_config.trace_into_api[x++] = strdup(p);
				DebugOutput("Config: Added '%s' to trace-into-API list.\n", p);
				if (p2 == NULL)
					break;
				p = p2 + 1;
			}
			g_config.debugger = 1;
		}
		else if (!stricmp(key, "dumptype0")) {
			g_config.dumptype0 = (unsigned int)strtoul(value, NULL, 0);
		}
		else if (!stricmp(key, "dumptype1")) {
			g_config.dumptype1 = (unsigned int)strtoul(value, NULL, 0);
		}
		else if (!stricmp(key, "dumptype2")) {
			g_config.dumptype2 = (unsigned int)strtoul(value, NULL, 0);
		}
		else if (!stricmp(key, "dumptype3")) {
			g_config.dumptype3 = (unsigned int)strtoul(value, NULL, 0);
		}
		else if (!stricmp(key, "bpva0")) {
			g_config.bpva0 = value[0] == '1';
		}
		else if (!stricmp(key, "bpva1")) {
			g_config.bpva1 = value[0] == '1';
		}
		else if (!stricmp(key, "bpva2")) {
			g_config.bpva2 = value[0] == '1';
		}
		else if (!stricmp(key, "bpva3")) {
			g_config.bpva3 = value[0] == '1';
		}
		else if (!stricmp(key, "typestring")) {
			memset(g_config.typestring, 0, MAX_PATH);
			strncpy(g_config.typestring, value, strlen(value));
			DebugOutput("Config: typestring set to %s", g_config.typestring);
		}
		else if (!stricmp(key, "typestring0")) {
			memset(g_config.typestring0, 0, MAX_PATH);
			strncpy(g_config.typestring0, value, strlen(value));
			DebugOutput("Config: typestring0 set to %s", g_config.typestring0);
		}
		else if (!stricmp(key, "typestring1")) {
			memset(g_config.typestring1, 0, MAX_PATH);
			strncpy(g_config.typestring1, value, strlen(value));
			DebugOutput("Config: typestring1 set to %s", g_config.typestring1);
		}
		else if (!stricmp(key, "typestring2")) {
			memset(g_config.typestring2, 0, MAX_PATH);
			strncpy(g_config.typestring2, value, strlen(value));
			DebugOutput("Config: typestring2 set to %s", g_config.typestring2);
		}
		else if (!stricmp(key, "typestring3")) {
			memset(g_config.typestring3, 0, MAX_PATH);
			strncpy(g_config.typestring3, value, strlen(value));
			DebugOutput("Config: typestring3 set to %s", g_config.typestring3);
		}
		else if (!stricmp(key, "str")) {
			memset(g_config.str, 0, MAX_PATH);
			strncpy((char*)g_config.str, value, strlen(value));
			DebugOutput("Config: Search string set to %s", g_config.str);
			if (strlen((char*)g_config.str))
				g_config.no_logs = 2;

		}
		else if (!stricmp(key, "type0")) {
			if (!_strnicmp(value, "w", 1)) {
				DebugOutput("Config: Breakpoint 0 type set to write (Type0 = BP_WRITE).\n");
				Type0 = BP_WRITE;
			}
			else if (!_strnicmp(value, "r", 1) || !_strnicmp(value, "rw", 2)) {
				DebugOutput("Config: Breakpoint 0 type set to read/write (Type0 = BP_READWRITE).\n");
				Type0 = BP_READWRITE;
			}
			else if (!_strnicmp(value, "x", 1)) {
				DebugOutput("Config: Breakpoint 0 type set to execute (Type0 = BP_EXEC).\n");
				Type0 = BP_EXEC;
			}
		}
		else if (!stricmp(key, "type1")) {
			if (!_strnicmp(value, "w", 1)) {
				DebugOutput("Config: Breakpoint 1 type set to write (Type1 = BP_WRITE).\n");
				Type1 = BP_WRITE;
			}
			else if (!_strnicmp(value, "r", 1) || !_strnicmp(value, "rw", 2)) {
				DebugOutput("Config: Breakpoint 1 type set to read/write (Type1 = BP_READWRITE).\n");
				Type1 = BP_READWRITE;
			}
			else if (!_strnicmp(value, "x", 1)) {
				DebugOutput("Config: Breakpoint 1 type set to execute (Type1 = BP_EXEC).\n");
				Type1 = BP_EXEC;
			}
		}
		else if (!stricmp(key, "type2")) {
			if (!_strnicmp(value, "w", 1)) {
				DebugOutput("Config: Breakpoint 2 type set to write (Type2 = BP_WRITE).\n");
				Type2 = BP_WRITE;
			}
			else if (!_strnicmp(value, "r", 1) || !_strnicmp(value, "rw", 2)) {
				DebugOutput("Config: Breakpoint 2 type set to read/write (Type2 = BP_READWRITE).\n");
				Type2 = BP_READWRITE;
			}
			else if (!_strnicmp(value, "x", 1)) {
				DebugOutput("Config: Breakpoint 2 type set to execute (Type2 = BP_EXEC).\n");
				Type2 = BP_EXEC;
			}
		}
		else if (!stricmp(key, "no-logs")) {
			g_config.no_logs = value[0];
			if (g_config.no_logs)
				DebugOutput("Config: Debugger log diverted.\n");
		}
		else if (!stricmp(key, "disable-logging")) {
			g_config.disable_logging = value[0] == '1';
			if (g_config.disable_logging)
				DebugOutput("Config: Logging disabled (analysis log).\n");
		}
		else if (!stricmp(key, "base-on-alloc")) {
			g_config.base_on_alloc = value[0] == '1';
			if (g_config.base_on_alloc)
				DebugOutput("Config: Base breakpoints on executable memory allocations.\n");
		}
		else if (!stricmp(key, "base-on-caller")) {
			g_config.base_on_caller = value[0] == '1';
			if (g_config.base_on_caller)
				DebugOutput("Config: Base breakpoints on new calling regions.\n");
		}
		else if (!stricmp(key, "fake-rdtsc")) {
			g_config.fake_rdtsc = value[0] == '1';
			if (g_config.fake_rdtsc)
				DebugOutput("Config: Fake RDTSC enabled (Trace)\n");
		}
		else if (!stricmp(key, "nop-rdtscp")) {
			g_config.nop_rdtscp = value[0] == '1';
			if (g_config.nop_rdtscp)
				DebugOutput("Config: RDTSCP nop enabled\n");
		}
		else if (!stricmp(key, "procdump")) {
			g_config.procdump = (unsigned int)strtoul(value, NULL, 10);
			if (g_config.procdump)
				DebugOutput("Process dumps enabled.\n");
			else
				DebugOutput("Process dumps disabled.\n");
		}
		else if (!stricmp(key, "procmemdump")) {
			// for backwards compatibility with spender
			if (!stricmp(value, "yes"))
				g_config.procmemdump = 1;
			else
				g_config.procmemdump = value[0] == '1';
			if (g_config.procmemdump)
				DebugOutput("Full process memory dumps enabled.\n");
			else
				DebugOutput("Full process memory dumps disabled.\n");
		}
		else if (!stricmp(key, "import_reconstruction")) {
			g_config.import_reconstruction = value[0] == '1';
			if (g_config.import_reconstruction)
				DebugOutput("Import reconstruction of process dumps enabled.\n");
			else
				DebugOutput("Import reconstruction of process dumps disabled.\n");
		}
		else if (!stricmp(key, "terminate-processes")) {
			g_config.terminate_processes = value[0] == '1';
			if (g_config.terminate_processes)
				DebugOutput("Terminate processes on terminate_event enabled.\n");
			else
				DebugOutput("Terminate processes on terminate_event disabled.\n");
		}
		else if (!stricmp(key, "branch-trace")) {
			g_config.branch_trace = value[0] == '1';
			if (g_config.branch_trace)
				DebugOutput("Branch tracing enabled.\n");
		}
		else if (!stricmp(key, "unpacker")) {
			g_config.unpacker = (unsigned int)strtoul(value, NULL, 10);
			if (g_config.unpacker == 1)
				DebugOutput("Passive unpacking of payloads enabled\n");
			else if (g_config.unpacker == 2)
				DebugOutput("Active unpacking of payloads enabled\n");
		}
		else if (!stricmp(key, "injection")) { //When set to 1 this will enable CAPE�s capture of injected payloads between processes
			g_config.injection = value[0] == '1';
			if (g_config.injection)
				DebugOutput("Capture of injected payloads enabled.\n");
		}
		else if (!stricmp(key, "dump-config-region")) {
			g_config.dump_config_region = value[0] == '1';
			if (g_config.dump_config_region)
				DebugOutput("Dump config region enabled.\n");
		}
		else if (!stricmp(key, "single-process")) {
			g_config.single_process = value[0] == '1';
			if (g_config.single_process)
				DebugOutput("Monitoring child processes disabled.\n");
		}
		else if (!stricmp(key, "pdf")) {
			g_config.pdf = value[0] == '1';
			if (g_config.pdf && g_config.first_process) {
				DebugOutput("PDF (Adobe) settings enabled.\n");
				g_config.api_rate_cap = 2;
			}
		}
		else if (!stricmp(key, "api-rate-cap")) {
			g_config.api_rate_cap = (unsigned int)strtoul(value, NULL, 10);
			if (g_config.api_rate_cap)
				DebugOutput("API rate cap set to %d.\n", g_config.api_rate_cap);
		}
		else if (!stricmp(key, "api-cap")) {
			g_config.api_cap = (unsigned int)strtoul(value, NULL, 10);
			if (g_config.api_cap)
				DebugOutput("API cap set to %d.\n", g_config.api_cap);
		}
		else if (!stricmp(key, "dump-crypto")) {
			g_config.dump_crypto = value[0] == '1';
			if (g_config.dump_crypto)
				DebugOutput("Dumping of crypto API buffers enabled.\n");
		}
		else if (!stricmp(key, "dump-keys")) {
			g_config.dump_keys = value[0] == '1';
			if (g_config.dump_keys)
				DebugOutput("Dumping of crypto API ImportKey buffers enabled.\n");
		}
		else if (!stricmp(key, "caller-dump")) {
			g_config.caller_regions = value[0] == '1';
			if (g_config.caller_regions)
				DebugOutput("Dumps & scans of caller regions enabled.\n");
			else
				DebugOutput("Dumps & scans of caller regions disabled.\n");
		}
		else if (!stricmp(key, "yarascan")) {
			g_config.yarascan = value[0] == '1';
			if (g_config.yarascan)
				DebugOutput("In-monitor YARA scans enabled.\n");
			else
				DebugOutput("In-monitor YARA scans disabled.\n");
		}
		else if (!stricmp(key, "amsidump")) {
			g_config.amsidump = value[0] == '1';
			if (g_config.amsidump)
				DebugOutput("AMSI dumping enabled.\n");
			else
				DebugOutput("AMSI dumping disabled.\n");
		}
		else if (!stricmp(key, "minhook")) {
			g_config.minhook = value[0] == '1';
			if (g_config.minhook)
				DebugOutput("Minimal hook set enabled.\n");
		}
		else if (!stricmp(key, "zerohook")) {
			g_config.zerohook = value[0] == '1';
			if (g_config.zerohook)
				DebugOutput("All* hooks disabled (*except essential)\n");
		}
		else if (!stricmp(key, "tlsdump")) {
			g_config.tlsdump = value[0] == '1';
			if (g_config.tlsdump) {
				DebugOutput("TLS secret dump mode enabled.\n");
			}
		}
		else if (!stricmp(key, "regdump")) {
			g_config.regdump = value[0] == '1';
			if (g_config.regdump) {
				DebugOutput("Registry dump mode enabled.\n");
			}
		}
		else if (!stricmp(key, "loaderlock")) {
			g_config.loaderlock_scans = value[0] == '1';
			if (g_config.loaderlock_scans) {
				DebugOutput("Scans/dumps while loader lock held enabled.\n");
			}
			else
				DebugOutput("Scans/dumps while loader lock held disabled.\n");
		}
		else if (!stricmp(key, "plugx")) {
			g_config.plugx = value[0] == '1';
			if (g_config.plugx)
				DebugOutput("PlugX package enabled.\n");
		}
		else if (!stricmp(key, "syscall")) {
			g_config.syscall = value[0] == '1';
			if (g_config.syscall)
				DebugOutput("Syscall hooks enabled.\n");
			else
				DebugOutput("Syscall hooks disabled.\n");
		}
		else if (!stricmp(key, "loopskip")) {
			g_config.loopskip = value[0] == '1';
			if (g_config.loopskip)
				DebugOutput("Loop skipping enabled (instruction trace)\n");
		}
		else if (!stricmp(key, "break-on-jit")) {
			g_config.break_on_jit = value[0] == '1';
			if (g_config.break_on_jit)
				DebugOutput("Break on .NET JIT native code enabled.\n");
		}
		else if (!stricmp(key, "interactive")) {
			if (!g_config.interactive)
				g_config.interactive = value[0] == '1';
			if (g_config.interactive == 1)
				DebugOutput("Interactive desktop enabled.\n");
		}
		else if (stricmp(key, "no-iat"))
			DebugOutput("CAPE debug - unrecognised key %s.\n", key);

		// Replace the '=' we nulled for convenience
		line[strlen(line)] = '=';
	}
}

int read_config(void)
{
	char buf[32768], config_fname[MAX_PATH];
	FILE *fp;

	// config defaults
	g_config.debugger = 1;
	g_config.force_sleepskip = -1;
#ifdef _WIN64
	g_config.hook_type = HOOK_JMP_INDIRECT;
#else
	g_config.hook_type = HOOK_HOTPATCH_JMP_INDIRECT;
#endif
	g_config.ntdll_protect = 1;
	g_config.ntdll_remap = 1;
	g_config.procdump = 1;
	g_config.procmemdump = 0;
	g_config.dropped_limit = 0;
	g_config.injection = 1;
	g_config.unpacker = 1;
	g_config.api_cap = 5000;
	g_config.api_rate_cap = 1;
	g_config.yarascan = 1;
	g_config.loaderlock_scans = 1;
	g_config.amsidump = 1;
	g_config.syscall = 1;

	StepLimit = SINGLE_STEP_LIMIT;

	strcpy(g_config.results, g_config.analyzer);

	memset(g_config.str, 0, MAX_PATH);
	memset(g_config.pythonpath, 0, MAX_PATH);
	memset(g_config.w_results, 0, sizeof(WCHAR)*MAX_PATH);
	memset(g_config.w_analyzer, 0, sizeof(WCHAR)*MAX_PATH);
	memset(g_config.w_pythonpath, 0, sizeof(WCHAR)*MAX_PATH);

	// look for the config in monitor directory
	memset(g_config.analyzer, 0, MAX_PATH);
	strncpy(g_config.analyzer, our_dll_path, strlen(our_dll_path));
	PathRemoveFileSpec(g_config.analyzer); // remove filename
	sprintf(config_fname, "%s\\%u.ini", g_config.analyzer, GetCurrentProcessId());

	fp = fopen(config_fname, "r");

	// backward compatibility
	if (fp == NULL) {
		memset(config_fname, 0, sizeof(config_fname));
		sprintf(config_fname, "C:\\%u.ini", GetCurrentProcessId());
		fp = fopen(config_fname, "r");
	}

	// for debugging purposes
	if (fp == NULL) {
		memset(config_fname, 0, sizeof(config_fname));
		sprintf(config_fname, "%s\\config.ini", g_config.analyzer);
		fp = fopen(config_fname, "r");
		if (fp == NULL)
			return 0;
	}

	memset(buf, 0, sizeof(buf));
	if (fp) {
		while (fgets(buf, sizeof(buf), fp) != NULL) {
			// cut off the newline
			char *p = strchr(buf, '\r');
			if (p != NULL) *p = 0;
			p = strchr(buf, '\n');
			if (p != NULL) *p = 0;

			parse_config_line(buf);
		}
	}
	else g_config.standalone = 1;

	/* don't suspend logging if this isn't the first process or if we want all the logs */
	if (!g_config.first_process || g_config.full_logs)
		g_config.suspend_logging = FALSE;

	if (!wcslen(g_config.w_pythonpath)) {
		char* DummyString = "default";
		strncpy(g_config.pythonpath, DummyString, strlen(DummyString));
		for (unsigned int i = 0; i < ARRAYSIZE(g_config.pythonpath); i++)
			g_config.w_pythonpath[i] = (wchar_t)(unsigned short)g_config.pythonpath[i];
		DebugOutput("Python path defaulted to '%ws'.\n", g_config.w_pythonpath);
	}

	if (fp)
		fclose(fp);

	if (g_config.tlsdump) {
		g_config.syscall = 0;
		g_config.debugger = 0;
		g_config.procdump = 0;
		g_config.procmemdump = 0;
		g_config.dropped_limit = DROPPED_LIMIT;
		g_config.injection = 0;
		g_config.unpacker = 0;
		g_config.api_rate_cap = 0;
		g_config.yarascan = 0;
		g_config.amsidump = 0;
		g_config.bp0 = 0;
		g_config.bp1 = 0;
		g_config.bp2 = 0;
		g_config.br0 = 0;
		g_config.br1 = 0;
		g_config.br2 = 0;
		memset(g_config.break_on_return, 0, ARRAYSIZE(g_config.break_on_return));
	}

	if (TraceDepthLimit == 0xFFFFFFFF)
		TraceDepthLimit = 1;

	/* if no option supplied for dropped limit set a sensible value */
	if (!g_config.dropped_limit) {
		g_config.dropped_limit = DROPPED_LIMIT;
		DebugOutput("Dropped file limit defaulting to %d.\n", DROPPED_LIMIT);
	}

	PVOID ImageBase = GetModuleHandle(NULL);

	if (is_image_base_remapped(ImageBase))
		ImageBaseRemapped = TRUE;

	if (!_stricmp(our_process_name, "explorer.exe") && g_config.interactive == 1)
	{
		g_config.minhook = 1;
		DebugOutput("Interactive desktop - injecting Explorer Shell\n");
	}
	else
		g_config.interactive = 0;

	if (!ImageBaseRemapped && path_is_program_files(our_process_path_w) && VerifyCodeSection(ImageBase, our_process_path_w) == 1)
	{
#ifndef _WIN64
		if (!_stricmp(our_process_name, "firefox.exe"))
        {
			g_config.firefox = 1;
			g_config.injection = 0;
			g_config.unpacker = 0;
			g_config.caller_regions = 0;
			g_config.api_rate_cap = 0;
			g_config.procmemdump = 0;
			g_config.yarascan = 0;
			g_config.ntdll_protect = 0;
			DebugOutput("Firefox-specific hook-set enabled.\n");
        }
#endif
		if (!_stricmp(our_process_name, "iexplore.exe"))
        {
			g_config.iexplore = 1;
			g_config.injection = 0;
			g_config.api_rate_cap = 0;
			g_config.ntdll_protect = 0;
			g_config.procmemdump = 0;
			g_config.yarascan = 0;
			DebugOutput("Internet Explorer-specific hook-set enabled.\n");
        }

		if (!_stricmp(our_process_name, "msedge.exe"))
		{
			g_config.edge = 1;
			g_config.injection = 0;
			g_config.api_rate_cap = 0;
			g_config.ntdll_protect = 0;
			g_config.yarascan = 0;
			g_config.procmemdump = 0;
			DebugOutput("Edge-specific hook-set enabled.\n");
		}

		if (!_stricmp(our_process_name, "chrome.exe"))
		{
			g_config.chrome = 1;
			g_config.injection = 0;
			g_config.api_rate_cap = 0;
			g_config.ntdll_protect = 0;
			g_config.yarascan = 0;
			g_config.procmemdump = 0;
			DebugOutput("Chrome-specific hook-set enabled.\n");
		}

		if (strstr(our_process_path, "Microsoft Office"))
        {
			g_config.office = 1;
			g_config.unpacker = 0;
			g_config.caller_regions = 0;
			g_config.injection = 0;
			g_config.procmemdump = 0;
			g_config.yarascan = 0;
			g_config.ntdll_protect = 0;
			DebugOutput("Microsoft Office settings enabled.\n");
        }
	}
	else if (!ImageBaseRemapped && path_is_system(our_process_path_w) && VerifyCodeSection(ImageBase, our_process_path_w) == 1)
	{
		if (!_stricmp(our_process_name, "msiexec.exe")) {
			const char *excluded_apis[] = {
				"NtAllocateVirtualMemory",
				"NtProtectVirtualMemory",
				"VirtualProtectEx",
				"CryptDecodeMessage",
				"CryptDecryptMessage",
				"NtCreateThreadEx",
				"SetWindowLongPtrA",
				"SetWindowLongPtrW",
				"NtWaitForSingleObject",
				"NtSetTimer",
				"NtSetTimerEx",
				"RegOpenKeyExA",
				"RegOpenKeyExW",
				"RegCreateKeyExA",
				"RegCreateKeyExW",
				"RegDeleteKeyA",
				"RegDeleteKeyW",
				"RegEnumKeyW",
				"RegEnumKeyExA",
				"RegEnumKeyExW",
				"RegEnumValueA",
				"RegEnumValueW",
				"RegSetValueExA",
				"RegSetValueExW",
				"RegQueryValueExA",
				"RegQueryValueExW",
				"RegDeleteValueA",
				"RegDeleteValueW",
				"RegQueryInfoKeyA",
				"RegQueryInfoKeyW",
				"RegCloseKey",
				"RegNotifyChangeKeyValue",
				"NtCreateKey",
				"NtOpenKey",
				"NtOpenKeyEx",
				"NtRenameKey",
				"NtReplaceKey",
				"NtEnumerateKey",
				"NtEnumerateValueKey",
				"NtSetValueKey",
				"NtQueryValueKey",
				"NtQueryMultipleValueKey",
				"NtDeleteKey",
				"NtDeleteValueKey",
				"NtLoadKey",
				"NtLoadKey2",
				"NtLoadKeyEx",
				"NtQueryKey",
				"NtSaveKey",
				"NtSaveKeyEx"
			};

			for (unsigned int i = 0; i < sizeof(excluded_apis) / sizeof(excluded_apis[0]); i++) {
				if (!add_hook_exclusion(excluded_apis[i])) {
					DebugOutput("Unable to set hook exclusion for msiexec.\n");
					break;
				}
			}
			g_config.ntdll_protect = 0;
			g_config.procmemdump = 0;
			g_config.yarascan = 0;
			g_config.msi = 1;
			DebugOutput("MsiExec hook set enabled\n");
		}
		else if (
			(!_stricmp(our_process_name, "services.exe") && parent_has_path("C:\\Windows\\System32\\wininit.exe")) ||
			(!_stricmp(our_process_name, "svchost.exe") && parent_has_path("C:\\Windows\\System32\\services.exe") && (wcsstr(our_commandline, L"-k DcomLaunch") || wcsstr(our_commandline, L"-k netsvcs"))) ||
			(!_stricmp(our_process_name, "WmiPrvSE.exe") && !can_open_parent() && wcsstr(our_commandline, L"-Embedding"))
		) {
			g_config.procmemdump = 0;
			g_config.yarascan = 0;
			g_config.unpacker = 0;
			g_config.caller_regions = 0;
			g_config.injection = 0;
			g_config.syscall = 0;
			g_config.minhook = 1;
			disable_sleep_skip();
			DebugOutput("Services hook set enabled\n");
		}
		else if (!_stricmp(our_process_name, "wscript.exe")) {
			const char *excluded_apis[] = {
				"memcpy",
				"LoadResource",
				"LockResource",
				"SizeofResource",
			};
			for (unsigned int i = 0; i < sizeof(excluded_apis) / sizeof(excluded_apis[0]); i++) {
				if (!add_hook_exclusion(excluded_apis[i])) {
					DebugOutput("Unable to set hook exclusion for wscript\n");
					break;
				}
			}
			DebugOutput("wscript hook set enabled\n");
		}
	}

	return 1;
}
