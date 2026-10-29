// Windows hosting options: the composition mode (WebView2's composition controller in a DirectComposition tree, the
// host's surfaces under a transparent page, mouse input routed by the page's hit regions) and popup hosting. All of
// it acts only for a window created with PhotinoWindowsOptions asking for it (Photino_ctorWithOptions_win32).

#include "Photino.h"

#include <windowsx.h>

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

HRESULT Photino::BuildCompositionTree()
{
	// One tree per window: a root visual holding the host's visual (the surfaces the host fills) and, above it, the
	// page's visual (the composition controller's root visual target).
	HRESULT hr = DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(&_dcompDevice));
	if (SUCCEEDED(hr)) hr = _dcompDevice->CreateTargetForHwnd(_hWnd, TRUE, &_dcompTarget);
	if (SUCCEEDED(hr)) hr = _dcompDevice->CreateVisual(&_dcompRootVisual);
	if (SUCCEEDED(hr)) hr = _dcompDevice->CreateVisual(&_dcompHostVisual);
	if (SUCCEEDED(hr)) hr = _dcompDevice->CreateVisual(&_dcompWebVisual);
	if (SUCCEEDED(hr)) hr = _dcompRootVisual->AddVisual(_dcompHostVisual.get(), FALSE, nullptr);
	if (SUCCEEDED(hr)) hr = _dcompRootVisual->AddVisual(_dcompWebVisual.get(), TRUE, _dcompHostVisual.get());
	if (SUCCEEDED(hr)) hr = _dcompTarget->SetRoot(_dcompRootVisual.get());
	if (SUCCEEDED(hr)) hr = _compositionController->put_RootVisualTarget(_dcompWebVisual.get());
	if (SUCCEEDED(hr)) hr = _dcompDevice->Commit();
	if (FAILED(hr))
	{
		_dcompDevice.reset();
		return hr;
	}

	RECT client;
	GetClientRect(_hWnd, &client);
	_lastClientSize = { client.right - client.left, client.bottom - client.top };
	MarkHitRegionsStale();	// until the page publishes its first regions
	return hr;
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

void Photino::FlushHeldInput()
{
	KillTimer(_hWnd, HIT_REGION_HOLD_TIMER_ID);
	std::vector<HeldInput> held;
	held.swap(_heldInput);
	for (const HeldInput& input : held)
		RouteMouse(input.message, input.wParam, input.lParam);
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

void Photino::SendHostInput(UINT message, POINT point, WPARAM wParam, LPARAM lParam)
{
	if (_hostInputCallback == nullptr)
		return;
	PhotinoHostInput input = { message, point.x, point.y, (unsigned long long)wParam, (long long)lParam };
	_hostInputCallback(&input);
}

void Photino::RouteMouse(UINT message, WPARAM wParam, LPARAM lParam)
{
	POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
	if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)
		ScreenToClient(_hWnd, &point);

	if (!_trackingMouseLeave)
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
	if (IsMouseRelease(message) && (GET_KEYSTATE_WPARAM(wParam) & buttons) == 0 && _inputOwner != InputTarget::None)
	{
		_inputOwner = InputTarget::None;
		if (GetCapture() == _hWnd)
			ReleaseCapture();
	}
}

bool Photino::HandleCompositionMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT* result)
{
	if (!_compositionController || !_dcompDevice)
		return false;

	if (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST)
	{
		if (_hitRegionsStale && _inputOwner == InputTarget::None && (IsMousePress(message) || !_heldInput.empty()))
		{
			ULONGLONG waited = GetTickCount64() - _hitRegionsStaleSince;
			if (waited < (ULONGLONG)_hitRegionHoldMilliseconds)
			{
				if (_heldInput.empty())
					SetTimer(_hWnd, HIT_REGION_HOLD_TIMER_ID, (UINT)(_hitRegionHoldMilliseconds - waited), nullptr);
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
		_trackingMouseLeave = false;
		if (_inputOwner == InputTarget::None)
		{
			if (_hoverTarget == InputTarget::Page)
				_compositionController->SendMouseInput(COREWEBVIEW2_MOUSE_EVENT_KIND_LEAVE, COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE, 0, { 0, 0 });
			else if (_hoverTarget == InputTarget::Host)
				SendHostInput(WM_MOUSELEAVE, { 0, 0 }, 0, 0);
			_hoverTarget = InputTarget::None;
		}
		*result = 0;
		return true;
	}
	case WM_CAPTURECHANGED:
	{
		// Capture taken by another window ends the drag.
		if ((HWND)lParam != _hWnd)
			_inputOwner = InputTarget::None;
		return false;
	}
	case WM_TIMER:
	{
		if (wParam != HIT_REGION_HOLD_TIMER_ID)
			return false;
		_hitRegionsStale = false;
		FlushHeldInput();
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
		HCURSOR pageCursor = nullptr;
		if (target != InputTarget::Page || FAILED(_compositionController->get_Cursor(&pageCursor)))
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
		if (IsKeyMessage(message))
			SendHostInput(message, { 0, 0 }, wParam, lParam);
		return false;
	}
}

void Photino::FinishPopupRequest(int requestId, ICoreWebView2* newWindow)
{
	auto found = _pendingPopups.find(requestId);
	if (found == _pendingPopups.end())
		return;
	PendingPopup pending = found->second;
	_pendingPopups.erase(found);

	if (newWindow != nullptr)
		pending.args->put_NewWindow(newWindow);
	pending.args->put_Handled(TRUE);	// handled with no new window: window.open returns null in the page
	if (pending.deferral)
		pending.deferral->Complete();
}

void Photino::OnWindowDestroyed()
{
	std::vector<int> pendingIds;
	for (auto& pending : _pendingPopups)
		pendingIds.push_back(pending.first);
	for (int requestId : pendingIds)
		FinishPopupRequest(requestId, nullptr);

	if (_compositionController || _popupOpenerHwnd != nullptr)
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
}
