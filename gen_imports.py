#!/usr/bin/env python3
"""
Generate the POD Gold import-bridge layer (Phase 3).

The lifted code reaches each Win32 import through its IAT slot VA. This emits a C
shim for every imported function and a {iat_va -> shim} bridge table the runtime
installs. Shims are stdcall-correct: each pops the right number of argument slots
(plus the dummy return address) so the simulated stack stays balanced even while a
shim is still a stub.

  MANUAL        -> hand-written body in src/runtime/shims_impl.c (extern decl emitted)
  FAIL_HRESULT  -> COM creators: return E_FAIL (0x80004005) so the game sees the
                   device-create fail gracefully instead of dereferencing an
                   uninitialised COM interface pointer
  NONZERO_DEF   -> stub returns 1 (a plausible handle/success) so init keeps going
  default       -> stub returns 0

Output: src/runtime/imports_gen.c  (committed; regenerate via this script)
Usage:  py -3.11 gen_imports.py [analysis/PODMMX.EXE] [src/runtime/imports_gen.c]
"""
import sys, os
_here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_here, 'tools', 'pcrecomp', 'tools', 'pe'))
from pe_analyze import analyze_pe, build_iat_map

# stdcall argument-slot counts (1 slot = 4 bytes) for every import PODMMX uses.
ARGC = {
 # GDI32
 'CreateCompatibleBitmap':3,'CreateCompatibleDC':1,'DeleteDC':1,'DeleteObject':1,
 'GetDeviceCaps':2,'GetStockObject':1,'SelectObject':2,'SetDIBitsToDevice':12,
 'StretchBlt':11,'StretchDIBits':13,
 # WINMM
 'joyGetDevCapsA':3,'joyGetPosEx':2,'mciGetErrorStringA':3,'mciSendCommandA':4,
 'mixerClose':1,'mixerGetControlDetailsA':3,'mixerGetDevCapsA':3,'mixerGetID':3,
 'mixerGetLineControlsA':3,'mixerGetLineInfoA':3,'mixerGetNumDevs':0,'mixerOpen':5,
 'mixerSetControlDetails':3,'timeGetTime':0,'waveOutClose':1,'waveOutGetDevCapsA':3,
 'waveOutGetErrorTextA':3,'waveOutGetPosition':3,'waveOutOpen':6,'waveOutPause':1,
 'waveOutPrepareHeader':3,'waveOutReset':1,'waveOutRestart':1,'waveOutUnprepareHeader':3,
 'waveOutWrite':3,
 # USER32
 'AdjustWindowRectEx':4,'CharUpperBuffA':2,'ClientToScreen':2,'ClipCursor':1,'CloseWindow':1,
 'CreatePopupMenu':0,'CreateWindowExA':12,'DefWindowProcA':4,'DestroyWindow':1,'DialogBoxParamA':5,
 'DispatchMessageA':1,'EndDialog':2,'FillRect':3,'GetClientRect':2,'GetCursorPos':1,'GetDC':1,
 'GetDlgItem':2,'GetFocus':0,'GetKeyNameTextA':3,'GetMenu':1,'GetMessageA':4,'GetSystemMetrics':1,
 'GetWindowDC':1,'GetWindowLongA':2,'GetWindowRect':2,'IsIconic':1,'LoadBitmapA':2,'LoadCursorA':2,
 'LoadIconA':2,'MessageBoxA':4,'MessageBoxW':4,'MsgWaitForMultipleObjects':5,'OpenIcon':1,
 'PeekMessageA':5,'PostMessageA':4,'PostQuitMessage':1,'PostThreadMessageA':4,'RegisterClassA':1,
 'RegisterWindowMessageA':1,'ReleaseDC':2,'ScreenToClient':2,'SendDlgItemMessageA':5,'SendMessageA':4,
 'SendMessageCallbackA':6,'SetClassLongA':3,'SetCursorPos':2,'SetDlgItemTextA':3,'SetDlgItemTextW':3,
 'SetForegroundWindow':1,'SetRect':5,'SetWindowLongA':3,'SetWindowPos':7,'SetWindowTextA':2,
 'SetWindowTextW':2,'ShowCursor':1,'ShowWindow':2,'TranslateAcceleratorA':3,'TranslateMessage':1,
 'UpdateWindow':1,'mouse_event':5,
 # KERNEL32
 'ClearCommBreak':1,'ClearCommError':3,'CloseHandle':1,'CreateDirectoryA':2,'CreateEventA':4,
 'CreateFileA':7,'CreateMutexA':3,'CreateProcessA':10,'CreateSemaphoreA':4,'CreateThread':6,
 'DebugBreak':0,'DeleteCriticalSection':1,'DeleteFileA':1,'DuplicateHandle':7,'EnterCriticalSection':1,
 'ExitProcess':1,'ExitThread':1,'FindFirstFileA':2,'FlushFileBuffers':1,'FreeLibrary':1,'GetACP':0,
 'GetCPInfo':2,'GetCommProperties':2,'GetCommState':2,'GetCommandLineA':0,'GetCommandLineW':0,
 'GetConsoleMode':2,'GetCurrentDirectoryA':2,'GetCurrentProcess':0,'GetCurrentProcessId':0,
 'GetCurrentThreadId':0,'GetCurrentThread':0,'GetDiskFreeSpaceA':5,'GetDriveTypeA':1,
 'GetEnvironmentStrings':0,'GetExitCodeProcess':2,'GetFileAttributesA':1,'GetFileSize':2,'GetFileType':1,
 'GetFullPathNameA':4,'GetLastError':0,'GetLocalTime':1,'GetLocaleInfoA':4,'GetModuleFileNameA':3,
 'GetModuleFileNameW':3,'GetModuleHandleA':1,'GetOEMCP':0,'GetOverlappedResult':4,'GetPrivateProfileIntA':4,
 'GetPrivateProfileStringA':6,'GetProcAddress':2,'GetStdHandle':1,'GetSystemDirectoryA':2,'GetSystemInfo':1,
 'GetTickCount':0,'GetTimeZoneInformation':1,'GetVersion':0,'GetVolumeInformationA':8,
 'GetWindowsDirectoryA':2,'GlobalAlloc':2,'GlobalFree':1,'GlobalMemoryStatus':1,'InitializeCriticalSection':1,
 'LeaveCriticalSection':1,'LoadLibraryA':1,'MoveFileA':2,'MultiByteToWideChar':6,'PurgeComm':2,
 'QueryPerformanceCounter':1,'QueryPerformanceFrequency':1,'ReadConsoleInputA':4,'ReadFile':5,
 'ReleaseMutex':1,'ReleaseSemaphore':3,'ResetEvent':1,'ResumeThread':1,'SetCommState':2,'SetCommTimeouts':2,
 'SetConsoleCtrlHandler':2,'SetConsoleMode':2,'SetEndOfFile':1,'SetEnvironmentVariableA':2,
 'SetEnvironmentVariableW':2,'SetEvent':1,'SetFilePointer':4,'SetLastError':1,'SetPriorityClass':2,
 'SetStdHandle':2,'SetThreadPriority':2,'SetUnhandledExceptionFilter':1,'SetupComm':3,'Sleep':1,
 'SuspendThread':1,'TerminateThread':2,'TlsAlloc':0,'TlsFree':1,'TlsGetValue':1,'TlsSetValue':2,
 'UnhandledExceptionFilter':1,'VirtualAlloc':4,'VirtualFree':3,'VirtualProtect':4,'VirtualQuery':3,
 'WaitForMultipleObjects':4,'WaitForSingleObject':2,'WideCharToMultiByte':8,'WriteConsoleA':5,'WriteFile':5,
 'WritePrivateProfileStringA':4,
 # DINPUT / DDRAW
 'DirectInputCreateA':4,'DirectDrawCreate':3,
 # AVIFIL32
 'AVIFileExit':0,'AVIFileGetStream':4,'AVIFileInfoA':3,'AVIFileInit':0,'AVIFileOpenA':4,'AVIFileRelease':1,
 'AVIStreamGetFrameClose':1,'AVIStreamGetFrameOpen':2,'AVIStreamGetFrame':2,'AVIStreamInfoA':3,
 'AVIStreamLength':1,'AVIStreamOpenFromFileA':6,'AVIStreamReadFormat':4,'AVIStreamRead':7,'AVIStreamRelease':1,
 'AVIStreamSampleToTime':2,'AVIStreamStart':1,'AVIStreamTimeToSample':2,
 # MSVFW32
 'DrawDibClose':1,'DrawDibDraw':13,'DrawDibOpen':0,'MCIWndCreateA':4,
 # VERSION
 'GetFileVersionInfoA':4,'GetFileVersionInfoSizeA':2,'VerQueryValueA':4,
}

# Imports with hand-written bodies in src/runtime/shims_impl.c.
MANUAL = {
 'GetVersion','GetACP','GetOEMCP','GetCPInfo','GetModuleHandleA','GetCommandLineA',
 'GetModuleFileNameA','GetCurrentDirectoryA','GetEnvironmentStrings','GetStdHandle','SetStdHandle',
 'VirtualAlloc','VirtualFree','GlobalAlloc','GlobalFree','GlobalMemoryStatus',
 'CreateFileA','ReadFile','WriteFile','SetFilePointer','CloseHandle','GetFileSize','GetFileType',
 'GetPrivateProfileStringA','GetPrivateProfileIntA','WritePrivateProfileStringA',
 'CreateWindowExA','GetDC','ReleaseDC','PeekMessageA','GetMessageA','StretchBlt',
 'MessageBoxA','timeGetTime','LoadLibraryA','GetProcAddress','ExitProcess','PostQuitMessage','PostThreadMessageA',
 'TlsAlloc','TlsFree','TlsSetValue','TlsGetValue',
 'DirectDrawCreate','DirectInputCreateA','GetDriveTypeA','GetVolumeInformationA','VirtualQuery',
 # threading + synchronization (thread_shim.c)
 'CreateThread','ResumeThread','SuspendThread','GetCurrentThreadId','ExitThread',
 'CreateEventA','SetEvent','ResetEvent','CreateMutexA','ReleaseMutex',
 'CreateSemaphoreA','ReleaseSemaphore','WaitForSingleObject','WaitForMultipleObjects',
 'MsgWaitForMultipleObjects','InitializeCriticalSection','DeleteCriticalSection',
 'EnterCriticalSection','LeaveCriticalSection',
 'waveOutGetDevCapsA','waveOutOpen','waveOutClose','waveOutReset','waveOutPause','waveOutRestart','waveOutPrepareHeader','waveOutUnprepareHeader','waveOutWrite','waveOutGetPosition','waveOutGetErrorTextA','joyGetDevCapsA','joyGetPosEx','mixerGetNumDevs',
}

# COM creators with no shim yet: return E_FAIL so the game's device init fails
# gracefully rather than dereferencing an uninitialised interface pointer.
# (DirectDrawCreate now has a real fake-COM shim in ddraw_shim.c -> MANUAL.)
FAIL_HRESULT = set()

# Stubs that return a non-zero "success/handle" so early init keeps going.
NONZERO_DEF = {
 'GetModuleHandleA','GetDC','GetWindowDC','CreateCompatibleDC','CreateCompatibleBitmap',
 'GetStockObject','SelectObject','RegisterClassA','LoadIconA','LoadCursorA','LoadBitmapA',
 'GetSystemMetrics','GetDesktopWindow','CreatePopupMenu','GetMenu','RealizePalette',
 'GetCurrentProcess','GetCurrentThread','GetStdHandle','BeginPaint','GetDeviceCaps',
 'SetThreadPriority',
 'GetTickCount','QueryPerformanceFrequency','QueryPerformanceCounter',
 'GetFileAttributesA','SetForegroundWindow','SetFocus',
 'GetFileVersionInfoSizeA','AVIStreamRelease','AVIFileRelease',
}

def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else os.path.join(_here,'analysis','PODMMX.EXE')
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(_here,'src','runtime','imports_gen.c')
    info = analyze_pe(exe)
    iat = build_iat_map(info)
    rows = sorted(iat.items())   # (va, (dll, name))

    missing = sorted({n for _,(_,n) in rows if n not in ARGC})
    if missing:
        print(f"WARNING: {len(missing)} imports without an ARGC entry (default 0): {missing}")

    L = ['/* POD Gold Recompilation - import bridge layer - AUTO-GENERATED */',
         '/* Regenerate: py -3.11 gen_imports.py */',
         '#define RECOMP_GENERATED_CODE',
         '#include "recomp_types.h"',
         '#include "imports.h"', '',
         f'/* {len(rows)} imports across {len({d for _,(d,_) in rows})} DLLs */', '']
    seen = {}
    for va,(dll,name) in rows:
        if name in seen:
            continue
        fn = f'imp_{name}'
        seen[name] = fn
        if name in MANUAL:
            L.append(f'extern void {fn}(void);  /* {dll}!{name} - hand-written in shims_impl.c */')
            continue
        argc = ARGC.get(name, 0)
        if name in FAIL_HRESULT:
            ret = '0x80004005u'   # E_FAIL
        elif name in NONZERO_DEF:
            ret = '1'
        else:
            ret = '0'
        L.append(f'/* {dll}!{name}  ({argc} args) */')
        L.append(f'static void {fn}(void) {{ IMPORT_STUB("{name}"); RET({ret}); STDRET({argc}); }}')
    L += ['', 'const recomp_dispatch_entry_t pod_import_bridges[] = {']
    L += [f'    {{ 0x{va:08X}u, {seen[name]} }},' for va,(dll,name) in rows]
    L += ['};', f'const uint32_t pod_import_bridge_count = {len(rows)};', '',
          '/* IAT slot VAs to self-patch so RECOMP_ICALL(slot)/ITAIL(MEM32(slot)) resolve. */',
          'const uint32_t pod_iat_slots[] = {']
    L += [f'    0x{va:08X}u,' for va,_ in rows]
    L += ['};', f'const uint32_t pod_iat_slot_count = {len(rows)};', '']

    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out,'w').write('\n'.join(L))
    print(f'Wrote {out}: {len(rows)} bridges ({len(seen)} unique shims)')

if __name__ == '__main__':
    main()
