"""PID-scoped Win32 dialog control; never sends global keystrokes."""
import ctypes
from ctypes import wintypes
import time
import json
import subprocess


def processes():
    result = subprocess.run(["powershell", "-NoProfile", "-Command",
        "Get-CimInstance Win32_Process | Select-Object ProcessId,ParentProcessId,ExecutablePath,"
        "@{Name='CreationTime';Expression={if ($_.CreationDate) {$_.CreationDate.ToFileTimeUtc()} else {0}}}"
        " | ConvertTo-Json -Compress"],
        capture_output=True, text=True, check=True)
    return json.loads(result.stdout)

USER = ctypes.WinDLL("user32", use_last_error=True)
CALLBACK = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
USER.EnumWindows.argtypes = [CALLBACK, wintypes.LPARAM]
USER.EnumChildWindows.argtypes = [wintypes.HWND, CALLBACK, wintypes.LPARAM]
USER.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
USER.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
USER.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
USER.GetDlgCtrlID.argtypes = [wintypes.HWND]
USER.IsWindowVisible.argtypes = [wintypes.HWND]
USER.IsWindow.argtypes = [wintypes.HWND]
USER.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]


def windows(pid, class_name=None, title_contains=None):
    result = []
    @CALLBACK
    def visit(hwnd, _):
        owner = wintypes.DWORD()
        USER.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        name = ctypes.create_unicode_buffer(256)
        USER.GetClassNameW(hwnd, name, 256)
        title = ctypes.create_unicode_buffer(512)
        USER.GetWindowTextW(hwnd, title, 512)
        if ((pid is None or owner.value == pid) and USER.IsWindowVisible(hwnd)
                and (class_name is None or name.value == class_name)
                and (title_contains is None or title_contains in title.value)):
            result.append(hwnd)
        return True
    USER.EnumWindows(visit, 0)
    return result


def wait_dialog(pid, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        found = windows(pid, "#32770")
        if found:
            return found[0]
        time.sleep(.05)
    raise AssertionError("Native dialog did not appear for the test app PID")


def click_dialog_button(hwnd, control_id):
    result = []
    @CALLBACK
    def visit(child, _):
        caption = ctypes.create_unicode_buffer(256)
        name = ctypes.create_unicode_buffer(256)
        USER.GetWindowTextW(child, caption, 256)
        USER.GetClassNameW(child, name, 256)
        wanted = {2: "Cancel", 6: "Yes", 7: "No"}.get(control_id)
        if (name.value == "Button" and
                (USER.GetDlgCtrlID(child) == control_id or caption.value.replace("&", "") == wanted)):
            result.append(child)
        return True
    USER.EnumChildWindows(hwnd, visit, 0)
    if not result:
        raise AssertionError(f"Native dialog has no button ID {control_id}")
    # BM_CLICK is scoped to the discovered child of this exact dialog.
    if not USER.PostMessageW(result[0], 0x00F5, 0, 0):
        raise ctypes.WinError(ctypes.get_last_error())


def request_close(hwnd):
    if not USER.PostMessageW(hwnd, 0x0010, 0, 0):
        raise ctypes.WinError(ctypes.get_last_error())


def process_alive(pid):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE
    if not handle:
        return False
    try:
        return kernel.WaitForSingleObject(handle, 0) == 0x102
    finally:
        kernel.CloseHandle(handle)


def terminate_verified(expected):
    """Fault injection into one verified process handle; never a PID tree."""
    from process_ownership import require_same_identity
    if expected is None or not 0 < expected[0] <= 0xffffffff:
        raise RuntimeError('Missing fault-injection process identity')
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD,
                                               wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    kernel.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel.OpenProcess(0x00101001, False, expected[0])
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        times = [wintypes.FILETIME() for _ in range(4)]
        if not kernel.GetProcessTimes(handle, *(ctypes.byref(value) for value in times)):
            raise ctypes.WinError(ctypes.get_last_error())
        size = wintypes.DWORD(32768)
        path = ctypes.create_unicode_buffer(size.value)
        if not kernel.QueryFullProcessImageNameW(handle, 0, path, ctypes.byref(size)):
            raise ctypes.WinError(ctypes.get_last_error())
        created = (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime
        require_same_identity(expected, dict(ProcessId=expected[0], CreationTime=created,
                                             ExecutablePath=path.value))
        if not kernel.TerminateProcess(handle, 1):
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        kernel.CloseHandle(handle)
