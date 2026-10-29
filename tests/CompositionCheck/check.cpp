// Headless check of Photino.Native's Windows hosting options. Every window is created hidden and never shown; input
// is synthesized as window messages posted to the check's own hidden window, so it goes through Photino's real
// routing code. With no arguments it runs each mode in a child process of its own and prints one line per check.
// Run it only through DesktopRun.exe (desktop_run.cpp), which puts it on a Win32 desktop of its own, never on the
// user's desktop:
//   DesktopRun.exe <out file> CompositionCheck.exe [--mode default|composition|composition-nohold] [--udf <folder>]

#include "Photino.h"

#include <d3d11.h>
#include <dxgi1_3.h>
#include <windowsx.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

extern "C"
{
	__declspec(dllimport) void Photino_register_win32(HINSTANCE hInstance);
	__declspec(dllimport) HWND Photino_getHwnd_win32(Photino* instance);
	__declspec(dllimport) Photino* Photino_ctorWithOptions_win32(PhotinoInitParams* initParams, PhotinoWindowsOptions* options);
	__declspec(dllimport) void Photino_WaitForExit(Photino* instance);
	__declspec(dllimport) void Photino_Close(Photino* instance);
	__declspec(dllimport) void Photino_Invoke(Photino* instance, ACTION callback);
	__declspec(dllimport) void Photino_SendWebMessage(Photino* instance, AutoString message);
	__declspec(dllimport) void Photino_SetSize(Photino* instance, int width, int height);
	__declspec(dllimport) void Photino_GetTransparentEnabled(Photino* instance, bool* enabled);
	__declspec(dllimport) void Photino_GetCompositionHostingEnabled_win32(Photino* instance, bool* enabled);
	__declspec(dllimport) HANDLE Photino_AddCompositionSurface_win32(Photino* instance, int x, int y, int width, int height);
	__declspec(dllimport) void Photino_SetCompositionSurfaceBounds_win32(Photino* instance, PhotinoSurfaceBounds* bounds, int count);
	__declspec(dllimport) void Photino_RemoveCompositionSurface_win32(Photino* instance, HANDLE surface);
	__declspec(dllimport) void Photino_SetHitRegions_win32(Photino* instance, int* rects, int count, bool regionsArePage);
}

using Clock = std::chrono::steady_clock;

// ---- shared state (written on the UI thread, read by the driver thread) ----
static std::mutex gLock;
static std::condition_variable gChanged;
static std::deque<std::wstring> gMainMessages;
static std::vector<std::wstring> gPopupMessages;
static std::vector<PhotinoHostInput> gHostInput;
static std::vector<std::pair<int, std::wstring>> gPopupRequests;
static std::map<std::wstring, Photino*> gPopupsByName;
static int gPopupClosings = 0;
static bool gReady = false;
static bool gDenyPopups = false;
static Photino* gMain = nullptr;
static HWND gMainHwnd = nullptr;
static bool gComposition = false;
static bool gNoHold = false;	// the control run: composition with the press hold turned off
static std::string gUdf;
static int gFailures = 0;

static void Check(bool ok, const std::string& name, const std::string& detail = "")
{
	printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.empty() ? "" : " | ", detail.c_str());
	fflush(stdout);
	if (!ok)
		++gFailures;
}

static std::string Narrow(const std::wstring& w)
{
	std::string s(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr), 0);
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), (int)s.size(), nullptr, nullptr);
	return s;
}

// Photino takes UTF-8 strings in AutoString parameters; some are copied with wcscpy, so pad them with NULs.
static AutoString Utf8(const std::string& s)
{
	char* buffer = new char[s.size() + 4]();
	memcpy(buffer, s.c_str(), s.size());
	return (AutoString)buffer;
}

// ---- running code on the UI thread ----
static std::mutex gInvokeLock;
static std::function<void()> gInvokeBody;
static void InvokeTrampoline() { gInvokeBody(); }
static void OnUi(Photino* instance, std::function<void()> body)
{
	std::lock_guard<std::mutex> guard(gInvokeLock);
	gInvokeBody = std::move(body);
	Photino_Invoke(instance, InvokeTrampoline);
}

static void SendToPage(const std::string& message)
{
	OnUi(gMain, [&] { Photino_SendWebMessage(gMain, Utf8(message)); });
}

// Waits for the next main-page message starting with prefix (earlier ones that do not match are dropped).
static std::wstring WaitFor(const std::wstring& prefix, int milliseconds = 10000)
{
	std::unique_lock<std::mutex> lock(gLock);
	auto deadline = Clock::now() + std::chrono::milliseconds(milliseconds);
	for (;;)
	{
		while (!gMainMessages.empty())
		{
			std::wstring message = gMainMessages.front();
			gMainMessages.pop_front();
			if (message.rfind(prefix, 0) == 0)
				return message;
		}
		if (gChanged.wait_until(lock, deadline) == std::cv_status::timeout && gMainMessages.empty())
			return L"";
	}
}

static bool WaitReady()
{
	std::unique_lock<std::mutex> lock(gLock);
	return gChanged.wait_for(lock, std::chrono::seconds(10), [] { return gReady; });
}

static std::map<std::string, std::string> Report()
{
	SendToPage("report");
	std::string text = Narrow(WaitFor(L"report:"));
	std::map<std::string, std::string> values;
	std::stringstream stream(text.size() > 7 ? text.substr(7) : "");
	std::string part;
	while (std::getline(stream, part, ';'))
	{
		size_t eq = part.find('=');
		if (eq != std::string::npos)
			values[part.substr(0, eq)] = part.substr(eq + 1);
	}
	return values;
}

static int Int(std::map<std::string, std::string>& r, const std::string& key) { return atoi(r[key].c_str()); }

static std::vector<int> Ints(const std::string& csv)
{
	std::vector<int> out;
	std::stringstream stream(csv);
	std::string part;
	while (std::getline(stream, part, ','))
		out.push_back(atoi(part.c_str()));
	return out;
}

static void ResetPage()
{
	SendToPage("reset");
	WaitFor(L"reset-done");
	std::lock_guard<std::mutex> guard(gLock);
	gHostInput.clear();
}

static int HostCount(UINT message)
{
	std::lock_guard<std::mutex> guard(gLock);
	int n = 0;
	for (auto& input : gHostInput)
		if (input.Message == message)
			++n;
	return n;
}

static void Settle(int milliseconds = 300) { std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds)); }

static void Post(UINT message, int x, int y, WPARAM keys)
{
	PostMessage(gMainHwnd, message, keys, MAKELPARAM(x, y));
}

static void PostWheel(int x, int y, int delta)
{
	POINT screen = { x, y };
	ClientToScreen(gMainHwnd, &screen);
	PostMessage(gMainHwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, delta), MAKELPARAM(screen.x, screen.y));
}

// ---- callbacks (Photino's style: plain function pointers) ----
static void OnMainMessage(AutoString message)
{
	std::lock_guard<std::mutex> guard(gLock);
	gMainMessages.push_back(message);
	if (wcscmp(message, L"ready") == 0)
		gReady = true;
	gChanged.notify_all();
}

static void OnPopupMessage(AutoString message)
{
	std::lock_guard<std::mutex> guard(gLock);
	gPopupMessages.push_back(message);
	gChanged.notify_all();
}

static bool OnPopupClosing()
{
	std::lock_guard<std::mutex> guard(gLock);
	++gPopupClosings;
	return false;	// let it close
}

static bool OnMainClosing() { return false; }

static void OnHostInput(const PhotinoHostInput* input)
{
	std::lock_guard<std::mutex> guard(gLock);
	gHostInput.push_back(*input);
}

static void* OnResource(AutoString url, int* numBytes, AutoString* contentType)
{
	static wchar_t html[] = L"text/html";
	std::wstring path = url;
	size_t slash = path.find_last_of(L'/');
	std::wstring file = std::wstring(CHECK_WEB_DIR) + L"/" + path.substr(slash + 1);
	HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (handle == INVALID_HANDLE_VALUE)
		return nullptr;
	DWORD size = GetFileSize(handle, nullptr), read = 0;
	void* data = CoTaskMemAlloc(size);
	ReadFile(handle, data, size, &read, nullptr);
	CloseHandle(handle);
	*numBytes = (int)read;
	*contentType = html;
	return data;
}

static PhotinoInitParams BaseParams(const std::string& title)
{
	PhotinoInitParams params = {};
	params.Size = sizeof(PhotinoInitParams);
	params.Title = Utf8(title);
	params.TemporaryFilesPath = Utf8(gUdf);
	params.CustomSchemeNames[0] = Utf8("https");
	params.CustomSchemeHandler = (WebResourceRequestedCallback*)OnResource;
	params.Left = 50;
	params.Top = 50;
	params.Width = 1280;
	params.Height = 800;
	params.Zoom = 100;
	params.MaxWidth = INT_MAX;
	params.MaxHeight = INT_MAX;
	params.Resizable = true;
	params.ContextMenuEnabled = true;
	params.DevToolsEnabled = true;
	params.WebSecurityEnabled = true;
	params.SmoothScrollingEnabled = false;
	return params;
}

static bool OnPopupRequested(int requestId, AutoString uri)
{
	{
		std::lock_guard<std::mutex> guard(gLock);
		gPopupRequests.push_back({ requestId, uri });
		if (gDenyPopups)
			return false;
	}
	PhotinoInitParams params = BaseParams("popup");
	params.Width = 400;
	params.Height = 300;
	params.WebMessageReceivedHandler = (WebMessageReceivedCallback*)OnPopupMessage;
	params.ClosingHandler = (ClosingCallback*)OnPopupClosing;
	PhotinoWindowsOptions options = {};
	options.Size = sizeof(PhotinoWindowsOptions);
	options.StartHidden = true;
	options.CompositionHosting = gComposition;
	options.PopupOpener = gMain;
	options.PopupRequestId = requestId;
	Photino* popup = Photino_ctorWithOptions_win32(&params, &options);
	std::lock_guard<std::mutex> guard(gLock);
	gPopupsByName[L"request" + std::to_wstring(requestId)] = popup;
	return true;
}

static int CountChildWindows(HWND hwnd)
{
	int count = 0;
	EnumChildWindows(hwnd, [](HWND, LPARAM counter) -> BOOL { ++*(int*)counter; return TRUE; }, (LPARAM)&count);
	return count;
}

// ---- the composition surface, filled from a thread of its own as a renderer would ----
static void CheckSurface()
{
	HANDLE surface = nullptr;
	OnUi(gMain, [&] { surface = Photino_AddCompositionSurface_win32(gMain, 0, 0, 800, 600); });
	Check(surface != nullptr, "surface: Photino made a composition surface handle in the window's tree");
	if (!surface)
		return;

	HRESULT created = E_FAIL, resized = E_FAIL;
	int presented = 0;
	std::thread renderer([&] {
		ID3D11Device* device = nullptr;
		ID3D11DeviceContext* context = nullptr;
		if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
			nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)))
			return;
		IDXGIFactoryMedia* factory = nullptr;
		CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
		DXGI_SWAP_CHAIN_DESC1 desc = {};
		desc.Width = 800;
		desc.Height = 600;
		desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		desc.BufferCount = 2;
		desc.Scaling = DXGI_SCALING_STRETCH;
		desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
		desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
		IDXGISwapChain1* swapChain = nullptr;
		created = factory ? factory->CreateSwapChainForCompositionSurfaceHandle(device, surface, &desc, nullptr, &swapChain) : E_FAIL;
		auto draw = [&](int frames) {
			for (int i = 0; i < frames; ++i)
			{
				ID3D11Texture2D* buffer = nullptr;
				ID3D11RenderTargetView* target = nullptr;
				swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer));
				device->CreateRenderTargetView(buffer, nullptr, &target);
				const float color[4] = { 0.2f, 0.4f, 0.8f, 1.0f };
				context->ClearRenderTargetView(target, color);
				target->Release();
				buffer->Release();
				if (SUCCEEDED(swapChain->Present(0, 0)))
					++presented;
			}
		};
		if (SUCCEEDED(created))
		{
			draw(30);
			context->ClearState();
			context->Flush();
			resized = swapChain->ResizeBuffers(0, 640, 400, DXGI_FORMAT_UNKNOWN, 0);
			if (SUCCEEDED(resized))
				draw(10);
			swapChain->Release();
		}
		if (factory) factory->Release();
		context->Release();
		device->Release();
	});
	renderer.join();

	PhotinoSurfaceBounds bounds = { surface, 10, 20, 640, 400 };
	OnUi(gMain, [&] { Photino_SetCompositionSurfaceBounds_win32(gMain, &bounds, 1); });
	char detail[160];
	sprintf_s(detail, "create 0x%08lx, resize 0x%08lx, %d presents", (unsigned long)created, (unsigned long)resized, presented);
	Check(SUCCEEDED(created) && SUCCEEDED(resized) && presented == 40,
		"surface: a D3D11 swap chain made for the handle on another thread presents and resizes", detail);
	OnUi(gMain, [&] { Photino_RemoveCompositionSurface_win32(gMain, surface); });
	DWORD flags = 0;
	Check(GetHandleInformation(surface, &flags) == FALSE,
		"surface: removing it closes the handle (after the swap chain was released)");
}

// ---- the checks of each mode ----
static void DriveDefault()
{
	Check(WaitReady(), "default: the page loaded");
	bool composition = true, transparent = true;
	OnUi(gMain, [&] {
		Photino_GetCompositionHostingEnabled_win32(gMain, &composition);
		Photino_GetTransparentEnabled(gMain, &transparent);
	});
	Check(!composition, "default: no composition controller");
	Check(!transparent, "default: page background opaque, as before");
	int children = CountChildWindows(gMainHwnd);
	Check(children > 0, "default: WebView2 hosted in a child window (HWND hosting)", std::to_string(children) + " child windows");
	Check(!IsWindowVisible(gMainHwnd), "default: window never shown");
	SendToPage("echo:hello");
	Check(WaitFor(L"echo:") == L"echo:hello", "default: web message round trip");
	SendToPage("send-prefixed");
	Check(WaitFor(L"__photino_hit_regions__") == L"__photino_hit_regions__:page:1,2,3,4",
		"default: messages reach the host unchanged (no hit-region interception)");
	auto r = Report();
	Check(r["hasPhotino"] == "false", "default: no window.photino injected");
}

static void DriveComposition()
{
	std::wstring regions = WaitFor(L"regions-published:");	// before or after the load event
	Check(WaitReady(), "composition: the page loaded");
	Check(!regions.empty(), "composition: the page published its hit region", Narrow(regions));

	bool composition = false, transparent = false;
	OnUi(gMain, [&] {
		Photino_GetCompositionHostingEnabled_win32(gMain, &composition);
		Photino_GetTransparentEnabled(gMain, &transparent);
	});
	Check(composition, "composition: composition controller in a DirectComposition tree");
	Check(transparent, "composition: WebView2 default background transparent");
	Check(!IsWindowVisible(gMainHwnd), "composition: window never shown");
	Check(true, "composition: child windows (information)", std::to_string(CountChildWindows(gMainHwnd)));
	auto r = Report();
	Check(true, "composition: page visibility (information)", r["visibility"] + ", hit region " + r["hasPhotino"]);
	Check(r["bodyBackground"] == "rgba(0, 0, 0, 0)" && r["htmlBackground"] == "rgba(0, 0, 0, 0)",
		"composition: page pixels transparent outside the panel", r["bodyBackground"]);

	if (!gNoHold)
		CheckSurface();

	RECT client;
	GetClientRect(gMainHwnd, &client);
	std::vector<int> button = Ints(r["button"]), list = Ints(r["list"]);
	int bx = button[0] + button[2] / 2, by = button[1] + button[3] / 2;
	int lx = list[0] + list[2] / 2, ly = list[1] + 100;
	int sx = 200, sy = 400;	// a point of the host's area (the 3D view in a real host)

	if (!gNoHold)
	{
		ResetPage();
		Post(WM_MOUSEMOVE, lx, ly, 0);
		Post(WM_MOUSEMOVE, lx + 5, ly + 5, 0);
		Settle();
		r = Report();
		Check(Int(r, "pointermove") >= 1 && HostCount(WM_MOUSEMOVE) == 0, "routing: hover over the panel goes to the page",
			"page moves " + r["pointermove"] + ", host moves " + std::to_string(HostCount(WM_MOUSEMOVE)));
		Post(WM_MOUSEMOVE, sx, sy, 0);
		Settle();
		r = Report();
		Check(HostCount(WM_MOUSEMOVE) == 1 && Int(r, "pointerleave") == 1, "routing: hover over the host's area goes to the host; the page gets a leave",
			"host moves " + std::to_string(HostCount(WM_MOUSEMOVE)) + ", page leaves " + r["pointerleave"]);

		ResetPage();
		Post(WM_LBUTTONDOWN, bx, by, MK_LBUTTON);
		Post(WM_LBUTTONUP, bx, by, 0);
		Settle();
		r = Report();
		Check(Int(r, "click") == 1 && HostCount(WM_LBUTTONDOWN) == 0, "routing: a click on the panel's button reaches the page",
			"clicks " + r["click"] + " at " + r["lastClick"]);

		ResetPage();
		Post(WM_LBUTTONDOWN, sx, sy, MK_LBUTTON);
		for (int x = sx; x <= lx; x += (lx - sx) / 5)
		{
			Post(WM_MOUSEMOVE, x, ly, MK_LBUTTON);
			Settle(40);
		}
		Post(WM_LBUTTONUP, lx, ly, 0);
		Settle();
		r = Report();
		Check(HostCount(WM_LBUTTONDOWN) == 1 && HostCount(WM_LBUTTONUP) == 1 && HostCount(WM_MOUSEMOVE) >= 6 && Int(r, "pointerdown") == 0 && Int(r, "pointermove") == 0,
			"routing: a drag from the host's area into the panel stays with the host",
			"host down/moves/up " + std::to_string(HostCount(WM_LBUTTONDOWN)) + "/" + std::to_string(HostCount(WM_MOUSEMOVE)) + "/" + std::to_string(HostCount(WM_LBUTTONUP))
			+ ", page down/moves " + r["pointerdown"] + "/" + r["pointermove"]);

		ResetPage();
		Post(WM_LBUTTONDOWN, lx, ly, MK_LBUTTON);
		for (int x = lx; x >= sx; x -= (lx - sx) / 5)
		{
			Post(WM_MOUSEMOVE, x, ly, MK_LBUTTON);
			Settle(40);
		}
		Post(WM_LBUTTONUP, sx, ly, 0);
		Settle();
		r = Report();
		Check(Int(r, "pointerdown") == 1 && Int(r, "pointerup") == 1 && Int(r, "pointermove") >= 3 && HostCount(WM_LBUTTONDOWN) == 0 && HostCount(WM_MOUSEMOVE) == 0 && HostCount(WM_LBUTTONUP) == 0,
			"routing: a drag from the panel out over the host's area stays with the page",
			"page down/moves/up " + r["pointerdown"] + "/" + r["pointermove"] + "/" + r["pointerup"] + ", host events " + std::to_string(HostCount(WM_MOUSEMOVE) + HostCount(WM_LBUTTONDOWN)));

		ResetPage();
		Post(WM_MOUSEMOVE, lx, ly, 0);	// as a real wheel: the pointer got there first
		Settle(50);
		PostWheel(lx, ly, -WHEEL_DELTA * 3);
		for (int i = 0; i < 10; ++i)	// the scroll itself lands a frame or more after the wheel event
		{
			Settle(100);
			r = Report();
			if (Int(r, "scrollTop") > 0)
				break;
		}
		Check(Int(r, "wheel") == 1 && Int(r, "scrollTop") > 0 && HostCount(WM_MOUSEWHEEL) == 0, "routing: the wheel over the list scrolls the page",
			"wheel events " + r["wheel"] + " (" + r["lastWheel"] + "), scrollTop " + r["scrollTop"]);
		PostWheel(sx, sy, WHEEL_DELTA);
		Settle();
		r = Report();
		Check(HostCount(WM_MOUSEWHEEL) == 1 && Int(r, "wheel") == 1, "routing: the wheel over the host's area goes to the host");

		// Regions given by the host, in 'host' mode: inside them goes to the host, everywhere else to the page.
		ResetPage();
		int hostRect[4] = { 0, 0, 100, 100 };
		OnUi(gMain, [&] { Photino_SetHitRegions_win32(gMain, hostRect, 1, false); });
		Post(WM_MOUSEMOVE, 50, 50, 0);
		Post(WM_MOUSEMOVE, 500, 500, 0);
		Settle();
		r = Report();
		Check(HostCount(WM_MOUSEMOVE) == 1 && Int(r, "pointermove") >= 1, "routing: 'host' mode regions route inside to the host, outside to the page");
		SendToPage("publish");
		WaitFor(L"regions-published:");
	}

	// A press right after the window's size changed waits for the page's new region instead of using the old one.
	ResetPage();
	Settle();
	int newWidth = 1000, newHeight = 700;
	ULONGLONG posted = 0;
	OnUi(gMain, [&] {
		Photino_SetSize(gMain, newWidth, newHeight);
		RECT resized;
		GetClientRect(gMainHwnd, &resized);
		int nbx = resized.right - (client.right - bx);	// the panel is anchored to the right edge
		Post(WM_LBUTTONDOWN, nbx, by, MK_LBUTTON);
		Post(WM_LBUTTONUP, nbx, by, 0);
		posted = GetTickCount64();
	});
	std::wstring republished = WaitFor(L"regions-published:");
	ULONGLONG arrived = GetTickCount64();
	Settle();
	r = Report();
	if (gNoHold)
		Check(!republished.empty() && Int(r, "click") == 0 && HostCount(WM_LBUTTONDOWN) == 1,
			"control (hold off): the same press is routed by the old region and goes to the host",
			"clicks " + r["click"] + ", host presses " + std::to_string(HostCount(WM_LBUTTONDOWN)));
	else
		Check(!republished.empty() && Int(r, "click") == 1 && HostCount(WM_LBUTTONDOWN) == 0,
			"hold: a press after a resize is held until the new region, then reaches the page's moved button",
			"region republished within " + std::to_string(arrived - posted) + " ms, clicks " + r["click"] + ", host presses " + std::to_string(HostCount(WM_LBUTTONDOWN)));
}

static void DrivePopups()
{
	const wchar_t* mode = gComposition ? L"composition" : L"default";
	std::string tag = gComposition ? "popup (composition): " : "popup (default): ";
	auto popupOf = [&](int requestId) {
		std::lock_guard<std::mutex> guard(gLock);
		return gPopupsByName[L"request" + std::to_wstring(requestId)];
	};
	auto lastRequest = [&] {
		std::lock_guard<std::mutex> guard(gLock);
		return gPopupRequests.empty() ? 0 : gPopupRequests.back().first;
	};

	SendToPage("open-popup:p1");
	std::wstring ok = WaitFor(L"popup-");
	Check(ok.rfind(L"popup-ok:p1:", 0) == 0, tag + "window.open hosted, scripted by the page", Narrow(ok));
	Check(ok.find(L"probe=42;text=from-opener;href=https://photino.check/popup.html;opener=true") != std::wstring::npos,
		tag + "the popup is scriptable both ways and knows its opener");
	if (gComposition)
		Check(ok.find(L"photino=true") != std::wstring::npos, tag + "the popup's page has window.photino for its own regions");
	Photino* p1 = popupOf(lastRequest());
	HWND p1Hwnd = p1 ? Photino_getHwnd_win32(p1) : nullptr;
	bool p1Composition = false;
	if (p1)
		OnUi(gMain, [&] { Photino_GetCompositionHostingEnabled_win32(p1, &p1Composition); });
	Check(p1Hwnd && !IsWindowVisible(p1Hwnd) && p1Composition == gComposition, tag + "a hidden window of the same kind");
	{
		std::lock_guard<std::mutex> guard(gLock);
		Check(!gPopupMessages.empty(), tag + "the popup's own messages reach its own callback");
	}

	SendToPage("close-popup:p1");
	std::wstring closed = WaitFor(L"popup-closed:p1");
	Settle();
	Check(closed == L"popup-closed:p1:true" && !IsWindow(p1Hwnd) && gPopupClosings == 1, tag + "the page closing it closes the window (closing callback first)",
		Narrow(closed));

	SendToPage("open-popup:p2");
	ok = WaitFor(L"popup-");
	Photino* p2 = popupOf(lastRequest());
	HWND p2Hwnd = p2 ? Photino_getHwnd_win32(p2) : nullptr;
	OnUi(gMain, [&] { Photino_Close(p2); });
	SendToPage("watch-popup:p2");
	closed = WaitFor(L"popup-closed:p2");
	Check(ok.rfind(L"popup-ok:p2:", 0) == 0 && closed == L"popup-closed:p2:true" && !IsWindow(p2Hwnd) && gPopupClosings == 2,
		tag + "the host closing it: the page sees it closed", Narrow(closed));

	{
		std::lock_guard<std::mutex> guard(gLock);
		gDenyPopups = true;
	}
	SendToPage("open-popup:p3");
	Check(WaitFor(L"popup-") == L"popup-null:p3", tag + "a refused request: window.open returns null");
	(void)mode;
}

int wmain(int argc, wchar_t** argv)
{
	std::wstring mode;
	std::wstring udf;
	for (int i = 1; i + 1 < argc; ++i)
	{
		if (wcscmp(argv[i], L"--mode") == 0) mode = argv[i + 1];
		if (wcscmp(argv[i], L"--udf") == 0) udf = argv[i + 1];
	}
	wchar_t exe[MAX_PATH];
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	if (udf.empty())
	{
		udf = exe;
		udf = udf.substr(0, udf.find_last_of(L'\\')) + L"\\check-udf";
	}

	if (mode.empty())
	{
		// Each mode in a process of its own (a Photino message loop ends with its root window).
		int failed = 0;
		for (const wchar_t* each : { L"default", L"composition", L"composition-nohold" })
		{
			std::wstring command = L"\"" + std::wstring(exe) + L"\" --mode " + each + L" --udf \"" + udf + L"-" + each + L"\"";
			STARTUPINFOW si = { sizeof(si) };
			PROCESS_INFORMATION pi = {};
			if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi))
				return 10;
			WaitForSingleObject(pi.hProcess, INFINITE);
			DWORD code = 1;
			GetExitCodeProcess(pi.hProcess, &code);
			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);
			printf("== mode %ls: exit code %lu\n", each, code);
			failed += code != 0;
		}
		printf(failed == 0 ? "ALL PASSED\n" : "FAILED\n");
		return failed;
	}

	gComposition = mode.rfind(L"composition", 0) == 0;
	gNoHold = mode == L"composition-nohold";
	gUdf = Narrow(udf);
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	Photino_register_win32(GetModuleHandleW(nullptr));

	std::thread([] {
		std::this_thread::sleep_for(std::chrono::seconds(120));
		printf("FAIL watchdog: the check took over 120 s\n");
		fflush(stdout);
		ExitProcess(3);
	}).detach();

	PhotinoInitParams params = BaseParams("composition check");
	params.StartUrl = Utf8("https://photino.check/main.html");
	params.WebMessageReceivedHandler = (WebMessageReceivedCallback*)OnMainMessage;
	params.ClosingHandler = (ClosingCallback*)OnMainClosing;
	PhotinoWindowsOptions options = {};
	options.Size = sizeof(PhotinoWindowsOptions);
	options.StartHidden = true;
	options.CompositionHosting = gComposition;
	options.HitRegionHoldMilliseconds = gNoHold ? -1 : 0;
	options.HostInputHandler = OnHostInput;
	options.PopupRequestedHandler = OnPopupRequested;
	gMain = Photino_ctorWithOptions_win32(&params, &options);
	gMainHwnd = Photino_getHwnd_win32(gMain);

	std::thread driver([] {
		auto started = Clock::now();
		if (gComposition)
			DriveComposition();
		else
			DriveDefault();
		if (!gNoHold)
			DrivePopups();
		printf("mode took %lld ms\n", (long long)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count());
		OnUi(gMain, [] { Photino_Close(gMain); });
	});
	Photino_WaitForExit(gMain);
	driver.join();
	return gFailures == 0 ? 0 : 1;
}
