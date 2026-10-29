#pragma once

#ifdef _WIN32
#include <Windows.h>
#include <wil/com.h>
#include <WebView2.h>
#include <dcomp.h>
typedef wchar_t *AutoString;
class WinToastHandler;
#else
// AutoString for macOS/Linux
typedef char *AutoString;
#endif

#ifdef __APPLE__
#include <Cocoa/Cocoa.h>
#include <Foundation/Foundation.h>
#include <UserNotifications/UserNotifications.h>
#include <WebKit/WebKit.h>
#include <WebKit/WKWebView.h>
#include <WebKit/WKWebViewConfiguration.h>
#include <Security/SecTrust.h>
#endif

#ifdef __linux__
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#endif

#include <map>
#include <string>
#include <vector>

struct Monitor
{
	struct MonitorRect
	{
		int x, y;
		int width, height;
	} monitor, work;
	double scale;
};

typedef void (*ACTION)();
typedef void (*WebMessageReceivedCallback)(AutoString message);
typedef void *(*WebResourceRequestedCallback)(AutoString url, int *outNumBytes, AutoString *outContentType);
typedef int (*GetAllMonitorsCallback)(const Monitor *monitor);
typedef void (*ResizedCallback)(int width, int height);
typedef void (*MaximizedCallback)();
typedef void (*RestoredCallback)();
typedef void (*MinimizedCallback)();
typedef void (*MovedCallback)(int x, int y);
typedef bool (*ClosingCallback)();
typedef void (*FocusInCallback)();
typedef void (*FocusOutCallback)();

class PhotinoDialog;
class Photino;

struct PhotinoInitParams
{
	AutoString StartString;
	AutoString StartUrl;
	AutoString Title;
	AutoString WindowIconFile;
	AutoString TemporaryFilesPath;
	AutoString UserAgent;
	AutoString BrowserControlInitParameters;
	AutoString NotificationRegistrationId;

	Photino *ParentInstance;

	ClosingCallback *ClosingHandler;
	FocusInCallback *FocusInHandler;
	FocusOutCallback *FocusOutHandler;
	ResizedCallback *ResizedHandler;
	MaximizedCallback *MaximizedHandler;
	RestoredCallback *RestoredHandler;
	MinimizedCallback *MinimizedHandler;
	MovedCallback *MovedHandler;
	WebMessageReceivedCallback *WebMessageReceivedHandler;
	AutoString CustomSchemeNames[16];
	WebResourceRequestedCallback *CustomSchemeHandler;

	int Left;
	int Top;
	int Width;
	int Height;
	int Zoom;
	int MinWidth;
	int MinHeight;
	int MaxWidth;
	int MaxHeight;

	bool CenterOnInitialize;
	bool Chromeless;
	bool Transparent;
	bool ContextMenuEnabled;
	bool DevToolsEnabled;
	bool FullScreen;
	bool Maximized;
	bool Minimized;
	bool Resizable;
	bool Topmost;
	bool UseOsDefaultLocation;
	bool UseOsDefaultSize;
	bool GrantBrowserPermissions;
	bool MediaAutoplayEnabled;
	bool FileSystemAccessEnabled;
	bool WebSecurityEnabled;
	bool JavascriptClipboardAccessEnabled;
	bool MediaStreamEnabled;
	bool SmoothScrollingEnabled;
    bool IgnoreCertificateErrorsEnabled;
	bool NotificationsEnabled;

	int Size;
};

#ifdef _WIN32
// Windows-only hosting options, passed with Photino_ctorWithOptions_win32. A zeroed struct (with Size set) gives
// Photino's usual behavior, except that the window's own placement calls (position, size, topmost, centre) never
// activate it; Photino_ctor is unchanged. Every Photino_*_win32 call for these options is made on the window's thread.

// One mouse or key message the composition mode routes to the host instead of the page. X and Y are in client
// pixels (also for the wheel, whose LParam is in screen pixels); WParam and LParam are the window message's own.
struct PhotinoHostInput
{
	unsigned int Message;
	int X;
	int Y;
	unsigned long long WParam;
	long long LParam;
};

// Where a composition surface sits in the window's client area, in physical pixels.
struct PhotinoSurfaceBounds
{
	HANDLE Surface;
	int X;
	int Y;
	int Width;
	int Height;
};

// Timer ids Photino uses on a window with these options: a popup request's timeout is this plus its id.
#define PHOTINO_POPUP_REQUEST_TIMER_BASE 0x50500000

// What window.open asked for (its features string), in pixels; a Has flag is false when the page gave no value.
struct PhotinoPopupFeatures
{
	bool HasPosition;
	bool HasSize;
	int Left;
	int Top;
	int Width;
	int Height;
};

// Return true when the host handled the message: Photino then skips its default handling (for example a key that
// would open the window menu, or WM_SETCURSOR when the host set its own cursor).
typedef bool (*HostInputCallback)(const PhotinoHostInput *input);
// The page called window.open. Return true to host it: the host then creates a window (on this thread) with
// PhotinoWindowsOptions.PopupOpener set to this instance and PopupRequestId to requestId, or refuses it later with
// Photino_RefusePopupRequest_win32. Return false to refuse it now (window.open returns null in the page). A popup
// window whose request was refused (or timed out) before its page arrived closes itself (its closing callback runs).
// The page's script is blocked until the request is fulfilled or refused, so fulfil it promptly; a request still
// open after PopupRequestTimeoutMilliseconds is refused.
typedef bool (*PopupRequestedCallback)(int requestId, AutoString uri, const PhotinoPopupFeatures *features);
// The window was destroyed (WM_DESTROY): no callback is made for this window after this one.
typedef void (*DestroyedCallback)();

struct PhotinoWindowsOptions
{
	// Create the window without showing it (the page still loads, lays out and runs; it never takes focus).
	// Photino_Show_win32 shows it. With these options, minimize, maximize and restore (at creation or later) never
	// activate the window: on a hidden window they are kept for Photino_Show_win32; on a visible one behind others,
	// minimize and restore apply without activating it, and maximize (Windows has no non-activating maximize) applies
	// when the window is next activated.
	bool StartHidden;
	// Host WebView2 through its composition controller in a DirectComposition tree: the page is a transparent
	// visual above a host layer that Photino_AddCompositionSurface_win32 fills (usable as soon as the window exists;
	// closing the window closes the surface handles it made). Mouse input is routed by hit regions: until the page
	// (or the host) publishes some, every input goes to the host. If composition is not available the window falls
	// back to the usual hosting; Photino_GetCompositionHostingEnabled_win32 says which one is active.
	bool CompositionHosting;
	// Composition mode: after the window's size or DPI changes, a press waits up to this long for the page to
	// publish its new hit regions before it is routed. 0 means the default (100 ms), a negative value turns it off.
	int HitRegionHoldMilliseconds;
	// Composition mode: receives the mouse input outside the page's hit regions and key input while the window
	// (not the page) has focus. May be null.
	HostInputCallback HostInputHandler;
	// Called when the page calls window.open. Null keeps WebView2's own behavior.
	PopupRequestedCallback PopupRequestedHandler;
	// An accepted popup request not fulfilled within this is refused. 0 means the default (5000 ms), negative never.
	int PopupRequestTimeoutMilliseconds;
	// Set both to make this window the popup a PopupRequestedCallback accepted: it shares the opener's WebView2
	// environment and becomes the page's new window (no StartUrl or StartString needed).
	Photino *PopupOpener;
	int PopupRequestId;
	// Called once when the window is destroyed, after its surface handles are closed. May be null.
	DestroyedCallback DestroyedHandler;

	int Size;
};
#endif

class Photino
{
private:
	WebMessageReceivedCallback _webMessageReceivedCallback;
	MovedCallback _movedCallback;
	ResizedCallback _resizedCallback;
	MaximizedCallback _maximizedCallback;
	RestoredCallback _restoredCallback;
	MinimizedCallback _minimizedCallback;
	ClosingCallback _closingCallback;
	FocusInCallback _focusInCallback;
	FocusOutCallback _focusOutCallback;
	std::vector<AutoString> _customSchemeNames;
	WebResourceRequestedCallback _customSchemeCallback;

	AutoString _startUrl;
	AutoString _startString;
	AutoString _temporaryFilesPath;
	AutoString _windowTitle;
	AutoString _iconFileName;
	AutoString _userAgent;
	AutoString _browserControlInitParameters;
	AutoString _notificationRegistrationId;

	bool _transparentEnabled;
	bool _devToolsEnabled;
	bool _grantBrowserPermissions;
	bool _mediaAutoplayEnabled;
	bool _fileSystemAccessEnabled;
	bool _webSecurityEnabled;
	bool _javascriptClipboardAccessEnabled;
	bool _mediaStreamEnabled;
	bool _smoothScrollingEnabled;
    bool _ignoreCertificateErrorsEnabled;
	bool _notificationsEnabled;

	int _zoom;

	Photino *_parent;
	PhotinoDialog *_dialog;
	void Show(bool isAlreadyShown);
#ifdef _WIN32
	static HINSTANCE _hInstance;
	HWND _hWnd;
	WinToastHandler *_toastHandler;
	wil::com_ptr<ICoreWebView2Environment> _webviewEnvironment;
	wil::com_ptr<ICoreWebView2> _webviewWindow;
	wil::com_ptr<ICoreWebView2Controller> _webviewController;
	bool EnsureWebViewIsInstalled();
	bool InstallWebView2();
	void AttachWebView();
	bool ToWide(PhotinoInitParams* params);

	// Windows hosting options (PhotinoWindowsOptions); every one off unless asked for.
	enum class InputTarget { None, Page, Host };
	struct HeldInput { UINT message; WPARAM wParam; LPARAM lParam; };
	struct PendingPopup
	{
		wil::com_ptr<ICoreWebView2NewWindowRequestedEventArgs> args;
		wil::com_ptr<ICoreWebView2Deferral> deferral;
	};
	bool _hasWindowsOptions = false;
	bool _startHidden = false;
	int _pendingShowCommand = 0;	// 0: none kept
	bool _deferredMaximize = false;
	int _popupRequestTimeoutMilliseconds = 0;
	bool _hostHadFocus = false;
	WPARAM _ownerMouseKeys = 0;
	POINT _ownerMousePoint = {};
	bool _compositionHosting = false;
	int _hitRegionHoldMilliseconds = 0;
	HostInputCallback _hostInputCallback = nullptr;
	PopupRequestedCallback _popupRequestedCallback = nullptr;
	DestroyedCallback _destroyedCallback = nullptr;
	HWND _popupOpenerHwnd = nullptr;
	int _popupRequestId = 0;
	int _nextPopupRequestId = 0;
	std::map<int, PendingPopup> _pendingPopups;
	wil::com_ptr<ICoreWebView2CompositionController> _compositionController;
	wil::com_ptr<IDCompositionDesktopDevice> _dcompDevice;
	wil::com_ptr<IDCompositionTarget> _dcompTarget;
	wil::com_ptr<IDCompositionVisual2> _dcompRootVisual;
	wil::com_ptr<IDCompositionVisual2> _dcompHostVisual;
	wil::com_ptr<IDCompositionVisual2> _dcompWebVisual;
	std::map<HANDLE, wil::com_ptr<IDCompositionVisual2>> _compositionSurfaces;
	std::vector<RECT> _hitRegions;
	bool _hitRegionsArePage = true;
	bool _hitRegionsStale = false;
	ULONGLONG _hitRegionsStaleSince = 0;
	std::vector<HeldInput> _heldInput;
	InputTarget _inputOwner = InputTarget::None;
	InputTarget _hoverTarget = InputTarget::None;
	bool _trackingMouseLeave = false;
	SIZE _lastClientSize = {};
	void CreateWebViewController(ICoreWebView2Environment *env);
	HRESULT OnWebViewControllerCreated(HRESULT result, ICoreWebView2Controller *controller);
	bool CreateCompositionTree();
	HRESULT AttachCompositionController();
	void EndMouseOwner();
	void HandleMouseLeave();
	UINT PlacementFlags();
	bool ShowStateWithoutActivating(int command);
	void LeaveIfCursorOutside();
	bool TakeHitRegionMessage(LPCWSTR message);
	void MarkHitRegionsStale();
	void FlushHeldInput();
	InputTarget TargetAt(POINT point);
	void RouteMouse(UINT message, WPARAM wParam, LPARAM lParam);
	bool SendHostInput(UINT message, POINT point, WPARAM wParam, LPARAM lParam);
	bool FinishPopupRequest(int requestId, ICoreWebView2 *newWindow);

#elif __linux__
	// GtkWidget* _window;
	GtkWidget *_webview;
	GdkGeometry _hints;
	void AddCustomSchemeHandlers();
	bool _isFullScreen;
#elif __APPLE__
	NSWindow *_window;
	WKWebView *_webview;
	WKWebViewConfiguration *_webviewConfiguration;
	std::vector<Monitor *> GetMonitors();
	
	bool _chromeless;

	int _preMaximizedWidth;
	int _preMaximizedHeight;
	int _preMaximizedXPosition;
	int _preMaximizedYPosition;

	void AttachWebView();
	void AddCustomScheme(AutoString scheme, WebResourceRequestedCallback requestHandler);

	void SetUserAgent(AutoString userAgent);

	void SetPreference(NSString *key, NSNumber *value);
	// void SetPreference(NSString *key, NSUInteger value);
	// void SetPreference(NSString *key, double value);
	void SetPreference(NSString *key, NSString *value);
	// void SetPreference(NSString *key, _WKEditableLinkBehavior value);
	// void SetPreference(NSString *key, _WKJavaScriptRuntimeFlags value);
	// void SetPreference(NSString *key, _WKPitchCorrectionAlgorithm value);
	// void SetPreference(NSString *key, _WKStorageBlockingPolicy value);
	// void SetPreference(NSString *key, _WKDebugOverlayRegions value);
#endif

public:
	bool _contextMenuEnabled;

#ifdef _WIN32
	static void Register(HINSTANCE hInstance);
	static void SetWebView2RuntimePath(AutoString pathToWebView2);
	HWND getHwnd();
	void RefitContent();
	void FocusWebView2();
	void NotifyWebView2WindowMove();
	void GetNotificationsEnabled(bool* enabled);
	AutoString ToUTF16String(AutoString source);
	AutoString ToUTF8String(AutoString source);
	Photino(PhotinoInitParams *initParams, const PhotinoWindowsOptions *options);
	bool HandleHostingMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT *result);
	void ShowHidden();
	void RefusePopupRequest(int requestId);
	void OnWindowDestroyed();
	void GetCompositionHostingEnabled(bool *enabled);
	HANDLE AddCompositionSurface(int x, int y, int width, int height);
	void SetCompositionSurfaceBounds(const PhotinoSurfaceBounds *bounds, int count);
	void RemoveCompositionSurface(HANDLE surface);
	void SetHitRegions(const int *rects, int count, bool regionsArePage);
	int _minWidth;
	int _minHeight;
	int _maxWidth;
	int _maxHeight;
#elif __linux__
	void set_webkit_settings();
	void set_webkit_customsettings(WebKitSettings* settings);
	GtkWidget *_window;
	int _lastHeight;
	int _lastWidth;
	int _lastTop;
	int _lastLeft;
	int _minWidth;
	int _minHeight;
	int _maxWidth;
	int _maxHeight;
#elif __APPLE__
	static void Register();
#endif

	Photino(PhotinoInitParams *initParams);
	~Photino();

	PhotinoDialog *GetDialog() const { return _dialog; };

	void Center();
	void ClearBrowserAutoFill();
	void Close();

	void GetTransparentEnabled(bool *enabled);
	void GetContextMenuEnabled(bool *enabled);
	void GetDevToolsEnabled(bool *enabled);
	void GetFullScreen(bool *fullScreen);
	void GetGrantBrowserPermissions(bool *grant);
	AutoString GetUserAgent();
	void GetMediaAutoplayEnabled(bool* enabled);
	void GetFileSystemAccessEnabled(bool* enabled);
	void GetWebSecurityEnabled(bool* enabled);
	void GetJavascriptClipboardAccessEnabled(bool* enabled);
	void GetMediaStreamEnabled(bool* enabled);
	void GetSmoothScrollingEnabled(bool* enabled);
	AutoString GetIconFileName();
	void GetMaximized(bool *isMaximized);
	void GetMinimized(bool *isMinimized);
	void GetPosition(int *x, int *y);
	void GetResizable(bool *resizable);
	unsigned int GetScreenDpi();
	void GetSize(int *width, int *height);
	AutoString GetTitle();
	void GetTopmost(bool *topmost);
	void GetZoom(int *zoom);
	void GetIgnoreCertificateErrorsEnabled(bool* enabled);

	void NavigateToString(AutoString content);
	void NavigateToUrl(AutoString url);
	void Restore(); // required anymore?backward compat?
	void SendWebMessage(AutoString message);

	void SetTransparentEnabled(bool enabled);
	void SetContextMenuEnabled(bool enabled);
	void SetDevToolsEnabled(bool enabled);
	void SetIconFile(AutoString filename);
	void SetFullScreen(bool fullScreen);
	void SetMaximized(bool maximized);
	void SetMaxSize(int width, int height);
	void SetMinimized(bool minimized);
	void SetMinSize(int width, int height);
	void SetPosition(int x, int y);
	void SetResizable(bool resizable);
	void SetSize(int width, int height);
	void SetTitle(AutoString title);
	void SetTopmost(bool topmost);
	void SetZoom(int zoom);

	void ShowNotification(AutoString title, AutoString message);
	void WaitForExit();

	// Callbacks
	void AddCustomSchemeName(AutoString scheme) { _customSchemeNames.push_back((AutoString)scheme); };
	void GetAllMonitors(GetAllMonitorsCallback callback);
	void SetClosingCallback(ClosingCallback callback) { _closingCallback = callback; }
	void SetFocusInCallback(FocusInCallback callback) { _focusInCallback = callback; }
	void SetFocusOutCallback(FocusOutCallback callback) { _focusOutCallback = callback; }
	void SetMovedCallback(MovedCallback callback) { _movedCallback = callback; }
	void SetResizedCallback(ResizedCallback callback) { _resizedCallback = callback; }
	void SetMaximizedCallback(MaximizedCallback callback) { _maximizedCallback = callback; }
	void SetRestoredCallback(RestoredCallback callback) { _restoredCallback = callback; }
	void SetMinimizedCallback(MinimizedCallback callback) { _minimizedCallback = callback; }

	void Invoke(ACTION callback);
	bool InvokeClose()
	{
		if (_closingCallback)
			return _closingCallback();
		else
			return false;
	}
	void InvokeFocusIn()
	{
		if (_focusInCallback)
			return _focusInCallback();
	}
	void InvokeFocusOut()
	{
		if (_focusOutCallback)
			return _focusOutCallback();
	}
	void InvokeMove(int x, int y)
	{
		if (_movedCallback)
			_movedCallback(x, y);
	}
	void InvokeResize(int width, int height)
	{
		if (_resizedCallback)
			_resizedCallback(width, height);
	}
	void InvokeMaximized()
	{
		if (_maximizedCallback)
			return _maximizedCallback();
	}
	void InvokeRestored()
	{
		if (_restoredCallback)
			return _restoredCallback();
	}
	void InvokeMinimized()
	{
		if (_minimizedCallback)
			return _minimizedCallback();
	}
};
