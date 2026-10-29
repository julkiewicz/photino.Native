// Runs a command on a Win32 desktop of its own, so nothing it creates can show on, take focus from, or take input
// from the user's desktop. It samples the user's foreground window while the command runs and reports any change.
//   DesktopRun.exe <out file> --proof              a trivial window shown on the separate desktop for 2 s
//   DesktopRun.exe <out file> <command line...>    the command's output goes to <out file>
// Exit code: the command's, or 20 if the user's foreground window changed while it ran.

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

static int ProofWindow()
{
	WNDCLASSW wc = {};
	wc.lpfnWndProc = DefWindowProcW;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"DesktopRunProof";
	RegisterClassW(&wc);
	HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"proof", WS_OVERLAPPEDWINDOW, 100, 100, 400, 300,
		nullptr, nullptr, wc.hInstance, nullptr);
	ShowWindow(hwnd, SW_SHOW);
	printf("proof window %p visible %d on its desktop\n", (void*)hwnd, IsWindowVisible(hwnd));
	ULONGLONG end = GetTickCount64() + 2000;
	MSG msg;
	while (GetTickCount64() < end)
	{
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
			DispatchMessageW(&msg);
		Sleep(10);
	}
	DestroyWindow(hwnd);
	return 0;
}

int wmain(int argc, wchar_t** argv)
{
	if (argc >= 2 && wcscmp(argv[1], L"--proof-window") == 0)
		return ProofWindow();
	if (argc < 3)
	{
		printf("usage: DesktopRun.exe <out file> --proof | <command line...>\n");
		return 2;
	}

	std::wstring command;
	if (wcscmp(argv[2], L"--proof") == 0)
	{
		wchar_t self[MAX_PATH];
		GetModuleFileNameW(nullptr, self, MAX_PATH);
		command = L"\"" + std::wstring(self) + L"\" --proof-window";
	}
	else
	{
		for (int i = 2; i < argc; ++i)
			command += (i > 2 ? L" " : L"") + std::wstring(L"\"") + argv[i] + L"\"";
	}

	wchar_t desktopName[64];
	swprintf_s(desktopName, L"PhotinoCheck%lu", GetCurrentProcessId());
	HDESK desktop = CreateDesktopW(desktopName, nullptr, nullptr, 0, GENERIC_ALL, nullptr);
	if (!desktop)
	{
		printf("CreateDesktop failed: %lu\n", GetLastError());
		return 3;
	}

	SECURITY_ATTRIBUTES inherit = { sizeof(inherit), nullptr, TRUE };
	HANDLE out = CreateFileW(argv[1], GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, 0, nullptr);
	std::wstring desktopPath = std::wstring(L"WinSta0\\") + desktopName;
	STARTUPINFOW si = { sizeof(si) };
	si.lpDesktop = desktopPath.data();
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = out;
	si.hStdError = out;
	si.hStdInput = nullptr;
	PROCESS_INFORMATION pi = {};

	// The user's foreground window, sampled before, during and after.
	HWND before = GetForegroundWindow();
	std::atomic<bool> running = true;
	std::atomic<int> changes = 0, samples = 0;
	std::thread sampler([&] {
		while (running)
		{
			if (GetForegroundWindow() != before)
				++changes;
			++samples;
			Sleep(20);
		}
	});

	BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
	DWORD code = 4;
	if (started)
	{
		WaitForSingleObject(pi.hProcess, 300000);
		GetExitCodeProcess(pi.hProcess, &code);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	}
	else
	{
		printf("CreateProcess failed: %lu\n", GetLastError());
	}
	Sleep(200);
	running = false;
	sampler.join();
	HWND after = GetForegroundWindow();
	CloseHandle(out);
	CloseDesktop(desktop);

	printf("desktop %ls, exit code %lu, user foreground before %p after %p, %d of %d samples changed\n",
		desktopName, code, (void*)before, (void*)after, (int)changes, (int)samples);
	if (changes != 0 || before != after)
		return 20;
	return (int)code;
}
