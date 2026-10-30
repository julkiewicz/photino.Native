#pragma once

#ifdef _WIN32
#include <Windows.h>
#include <wil/com.h>
#include <WebView2.h>
#include <dcomp.h>
#include <ShlObj.h>
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
// Called before the page, or a frame in it, navigates to uri (a link, a script setting location, a reload, a dropped
// file or link). Return false to cancel the navigation: the page stays as it is.
typedef bool (*NavigationStartingCallback)(AutoString uri);

// Composition mode: one drag and drop event over the host's area (outside the page's hit regions). Kind is one of
// PHOTINO_HOST_DROP_*. X and Y are in client pixels; KeyState holds the MK_* flags; AllowedEffects the DROPEFFECT_*
// flags the drag's source allows. Formats names the data's clipboard formats (standard ones as CF_*, registered ones
// by name), one per line, on every kind but leave. On a drop, Files holds the dropped files' paths (CF_HDROP) and Text
// the data's text (CF_UNICODETEXT), one per line, each null when the data has none. The strings live for the call only.
#define PHOTINO_HOST_DROP_ENTER 0
#define PHOTINO_HOST_DROP_OVER 1
#define PHOTINO_HOST_DROP_LEAVE 2
#define PHOTINO_HOST_DROP_DROP 3
struct PhotinoHostDrop
{
	int Kind;
	int X;
	int Y;
	unsigned int KeyState;
	unsigned int AllowedEffects;
	AutoString Formats;
	AutoString Files;
	AutoString Text;
};

// Returned for an enter: the host does not take this drag here; it goes to the page (as over the page's regions) until
// it leaves the host's area, and the host gets no over, leave or drop for it meanwhile.
#define PHOTINO_HOST_DROP_TO_PAGE 0xFFFFFFFFu
// Return the DROPEFFECT_* the host takes (DROPEFFECT_NONE refuses it there), masked by AllowedEffects; for an enter,
// PHOTINO_HOST_DROP_TO_PAGE passes the drag to the page instead (for any other kind it is only masked). The return of a
// leave is ignored. It is called inside OLE's call to the window: to close the window from it, post the close (closing
// it in the call ends the drag, which the host then hears as a leave).
typedef unsigned int (*HostDropCallback)(const PhotinoHostDrop *drop);

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
	// Called before every navigation of the page and of its frames, the first one included. Null keeps WebView2's
	// own behavior.
	NavigationStartingCallback NavigationStartingHandler;
	// Turn off the browser's own keys (reload, find, print, history, zoom and the like); the page still gets every
	// key. DevTools' keys follow the DevTools setting.
	bool BrowserAcceleratorKeysDisabled;
	// Composition mode: drags over the window (OLE drag and drop, from the page itself, another window or another app)
	// go to the page through the composition controller, as the usual hosting does; the window is their drop target,
	// with OLE initialized on its thread. With this handler, a drag over the host's area (outside the page's hit
	// regions) is offered to the host first (see HostDropCallback). Null: every drag goes to the page.
	HostDropCallback HostDropHandler;

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
	NavigationStartingCallback _navigationStartingCallback = nullptr;
	bool _browserAcceleratorKeysDisabled = false;
	// Popup deferrals taken (one per window.open request), completed, and completions WebView2 refused: a check of
	// their counts (Photino_GetPopupDeferralCounts_win32) sees each completed exactly once.
	int _popupDeferralsTaken = 0;
	int _popupDeferralsCompleted = 0;
	int _popupDeferralsFailed = 0;
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
	// Drag and drop in composition mode: the window's drop target forwards to the composition controller (the page)
	// or to the host, by the hit regions.
	friend class PhotinoDropTarget;
	HostDropCallback _hostDropCallback = nullptr;
	bool _oleInitialized = false;
	wil::com_ptr<ICoreWebView2CompositionController3> _dragController;
	wil::com_ptr<IDropTarget> _dropTarget;
	wil::com_ptr<IDropTargetHelper> _dropHelper;
	wil::com_ptr<IDataObject> _dragData;
	std::wstring _dragFormats;
	InputTarget _dragSide = InputTarget::None;	// the side the drag is with now
	bool _dragOverHostArea = false;	// over the host's area (the host asked once per stay)
	bool _dragStaleTimerArmed = false;
	void RegisterDropTarget();
	HRESULT OnDragEnter(IDataObject *data, DWORD keyState, POINTL screen, DWORD *effect);
	HRESULT OnDragOver(DWORD keyState, POINTL screen, DWORD *effect);
	HRESULT OnDragLeave();
	HRESULT OnDrop(IDataObject *data, DWORD keyState, POINTL screen, DWORD *effect);
	bool RouteDrag(DWORD keyState, POINT point, DWORD *effect, bool dropping);
	void NoteMouseDuringDrag(WPARAM wParam);
	void EndStaleDrag();
	DWORD SendHostDrop(int kind, POINT point, DWORD keyState, DWORD allowed, IDataObject *data);
	void EndDrag();
	bool CaptureTakenByPage(HWND window);
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
	void GetPopupDeferralCounts(int *taken, int *completed, int *failed);
	void OnWindowDestroyed();
	void GetCompositionHostingEnabled(bool *enabled);
	HANDLE AddCompositionSurface(int x, int y, int width, int height);
	void SetCompositionSurfaceBounds(const PhotinoSurfaceBounds *bounds, int count);
	void RemoveCompositionSurface(HANDLE surface);
	void SetHitRegions(const int *rects, int count, bool regionsArePage);
	IDropTarget *GetDropTarget() { return _dropTarget.get(); }
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
