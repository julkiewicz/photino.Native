// Windows hosting options: the composition mode (WebView2's composition controller in a DirectComposition tree, the
// host's surfaces under a transparent page, mouse input routed by the page's hit regions) and popup hosting. All of
// it acts only for a window created with PhotinoWindowsOptions asking for it (Photino_ctorWithOptions_win32).

#include "Photino.h"

#include <windowsx.h>

#include <algorithm>

#pragma comment(lib, "dcomp.lib")

#define HIT_REGION_HOLD_TIMER_ID 0x50484F4C

static bool IsMousePress(UINT message)
{
	return message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN
		|| message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDBLCLK || message == WM_MBUTTONDBLCLK || message == WM_XBUTTONDBLCLK;
}

static bool IsMouseRelease(UINT message)
{
	return message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP || message == WM_XBUTTONUP;
}

static bool IsKeyMessage(UINT message)
{
	return message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYDOWN || message == WM_SYSKEYUP
		|| message == WM_CHAR || message == WM_SYSCHAR;
}

bool Photino::CreateCompositionTree()
{
	// One tree per window: a root visual holding the host's visual (the surfaces the host fills) and, above it, the
	// page's visual (the composition controller's root visual target, attached when the controller exists).
	HRESULT hr = DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(&_dcompDevice));
	if (SUCCEEDED(hr)) hr = _dcompDevice->CreateTargetForHwnd(_hWnd, TRUE, &_dcompTarget);
	if (SUCCEEDED(hr)) hr = _dcompDevice->CreateVisual(&_dcompRootVisual);
	if (SUCCEEDED(hr)) hr = _dcompDevice->CreateVisual(&_dcompHostVisual);
	if (SUCCEEDED(hr)) hr = _dcompDevice->CreateVisual(&_dcompWebVisual);
	if (SUCCEEDED(hr)) hr = _dcompRootVisual->AddVisual(_dcompHostVisual.get(), FALSE, nullptr);
	if (SUCCEEDED(hr)) hr = _dcompRootVisual->AddVisual(_dcompWebVisual.get(), TRUE, _dcompHostVisual.get());
	if (SUCCEEDED(hr)) hr = _dcompTarget->SetRoot(_dcompRootVisual.get());
	if (SUCCEEDED(hr)) hr = _dcompDevice->Commit();
	if (FAILED(hr))
	{
		// For example a layered (Transparent) window, which DirectComposition cannot target.
		_dcompWebVisual.reset();
		_dcompHostVisual.reset();
		_dcompRootVisual.reset();
		_dcompTarget.reset();
		_dcompDevice.reset();
		return false;
	}

	RECT client;
	GetClientRect(_hWnd, &client);
	_lastClientSize = { client.right - client.left, client.bottom - client.top };
	return true;
}

HRESULT Photino::AttachCompositionController()
{
	if (!_dcompDevice)
		return E_FAIL;
	HRESULT hr = _compositionController->put_RootVisualTarget(_dcompWebVisual.get());
	if (SUCCEEDED(hr)) hr = _dcompDevice->Commit();
	MarkHitRegionsStale();	// until the page publishes its first regions
	if (SUCCEEDED(hr))
		RegisterDropTarget();
	return hr;
}

// ---- drag and drop ----

// The window's OLE drop target: every drag over the window comes here, on the window's thread.
class PhotinoDropTarget : public IDropTarget
{
public:
	explicit PhotinoDropTarget(Photino* owner) : _owner(owner) {}
	void Detach() { _owner = nullptr; }

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
	{
		if (riid == IID_IUnknown || riid == IID_IDropTarget)
		{
			*object = static_cast<IDropTarget*>(this);
			AddRef();
			return S_OK;
		}
		*object = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&_references); }
	ULONG STDMETHODCALLTYPE Release() override
	{
		ULONG left = InterlockedDecrement(&_references);
		if (left == 0)
			delete this;
		return left;
	}

	HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keyState, POINTL point, DWORD* effect) override
	{
		if (!_owner) { *effect = DROPEFFECT_NONE; return S_OK; }
		return _owner->OnDragEnter(data, keyState, point, effect);
	}
	HRESULT STDMETHODCALLTYPE DragOver(DWORD keyState, POINTL point, DWORD* effect) override
	{
		if (!_owner) { *effect = DROPEFFECT_NONE; return S_OK; }
		return _owner->OnDragOver(keyState, point, effect);
	}
	HRESULT STDMETHODCALLTYPE DragLeave() override
	{
		return _owner ? _owner->OnDragLeave() : S_OK;
	}
	HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keyState, POINTL point, DWORD* effect) override
	{
		if (!_owner) { *effect = DROPEFFECT_NONE; return S_OK; }
		return _owner->OnDrop(data, keyState, point, effect);
	}

private:
	Photino* _owner;
	LONG _references = 1;
};

void Photino::RegisterDropTarget()
{
	// WebView2's composition controller takes drags only from its host: the window must be the drop target.
	if (!_oleInitialized || _dropTarget || !_compositionController)
		return;
	if (FAILED(_compositionController->QueryInterface(IID_PPV_ARGS(&_dragController))))
		return;
	wil::com_ptr<IDropTarget> target;
	target.attach(new PhotinoDropTarget(this));
	if (FAILED(RegisterDragDrop(_hWnd, target.get())))
	{
		static_cast<PhotinoDropTarget*>(target.get())->Detach();
		_dragController.reset();
		return;
	}
	_dropTarget = target;
	// The shell's drag image (a file from Explorer shows its icon over the window); optional.
	CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&_dropHelper));
}

static std::wstring FormatsOf(IDataObject* data)
{
	std::wstring formats;
	wil::com_ptr<IEnumFORMATETC> formatList;
	if (data == nullptr || FAILED(data->EnumFormatEtc(DATADIR_GET, &formatList)) || !formatList)
		return formats;
	static const struct { CLIPFORMAT format; const wchar_t* name; } standard[] = {
		{ CF_TEXT, L"CF_TEXT" }, { CF_BITMAP, L"CF_BITMAP" }, { CF_OEMTEXT, L"CF_OEMTEXT" }, { CF_DIB, L"CF_DIB" },
		{ CF_UNICODETEXT, L"CF_UNICODETEXT" }, { CF_ENHMETAFILE, L"CF_ENHMETAFILE" }, { CF_HDROP, L"CF_HDROP" },
		{ CF_LOCALE, L"CF_LOCALE" }, { CF_DIBV5, L"CF_DIBV5" } };
	std::vector<CLIPFORMAT> seen;
	FORMATETC each;
	while (formatList->Next(1, &each, nullptr) == S_OK)
	{
		if (each.ptd != nullptr)
			CoTaskMemFree(each.ptd);
		if (std::find(seen.begin(), seen.end(), each.cfFormat) != seen.end())
			continue;
		seen.push_back(each.cfFormat);
		std::wstring name;
		for (const auto& known : standard)
			if (known.format == each.cfFormat)
				name = known.name;
		if (name.empty())
		{
			wchar_t registered[256];
			int length = GetClipboardFormatNameW(each.cfFormat, registered, _countof(registered));
			name = length > 0 ? std::wstring(registered, length) : L"#" + std::to_wstring(each.cfFormat);
		}
		if (!formats.empty())
			formats += L"\n";
		formats += name;
	}
	return formats;
}

static bool FilesOf(IDataObject* data, std::wstring* files)
{
	FORMATETC format = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
	STGMEDIUM medium = {};
	if (FAILED(data->GetData(&format, &medium)))
		return false;
	HDROP drop = (HDROP)GlobalLock(medium.hGlobal);
	if (drop != nullptr)
	{
		UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
		for (UINT i = 0; i < count; ++i)
		{
			UINT length = DragQueryFileW(drop, i, nullptr, 0);
			std::vector<wchar_t> path(length + 1);
			DragQueryFileW(drop, i, path.data(), length + 1);
			if (!files->empty())
				*files += L"\n";
			files->append(path.data(), length);
		}
		GlobalUnlock(medium.hGlobal);
	}
	ReleaseStgMedium(&medium);
	return drop != nullptr;
}

static bool TextOf(IDataObject* data, std::wstring* text)
{
	FORMATETC format = { CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
	STGMEDIUM medium = {};
	if (FAILED(data->GetData(&format, &medium)))
		return false;
	const wchar_t* chars = (const wchar_t*)GlobalLock(medium.hGlobal);
	if (chars != nullptr)
	{
		size_t limit = GlobalSize(medium.hGlobal) / sizeof(wchar_t);
		*text = std::wstring(chars, wcsnlen(chars, limit));
		GlobalUnlock(medium.hGlobal);
	}
	ReleaseStgMedium(&medium);
	return chars != nullptr;
}

DWORD Photino::SendHostDrop(int kind, POINT point, DWORD keyState, DWORD allowed, IDataObject* data)
{
	std::wstring files, text;
	bool hasFiles = false, hasText = false;
	if (kind == PHOTINO_HOST_DROP_DROP && data != nullptr)
	{
		hasFiles = FilesOf(data, &files);
		hasText = TextOf(data, &text);
	}
	PhotinoHostDrop drop = { kind, point.x, point.y, keyState, allowed,
		kind == PHOTINO_HOST_DROP_LEAVE ? nullptr : (AutoString)_dragFormats.c_str(),
		hasFiles ? (AutoString)files.c_str() : nullptr, hasText ? (AutoString)text.c_str() : nullptr };
	return _hostDropCallback(&drop);
}

void Photino::RouteDrag(DWORD keyState, POINT point, DWORD* effect, bool dropping)
{
	// By the hit regions, as the mouse: over the host's area the host is asked first (once per stay there) and may pass
	// the drag to the page. Without a host drop handler the page has the whole window.
	DWORD allowed = *effect;
	bool overHostArea = _hostDropCallback != nullptr && TargetAt(point) == InputTarget::Host;
	InputTarget side = _dragSide;
	if (overHostArea && !_dragOverHostArea)
	{
		_dragOverHostArea = true;
		DWORD answer = SendHostDrop(PHOTINO_HOST_DROP_ENTER, point, keyState, allowed, _dragData.get());
		if (answer != PHOTINO_HOST_DROP_TO_PAGE)
		{
			if (side == InputTarget::Page)
				_dragController->DragLeave();
			_dragSide = InputTarget::Host;
			*effect = answer & allowed;
			return;
		}
		side = InputTarget::Page;	// passed to the page (it may have it already)
	}
	else if (!overHostArea && _dragOverHostArea)
	{
		_dragOverHostArea = false;
		if (side == InputTarget::Host)
			SendHostDrop(PHOTINO_HOST_DROP_LEAVE, point, keyState, allowed, nullptr);
		side = InputTarget::Page;
	}
	else if (side == InputTarget::None)
	{
		side = InputTarget::Page;	// the drag's first event, outside the host's area
	}

	if (side == InputTarget::Host)
	{
		*effect = dropping ? allowed : SendHostDrop(PHOTINO_HOST_DROP_OVER, point, keyState, allowed, nullptr) & allowed;
		return;
	}
	if (_dragSide != InputTarget::Page)
	{
		_dragSide = InputTarget::Page;
		_dragController->DragEnter(_dragData.get(), keyState, point, effect);
	}
	else if (!dropping)
	{
		_dragController->DragOver(keyState, point, effect);
	}
}

HRESULT Photino::OnDragEnter(IDataObject* data, DWORD keyState, POINTL screen, DWORD* effect)
{
	// A press the page still holds started this drag (WebView2 runs the page's drags itself): the drag carries the
	// button now, and its end reaches the page through the drop, so no release is owed to the page.
	if (_inputOwner == InputTarget::Page)
	{
		_inputOwner = InputTarget::None;
		_ownerMouseKeys = 0;
		if (GetCapture() == _hWnd)
			ReleaseCapture();
	}
	EndDrag();
	_dragData = data;
	_dragFormats = FormatsOf(data);
	POINT point = { screen.x, screen.y };
	if (_dropHelper)
		_dropHelper->DragEnter(_hWnd, data, &point, *effect);
	ScreenToClient(_hWnd, &point);
	RouteDrag(keyState, point, effect, false);
	return S_OK;
}

HRESULT Photino::OnDragOver(DWORD keyState, POINTL screen, DWORD* effect)
{
	POINT point = { screen.x, screen.y };
	if (_dropHelper)
		_dropHelper->DragOver(&point, *effect);
	ScreenToClient(_hWnd, &point);
	if (!_dragData)
	{
		*effect = DROPEFFECT_NONE;
		return S_OK;
	}
	RouteDrag(keyState, point, effect, false);
	return S_OK;
}

HRESULT Photino::OnDragLeave()
{
	if (_dropHelper)
		_dropHelper->DragLeave();
	EndDrag();
	return S_OK;
}

HRESULT Photino::OnDrop(IDataObject* data, DWORD keyState, POINTL screen, DWORD* effect)
{
	POINT point = { screen.x, screen.y };
	if (_dropHelper)
		_dropHelper->Drop(data, &point, *effect);
	ScreenToClient(_hWnd, &point);
	if (!_dragData)
		_dragData = data;
	DWORD allowed = *effect;
	RouteDrag(keyState, point, effect, true);
	if (_dragSide == InputTarget::Host)
		*effect = SendHostDrop(PHOTINO_HOST_DROP_DROP, point, keyState, allowed, data) & allowed;
	else
	{
		*effect = allowed;
		_dragController->Drop(data, keyState, point, effect);
	}
	_dragSide = InputTarget::None;	// dropped: no leave follows
	EndDrag();
	return S_OK;
}

void Photino::EndDrag()
{
	// The drag left the window (or a new one starts): the side that has it gets its leave.
	if (_dragSide == InputTarget::Page && _dragController)
		_dragController->DragLeave();
	else if (_dragSide == InputTarget::Host)
		SendHostDrop(PHOTINO_HOST_DROP_LEAVE, { 0, 0 }, 0, 0, nullptr);
	_dragSide = InputTarget::None;
	_dragOverHostArea = false;
	_dragData.reset();
	_dragFormats.clear();
}

bool Photino::CaptureTakenByPage(HWND window)
{
	// WebView2's browser process runs the page's own drags and dropdowns, taking the mouse from the window (their
	// windows share its input). The page then has the mouse itself; it is owed no release.
	if (window == nullptr || !_webviewWindow)
		return false;
	UINT32 browser = 0;
	DWORD owner = 0;
	GetWindowThreadProcessId(window, &owner);
	return SUCCEEDED(_webviewWindow->get_BrowserProcessId(&browser)) && browser != 0 && owner == browser;
}

UINT Photino::PlacementFlags()
{
	// A window made with the Windows options is never activated by its own placement calls.
	return _hasWindowsOptions ? SWP_NOACTIVATE : 0;
}

bool Photino::ShowStateWithoutActivating(int command)
{
	// Minimize, maximize and restore of a window made with the Windows options; false leaves the usual call to run
	// (a window without the options, or one already in front, where activating changes nothing).
	if (!_hasWindowsOptions)
		return false;
	if (!IsWindowVisible(_hWnd))
	{
		_pendingShowCommand = command == SW_MINIMIZE ? SW_SHOWMINNOACTIVE : command == SW_MAXIMIZE ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
		return true;
	}
	if (GetForegroundWindow() == _hWnd)
		return false;
	if (command == SW_MINIMIZE)
		ShowWindow(_hWnd, SW_SHOWMINNOACTIVE);
	else if (command == SW_MAXIMIZE)
		_deferredMaximize = true;	// Windows has no non-activating maximize: done on the next activation
	else
		ShowWindow(_hWnd, SW_SHOWNOACTIVATE);
	return true;
}

void Photino::ShowHidden()
{
	_startHidden = false;
	ShowWindow(_hWnd, _pendingShowCommand != 0 ? _pendingShowCommand : SW_SHOW);
}

void Photino::LeaveIfCursorOutside()
{
	// A drag that ended outside the window: no leave will come from tracking (it is armed over the window only).
	POINT cursor;
	if (_inputOwner == InputTarget::None && GetCursorPos(&cursor) && WindowFromPoint(cursor) != _hWnd)
		HandleMouseLeave();
}

void Photino::RefusePopupRequest(int requestId)
{
	FinishPopupRequest(requestId, nullptr);
}

void Photino::GetCompositionHostingEnabled(bool* enabled)
{
	*enabled = _compositionController != nullptr && _dcompDevice != nullptr;
}

HANDLE Photino::AddCompositionSurface(int x, int y, int width, int height)
{
	// The host gives the handle to its renderer, which makes its swap chain for it
	// (IDXGIFactoryMedia::CreateSwapChainForCompositionSurfaceHandle) on any thread. Remove the surface only after
	// that swap chain is released.
	if (!_dcompDevice)
		return nullptr;

	HANDLE surfaceHandle = nullptr;
	if (FAILED(DCompositionCreateSurfaceHandle(COMPOSITIONOBJECT_ALL_ACCESS, nullptr, &surfaceHandle)))
		return nullptr;

	wil::com_ptr<IUnknown> surface;
	wil::com_ptr<IDCompositionVisual2> visual;
	HRESULT hr = _dcompDevice->CreateSurfaceFromHandle(surfaceHandle, &surface);
	if (SUCCEEDED(hr)) hr = _dcompDevice->CreateVisual(&visual);
	if (SUCCEEDED(hr)) hr = visual->SetContent(surface.get());
	if (SUCCEEDED(hr)) hr = _dcompHostVisual->AddVisual(visual.get(), TRUE, nullptr);
	if (FAILED(hr))
	{
		CloseHandle(surfaceHandle);
		return nullptr;
	}

	_compositionSurfaces[surfaceHandle] = visual;
	PhotinoSurfaceBounds bounds = { surfaceHandle, x, y, width, height };
	SetCompositionSurfaceBounds(&bounds, 1);
	return surfaceHandle;
}

void Photino::SetCompositionSurfaceBounds(const PhotinoSurfaceBounds* bounds, int count)
{
	if (!_dcompDevice)
		return;

	for (int i = 0; i < count; ++i)
	{
		auto found = _compositionSurfaces.find(bounds[i].Surface);
		if (found == _compositionSurfaces.end())
			continue;
		IDCompositionVisual2* visual = found->second.get();
		visual->SetOffsetX((float)bounds[i].X);
		visual->SetOffsetY((float)bounds[i].Y);
		// Clip to the rectangle, so a swap chain not yet resized never draws outside it.
		D2D_RECT_F clip = { 0.0f, 0.0f, (float)bounds[i].Width, (float)bounds[i].Height };
		visual->SetClip(clip);
	}
	_dcompDevice->Commit();	// one commit for the batch
}

void Photino::RemoveCompositionSurface(HANDLE surface)
{
	auto found = _compositionSurfaces.find(surface);
	if (found == _compositionSurfaces.end())
		return;
	_dcompHostVisual->RemoveVisual(found->second.get());
	_compositionSurfaces.erase(found);
	_dcompDevice->Commit();
	CloseHandle(surface);
}

void Photino::SetHitRegions(const int* rects, int count, bool regionsArePage)
{
	_hitRegions.clear();
	for (int i = 0; i < count; ++i)
	{
		const int* r = rects + i * 4;
		_hitRegions.push_back({ r[0], r[1], r[0] + r[2], r[1] + r[3] });
	}
	_hitRegionsArePage = regionsArePage;

	if (_hitRegionsStale)
	{
		_hitRegionsStale = false;
		FlushHeldInput();
	}
}

bool Photino::TakeHitRegionMessage(LPCWSTR message)
{
	static const wchar_t prefix[] = L"__photino_hit_regions__:";
	const size_t prefixLength = _countof(prefix) - 1;
	if (message == nullptr || wcsncmp(message, prefix, prefixLength) != 0)
		return false;

	// "<page|host>:x,y,w,h;x,y,w,h" in client pixels (the page's script scales by devicePixelRatio).
	const wchar_t* cursor = message + prefixLength;
	bool regionsArePage = wcsncmp(cursor, L"host:", 5) != 0;
	cursor = wcschr(cursor, L':');
	std::vector<int> values;
	if (cursor != nullptr)
	{
		++cursor;
		while (*cursor != 0)
		{
			wchar_t* end = nullptr;
			long value = wcstol(cursor, &end, 10);
			if (end == cursor)
				break;
			values.push_back((int)value);
			cursor = end;
			if (*cursor == L',' || *cursor == L';')
				++cursor;
		}
	}
	SetHitRegions(values.data(), (int)(values.size() / 4), regionsArePage);
	return true;
}

void Photino::MarkHitRegionsStale()
{
	// Decision: a hit region trails a layout change by the page's publish (up to about two frames). For the layout
	// changes the window itself causes (its size, its DPI, its first layout), a press waits until the page publishes
	// again, or HitRegionHoldMilliseconds at most, then everything held is routed in order by the new region. A layout
	// change the page makes on its own is published from its own layout pass; a press in that gap routes by the
	// previous region.
	if (_hitRegionHoldMilliseconds <= 0)
		return;
	_hitRegionsStale = true;
	_hitRegionsStaleSince = GetTickCount64();
}

static int ButtonOf(UINT message, WPARAM wParam)
{
	switch (message)
	{
	case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: return VK_LBUTTON;
	case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK: return VK_RBUTTON;
	case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK: return VK_MBUTTON;
	default: return GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
	}
}

static int PhysicalButton(int logicalButton)
{
	// GetAsyncKeyState reads the physical buttons; a window message names the logical one.
	if (GetSystemMetrics(SM_SWAPBUTTON))
	{
		if (logicalButton == VK_LBUTTON) return VK_RBUTTON;
		if (logicalButton == VK_RBUTTON) return VK_LBUTTON;
	}
	return logicalButton;
}

void Photino::FlushHeldInput()
{
	KillTimer(_hWnd, HIT_REGION_HOLD_TIMER_ID);
	std::vector<HeldInput> held;
	held.swap(_heldInput);
	bool foreground = GetForegroundWindow() == _hWnd;
	for (size_t i = 0; i < held.size(); ++i)
	{
		const HeldInput& input = held[i];
		if (input.message == WM_MOUSELEAVE)
		{
			HandleMouseLeave();
			continue;
		}
		if (foreground && IsMousePress(input.message))
		{
			// A real press whose button was released outside the window while it was held: its release never came,
			// so routing the press would leave that side with a button down.
			int button = ButtonOf(input.message, input.wParam);
			bool released = false;
			for (size_t j = i + 1; j < held.size() && !released; ++j)
				released = IsMouseRelease(held[j].message) && ButtonOf(held[j].message, held[j].wParam) == button;
			if (!released && (GetAsyncKeyState(PhysicalButton(button)) & 0x8000) == 0)
				continue;
		}
		RouteMouse(input.message, input.wParam, input.lParam);
	}
	if (_inputOwner == InputTarget::None && GetCapture() == _hWnd)
		ReleaseCapture();
}

void Photino::EndMouseOwner()
{
	// The side that owned a drag loses it (capture taken elsewhere, the window deactivated): tell it, so no button
	// stays down there.
	InputTarget owner = _inputOwner;
	_inputOwner = InputTarget::None;
	if (owner == InputTarget::Page)
	{
		const struct { WPARAM key; UINT up; } buttons[] = {
			{ MK_LBUTTON, WM_LBUTTONUP }, { MK_RBUTTON, WM_RBUTTONUP }, { MK_MBUTTON, WM_MBUTTONUP },
			{ MK_XBUTTON1, WM_XBUTTONUP }, { MK_XBUTTON2, WM_XBUTTONUP } };
		WPARAM keys = _ownerMouseKeys;
		for (const auto& button : buttons)
		{
			if ((keys & button.key) == 0)
				continue;
			keys &= ~button.key;
			UINT32 data = button.key == MK_XBUTTON1 ? XBUTTON1 : button.key == MK_XBUTTON2 ? XBUTTON2 : 0;
			_compositionController->SendMouseInput((COREWEBVIEW2_MOUSE_EVENT_KIND)button.up,
				(COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS)keys, data, _ownerMousePoint);
		}
		_compositionController->SendMouseInput(COREWEBVIEW2_MOUSE_EVENT_KIND_LEAVE, COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE, 0, { 0, 0 });
		_hoverTarget = InputTarget::None;
	}
	else if (owner == InputTarget::Host)
	{
		SendHostInput(WM_CAPTURECHANGED, _ownerMousePoint, 0, 0);
	}
	_ownerMouseKeys = 0;
	if (GetCapture() == _hWnd)
		ReleaseCapture();
	LeaveIfCursorOutside();
}

void Photino::HandleMouseLeave()
{
	_trackingMouseLeave = false;
	if (_inputOwner != InputTarget::None)
		return;
	if (_hoverTarget == InputTarget::Page)
		_compositionController->SendMouseInput(COREWEBVIEW2_MOUSE_EVENT_KIND_LEAVE, COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE, 0, { 0, 0 });
	else if (_hoverTarget == InputTarget::Host)
		SendHostInput(WM_MOUSELEAVE, { 0, 0 }, 0, 0);
	_hoverTarget = InputTarget::None;
}

Photino::InputTarget Photino::TargetAt(POINT point)
{
	bool inside = false;
	for (const RECT& rect : _hitRegions)
	{
		if (PtInRect(&rect, point))
		{
			inside = true;
			break;
		}
	}
	return inside == _hitRegionsArePage ? InputTarget::Page : InputTarget::Host;
}

bool Photino::SendHostInput(UINT message, POINT point, WPARAM wParam, LPARAM lParam)
{
	if (_hostInputCallback == nullptr)
		return false;
	PhotinoHostInput input = { message, point.x, point.y, (unsigned long long)wParam, (long long)lParam };
	return _hostInputCallback(&input);
}

void Photino::RouteMouse(UINT message, WPARAM wParam, LPARAM lParam)
{
	POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
	if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)
		ScreenToClient(_hWnd, &point);

	// Track the leave only while the real cursor is over the window: TrackMouseEvent posts a leave at once when it
	// is not (a wheel sent to the focus window from elsewhere, or a message posted to the window).
	POINT cursor;
	if (!_trackingMouseLeave && GetCursorPos(&cursor) && WindowFromPoint(cursor) == _hWnd)
	{
		TRACKMOUSEEVENT track = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, _hWnd, 0 };
		_trackingMouseLeave = TrackMouseEvent(&track) != FALSE;
	}

	// The side a press lands on keeps every mouse message until the last button is up, so a drag that crosses the
	// region's edge stays with the side it started on.
	InputTarget target = _inputOwner != InputTarget::None ? _inputOwner : TargetAt(point);
	if (_inputOwner == InputTarget::None)
	{
		if (_hoverTarget == InputTarget::Page && target == InputTarget::Host)
			_compositionController->SendMouseInput(COREWEBVIEW2_MOUSE_EVENT_KIND_LEAVE, COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE, 0, { 0, 0 });
		else if (_hoverTarget == InputTarget::Host && target == InputTarget::Page)
			SendHostInput(WM_MOUSELEAVE, point, 0, 0);
		_hoverTarget = target;

		if (IsMousePress(message))
		{
			_inputOwner = target;
			// Capture and focus only for a press on the foreground window (a real click): a message posted to a
			// window in the background never takes the user's mouse or keyboard.
			if (GetForegroundWindow() == _hWnd)
			{
				SetCapture(_hWnd);
				if (target == InputTarget::Page)
					_webviewController->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
				else
					SetFocus(_hWnd);
			}
		}
	}

	if (target == InputTarget::Page)
	{
		UINT32 mouseData = 0;
		if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)
			mouseData = (UINT32)(INT32)GET_WHEEL_DELTA_WPARAM(wParam);
		else if (message == WM_XBUTTONDOWN || message == WM_XBUTTONUP || message == WM_XBUTTONDBLCLK)
			mouseData = GET_XBUTTON_WPARAM(wParam);
		_compositionController->SendMouseInput((COREWEBVIEW2_MOUSE_EVENT_KIND)message,
			(COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS)GET_KEYSTATE_WPARAM(wParam), mouseData, point);
	}
	else
	{
		SendHostInput(message, point, wParam, lParam);
	}

	const WPARAM buttons = MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2;
	if (_inputOwner != InputTarget::None)
	{
		_ownerMouseKeys = GET_KEYSTATE_WPARAM(wParam) & buttons;
		_ownerMousePoint = point;
	}
	if (IsMouseRelease(message) && (GET_KEYSTATE_WPARAM(wParam) & buttons) == 0 && _inputOwner != InputTarget::None)
	{
		_inputOwner = InputTarget::None;
		if (GetCapture() == _hWnd)
			ReleaseCapture();
		LeaveIfCursorOutside();
	}
}

bool Photino::HandleHostingMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT* result)
{
	if (message == WM_TIMER && wParam > PHOTINO_POPUP_REQUEST_TIMER_BASE && wParam <= PHOTINO_POPUP_REQUEST_TIMER_BASE + 0xFFFFF)
	{
		// An accepted popup request the host did not fulfil in time: refuse it, so the page's script runs on.
		KillTimer(_hWnd, wParam);
		FinishPopupRequest((int)(wParam - PHOTINO_POPUP_REQUEST_TIMER_BASE), nullptr);
		*result = 0;
		return true;
	}

	if (message == WM_ACTIVATE && LOWORD(wParam) != WA_INACTIVE && _deferredMaximize)
	{
		_deferredMaximize = false;
		PostMessage(_hWnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0);	// now in front: maximizing changes no activation
	}

	if (!_compositionController || !_dcompDevice)
		return false;

	if (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST)
	{
		if (_dragSide != InputTarget::None)
		{
			// A drag is over the window: OLE's drag loop has the mouse and the drag's side gets it as drag events, as
			// with the usual hosting, where the page's window gets no mouse messages meanwhile.
			*result = 0;
			return true;
		}
		if (_hitRegionsStale && _inputOwner == InputTarget::None && (IsMousePress(message) || !_heldInput.empty()))
		{
			ULONGLONG waited = GetTickCount64() - _hitRegionsStaleSince;
			if (waited < (ULONGLONG)_hitRegionHoldMilliseconds)
			{
				if (_heldInput.empty())
				{
					SetTimer(_hWnd, HIT_REGION_HOLD_TIMER_ID, (UINT)(_hitRegionHoldMilliseconds - waited), nullptr);
					if (IsMousePress(message) && GetForegroundWindow() == _hWnd)
						SetCapture(_hWnd);	// so the release arrives even outside the window
				}
				_heldInput.push_back({ message, wParam, lParam });
				*result = 0;
				return true;
			}
			_hitRegionsStale = false;
			FlushHeldInput();
		}
		RouteMouse(message, wParam, lParam);
		*result = 0;
		return true;
	}

	switch (message)
	{
	case WM_MOUSELEAVE:
	{
		if (!_heldInput.empty())
			_heldInput.push_back({ message, wParam, lParam });	// after the held moves, in order
		else
			HandleMouseLeave();
		*result = 0;
		return true;
	}
	case WM_CAPTURECHANGED:
	{
		// Capture taken by another window (or released by the system) ends the drag on its side.
		if ((HWND)lParam != _hWnd && _inputOwner != InputTarget::None)
		{
			if (_inputOwner == InputTarget::Page && CaptureTakenByPage((HWND)lParam))
			{
				// The page took the mouse itself (its own drag and drop, a dropdown): it holds its own button state.
				_inputOwner = InputTarget::None;
				_ownerMouseKeys = 0;
			}
			else
				EndMouseOwner();
		}
		return false;
	}
	case WM_SETFOCUS:
	case WM_KILLFOCUS:
	{
		// The host learns when the keyboard comes to or leaves the window (to the page or another app), so it can
		// let go of keys it holds.
		SendHostInput(message, { 0, 0 }, wParam, lParam);
		return false;
	}
	case WM_ACTIVATE:
	{
		if (LOWORD(wParam) == WA_INACTIVE)
		{
			_hostHadFocus = GetFocus() == _hWnd;
			if (_inputOwner != InputTarget::None)
				EndMouseOwner();
			return false;
		}
		// Give the keyboard back to the side that had it, or for a click to the side clicked.
		bool host = _hostHadFocus;
		if (LOWORD(wParam) == WA_CLICKACTIVE)
		{
			POINT cursor;
			GetCursorPos(&cursor);
			ScreenToClient(_hWnd, &cursor);
			host = TargetAt(cursor) == InputTarget::Host;
		}
		if (!host)
			return false;	// the usual: the page takes the keyboard
		SetFocus(_hWnd);
		InvokeFocusIn();
		*result = 0;
		return true;
	}
	case WM_TIMER:
	{
		if (wParam != HIT_REGION_HOLD_TIMER_ID)
			return false;
		KillTimer(_hWnd, HIT_REGION_HOLD_TIMER_ID);
		// A late timer from an earlier hold must not end this one early.
		if (_hitRegionsStale && !_heldInput.empty())
		{
			ULONGLONG waited = GetTickCount64() - _hitRegionsStaleSince;
			if (waited >= (ULONGLONG)_hitRegionHoldMilliseconds)
			{
				_hitRegionsStale = false;
				FlushHeldInput();
			}
			else	// marked stale again during the hold (another resize): wait out the rest of the new hold
				SetTimer(_hWnd, HIT_REGION_HOLD_TIMER_ID, (UINT)(_hitRegionHoldMilliseconds - waited), nullptr);
		}
		*result = 0;
		return true;
	}
	case WM_SETCURSOR:
	{
		if (LOWORD(lParam) != HTCLIENT)
			return false;
		POINT cursor;
		GetCursorPos(&cursor);
		ScreenToClient(_hWnd, &cursor);
		InputTarget target = _inputOwner != InputTarget::None ? _inputOwner : TargetAt(cursor);
		if (target == InputTarget::Host)
		{
			// The host may set its own cursor (a crosshair, none while looking) and return true.
			if (!SendHostInput(message, cursor, wParam, lParam))
				return false;
			*result = TRUE;
			return true;
		}
		HCURSOR pageCursor = nullptr;
		if (FAILED(_compositionController->get_Cursor(&pageCursor)))
			return false;
		SetCursor(pageCursor);
		*result = TRUE;
		return true;
	}
	case WM_SIZE:
	{
		SIZE size = { LOWORD(lParam), HIWORD(lParam) };
		if (wParam != SIZE_MINIMIZED && (size.cx != _lastClientSize.cx || size.cy != _lastClientSize.cy))
		{
			_lastClientSize = size;
			MarkHitRegionsStale();
		}
		return false;
	}
	case WM_DPICHANGED:
	{
		MarkHitRegionsStale();
		return false;
	}
	case WM_MOVE:
	{
		_webviewController->NotifyParentWindowPositionChanged();
		return false;
	}
	default:
		// Keys reach the window only while it has focus (a press on the host's side took it from the page).
		if (IsKeyMessage(message) && SendHostInput(message, { 0, 0 }, wParam, lParam))
		{
			*result = 0;	// handled by the host: no window menu for Alt or F10
			return true;
		}
		return false;
	}
}

bool Photino::FinishPopupRequest(int requestId, ICoreWebView2* newWindow)
{
	auto found = _pendingPopups.find(requestId);
	if (found == _pendingPopups.end())
		return false;
	PendingPopup pending = found->second;
	_pendingPopups.erase(found);

	if (newWindow != nullptr)
		pending.args->put_NewWindow(newWindow);
	KillTimer(_hWnd, PHOTINO_POPUP_REQUEST_TIMER_BASE + requestId);
	pending.args->put_Handled(TRUE);	// handled with no new window: window.open returns null in the page
	if (pending.deferral)
	{
		if (SUCCEEDED(pending.deferral->Complete()))
			++_popupDeferralsCompleted;
		else
			++_popupDeferralsFailed;
	}
	return true;
}

void Photino::GetPopupDeferralCounts(int* taken, int* completed, int* failed)
{
	*taken = _popupDeferralsTaken;
	*completed = _popupDeferralsCompleted;
	*failed = _popupDeferralsFailed;
}

void Photino::OnWindowDestroyed()
{
	std::vector<int> pendingIds;
	for (auto& pending : _pendingPopups)
		pendingIds.push_back(pending.first);
	for (int requestId : pendingIds)
		FinishPopupRequest(requestId, nullptr);

	// Every window made with the options closes its controller here, so no WebView2 event reaches the host after
	// the destroyed callback.
	if (_dropTarget)
	{
		RevokeDragDrop(_hWnd);
		EndDrag();
		static_cast<PhotinoDropTarget*>(_dropTarget.get())->Detach();
		_dropTarget.reset();
		_dropHelper.reset();
		_dragController.reset();
	}
	if (_oleInitialized)
	{
		_oleInitialized = false;
		OleUninitialize();
	}
	if (_hasWindowsOptions || _compositionController || _popupOpenerHwnd != nullptr)
	{
		KillTimer(_hWnd, HIT_REGION_HOLD_TIMER_ID);
		_heldInput.clear();
		if (_webviewController)
			_webviewController->Close();	// the opener's page sees its popup closed
		for (auto& surface : _compositionSurfaces)
			CloseHandle(surface.first);
		_compositionSurfaces.clear();
		_dcompWebVisual.reset();
		_dcompHostVisual.reset();
		_dcompRootVisual.reset();
		_dcompTarget.reset();
		_dcompDevice.reset();
		_compositionController.reset();
	}

	if (_destroyedCallback != nullptr)
		_destroyedCallback();
}
