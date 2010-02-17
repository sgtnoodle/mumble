/* Copyright (C) 2005-2010, Thorvald Natvig <thorvald@natvig.com>

   All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright notice,
     this list of conditions and the following disclaimer.
   - Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation
     and/or other materials provided with the distribution.
   - Neither the name of the Mumble Developers nor the names of its
     contributors may be used to endorse or promote products derived from this
     software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "GlobalShortcut_win.h"
#include "MainWindow.h"
#include "Overlay.h"
#include "Global.h"

#undef FAILED
#define FAILED(Status) (static_cast<HRESULT>(Status)<0)

#define DX_SAMPLE_BUFFER_SIZE 512

uint qHash(const GUID &a) {
	uint val = a.Data1 ^ a.Data2 ^ a.Data3;
	for (int i=0;i<8;i++)
		val += a.Data4[i];
	return val;
}

WinEvent::WinEvent(HWND h, UINT u, WPARAM w, LPARAM l) : QEvent(eventType()), hWnd(h), msg(u), wParam(w), lParam(l) {
}

QEvent::Type WinEvent::eventType() {
	static Type t = static_cast<QEvent::Type>(registerEventType());
	return t;
}

GlobalShortcutEngine *GlobalShortcutEngine::platformInit() {
	return new GlobalShortcutWin();
}

GlobalShortcutWin::GlobalShortcutWin() {
	pDI = NULL;
	uiHardwareDevices = 0;
	start(QThread::LowestPriority);
}

GlobalShortcutWin::~GlobalShortcutWin() {
	quit();
	wait();
}

void GlobalShortcutWin::run() {
	HMODULE hSelf;
	HRESULT hr;
	QTimer *timer;

	if (FAILED(hr = DirectInput8Create(GetModuleHandle(NULL), DIRECTINPUT_VERSION, IID_IDirectInput8, reinterpret_cast<void **>(&pDI), NULL))) {
		qFatal("GlobalShortcutWin: Failed to create d8input");
		return;
	}

	/*
	 * Wait for MainWindow's constructor to finish before we enumerate DirectInput devices.
	 * We need to do this because adding a new device requires a Window handle. (SetCooperativeLevel())
	 */
	while (! g.mw)
		this->msleep(20);

	GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (wchar_t *) &HookKeyboard, &hSelf);
	hhKeyboard = SetWindowsHookEx(WH_KEYBOARD_LL, HookKeyboard, hSelf, 0);
	hhMouse = SetWindowsHookEx(WH_MOUSE_LL, HookMouse, hSelf, 0);

#ifdef QT_NO_DEBUG
#endif

	timer = new QTimer(this);
	timer->moveToThread(this);
	connect(timer, SIGNAL(timeout()), this, SLOT(timeTicked()));
	timer->start(20);

	setPriority(QThread::TimeCriticalPriority);

	exec();

	UnhookWindowsHookEx(hhKeyboard);
	UnhookWindowsHookEx(hhMouse);

	foreach(InputDevice *id, qhInputDevices) {
		if (id->pDID) {
			id->pDID->Unacquire();
			id->pDID->Release();
		}
		delete id;
	}
	pDI->Release();
}

LRESULT CALLBACK GlobalShortcutWin::HookKeyboard(int nCode, WPARAM wParam, LPARAM lParam) {
	GlobalShortcutWin *gsw=static_cast<GlobalShortcutWin *>(engine);
	KBDLLHOOKSTRUCT *key=reinterpret_cast<KBDLLHOOKSTRUCT *>(lParam);
	if (nCode >= 0) {
		bool suppress = false;
		if (g.ocIntercept) {
			WPARAM w = key->vkCode;
			LPARAM l = 1 | (key->scanCode << 16);
			if (key->flags & LLKHF_EXTENDED)
				l |= 0x1000000;
			if (wParam == WM_KEYUP)
				l |= 0xC0000000;

			QWidget *widget = qApp->focusWidget();
			if (! widget)
				widget = & g.ocIntercept->qgv;
			WinEvent *we = new WinEvent(widget->winId(), wParam, w, l);
			QCoreApplication::postEvent(gsw, we);
			
			suppress = true;
		}
		
		QList<QVariant> ql;
		unsigned int keyid = static_cast<unsigned int>((key->scanCode << 8) | 0x4);
		if (key->flags & LLKHF_EXTENDED)
			keyid |= 0x8000U;
		ql << keyid;
		ql << QVariant(QUuid(GUID_SysKeyboard));
		if (gsw->handleButton(ql, !(key->flags & LLKHF_UP)) || suppress)
			return 1;
	}
	return CallNextHookEx(gsw->hhKeyboard, nCode, wParam, lParam);
}

LRESULT CALLBACK GlobalShortcutWin::HookMouse(int nCode, WPARAM wParam, LPARAM lParam) {
	GlobalShortcutWin *gsw=static_cast<GlobalShortcutWin *>(engine);
	MSLLHOOKSTRUCT *mouse=reinterpret_cast<MSLLHOOKSTRUCT *>(lParam);
	if (nCode >= 0) {
		bool suppress = false;

		if (g.ocIntercept) {
			WPARAM w = (mouse->mouseData) >> 16;
			POINT p;
			GetCursorPos(&p);

			LONG x = mouse->pt.x - p.x;
			LONG y = mouse->pt.y - p.y;

			LPARAM l = (static_cast<short>(x) & 0xFFFF) | ((y << 16) & 0xFFFF0000);

			QWidget *widget = qApp->focusWidget();
			if (! widget)
				widget = & g.ocIntercept->qgv;
			WinEvent *we = new WinEvent(widget->winId(), wParam, w, l);
			QCoreApplication::postEvent(gsw, we);

			suppress = true;
		}

		bool down = false;
		unsigned int btn = 0;
		switch (wParam) {
			case WM_LBUTTONDOWN:
				down = true;
			case WM_LBUTTONUP:
				btn = 3;
				break;
			case WM_RBUTTONDOWN:
				down = true;
			case WM_RBUTTONUP:
				btn = 4;
				break;
			case WM_MBUTTONDOWN:
				down = true;
			case WM_MBUTTONUP:
				btn = 5;
				break;
			case WM_XBUTTONDOWN:
				down = true;
			case WM_XBUTTONUP:
				btn = 5 + (mouse->mouseData >> 16);
			default:
				break;
		}
		if (btn) {
			QList<QVariant> ql;
			ql << static_cast<unsigned int>((btn << 8) | 0x4);
			ql << QVariant(QUuid(GUID_SysMouse));
			if (gsw->handleButton(ql, down) || suppress)
				return 1;
		} else if (suppress) {
			return 1;
		}
	}
	return CallNextHookEx(gsw->hhMouse, nCode, wParam, lParam);
}

BOOL CALLBACK GlobalShortcutWin::EnumDeviceObjectsCallback(LPCDIDEVICEOBJECTINSTANCE lpddoi, LPVOID pvRef) {
	InputDevice *id=static_cast<InputDevice *>(pvRef);
	QString name = QString::fromUtf16(reinterpret_cast<const ushort *>(lpddoi->tszName));
	id->qhNames[lpddoi->dwType] = name;

	return DIENUM_CONTINUE;
}

BOOL GlobalShortcutWin::EnumDevicesCB(LPCDIDEVICEINSTANCE pdidi, LPVOID pContext) {
	GlobalShortcutWin *cbgsw=static_cast<GlobalShortcutWin *>(pContext);
	HRESULT hr;

	QString name = QString::fromUtf16(reinterpret_cast<const ushort *>(pdidi->tszProductName));
	QString sname = QString::fromUtf16(reinterpret_cast<const ushort *>(pdidi->tszInstanceName));

	InputDevice *id = new InputDevice;
	id->pDID = NULL;
	id->name = name;
	id->guid = pdidi->guidInstance;
	id->vguid = QVariant(QUuid(id->guid).toString());

	foreach(InputDevice *dev, cbgsw->qhInputDevices) {
		if (dev->guid == id->guid) {
			delete id;
			return DIENUM_CONTINUE;
		}
	}

	if (FAILED(hr = cbgsw->pDI->CreateDevice(pdidi->guidInstance, &id->pDID, NULL)))
		qFatal("GlobalShortcutWin: CreateDevice: %lx", hr);

	if (FAILED(hr = id->pDID->EnumObjects(EnumDeviceObjectsCallback, static_cast<void *>(id), DIDFT_BUTTON)))
		qFatal("GlobalShortcutWin: EnumObjects: %lx", hr);

	if (id->qhNames.count() > 0) {
		QList<DWORD> types = id->qhNames.keys();
		qSort(types);

		int nbuttons = types.count();
		STACKVAR(DIOBJECTDATAFORMAT, rgodf, nbuttons);
		DIDATAFORMAT df;
		ZeroMemory(&df, sizeof(df));
		df.dwSize = sizeof(df);
		df.dwObjSize = sizeof(DIOBJECTDATAFORMAT);
		df.dwFlags=DIDF_ABSAXIS;
		df.dwDataSize = (nbuttons + 3) & (~0x3);
		df.dwNumObjs = nbuttons;
		df.rgodf = rgodf;
		for (int i=0;i<nbuttons;i++) {
			ZeroMemory(& rgodf[i], sizeof(DIOBJECTDATAFORMAT));
			DWORD dwType = types[i];
			DWORD dwOfs = i;
			rgodf[i].dwOfs = dwOfs;
			rgodf[i].dwType = dwType;
			id->qhOfsToType[dwOfs] = dwType;
			id->qhTypeToOfs[dwType] = dwOfs;
		}

		if (FAILED(hr = id->pDID->SetCooperativeLevel(g.mw->winId(), DISCL_NONEXCLUSIVE|DISCL_BACKGROUND)))
			qFatal("GlobalShortcutWin: SetCooperativeLevel: %lx", hr);

		if (FAILED(hr = id->pDID->SetDataFormat(&df)))
			qFatal("GlobalShortcutWin: SetDataFormat: %lx", hr);

		DIPROPDWORD dipdw;

		dipdw.diph.dwSize       = sizeof(DIPROPDWORD);
		dipdw.diph.dwHeaderSize = sizeof(DIPROPHEADER);
		dipdw.diph.dwObj        = 0;
		dipdw.diph.dwHow        = DIPH_DEVICE;
		dipdw.dwData            = DX_SAMPLE_BUFFER_SIZE;

		if (FAILED(hr = id->pDID->SetProperty(DIPROP_BUFFERSIZE, &dipdw.diph)))
			qFatal("GlobalShortcutWin::SetProperty");

		qWarning("Adding device %s %s %s:%d", qPrintable(QUuid(id->guid).toString()),qPrintable(name),qPrintable(sname),id->qhNames.count());
		cbgsw->qhInputDevices[id->guid] = id;
	} else {
		id->pDID->Release();
		delete id;
	}

	return DIENUM_CONTINUE;
}

void GlobalShortcutWin::timeTicked() {
	if (g.mw->uiNewHardware != uiHardwareDevices) {
		uiHardwareDevices = g.mw->uiNewHardware;

		pDI->EnumDevices(DI8DEVCLASS_ALL, EnumDevicesCB, static_cast<void *>(this), DIEDFL_ATTACHEDONLY);
	}

	if (bNeedRemap)
		remap();

	foreach(InputDevice *id, qhInputDevices) {
		DIDEVICEOBJECTDATA rgdod[DX_SAMPLE_BUFFER_SIZE];
		DWORD   dwItems = DX_SAMPLE_BUFFER_SIZE;
		HRESULT hr;

		hr = id->pDID->Acquire();

		switch (hr) {
			case DI_OK:
			case S_FALSE:
				break;
			case DIERR_UNPLUGGED:
			case DIERR_GENERIC:
				qWarning("Removing device %s", qPrintable(QUuid(id->guid).toString()));
				id->pDID->Release();
				qhInputDevices.remove(id->guid);
				delete id;
				return;
			case DIERR_OTHERAPPHASPRIO:
				continue;
			default:
				break;
		}
		id->pDID->Poll();

		hr = id->pDID->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), rgdod, &dwItems, 0);
		if (FAILED(hr))
			continue;

		if (dwItems <= 0)
			continue;

		for (DWORD j=0; j<dwItems; j++) {
			QList<QVariant> ql;

			quint32 uiType = id->qhOfsToType.value(rgdod[j].dwOfs);
			ql << uiType;
			ql << id->vguid;
			handleButton(ql, rgdod[j].dwData & 0x80);
		}
	}
}

QString GlobalShortcutWin::buttonName(const QVariant &v) {
	GlobalShortcutWin *gsw = static_cast<GlobalShortcutWin *>(GlobalShortcutEngine::engine);

	const QList<QVariant> &sublist = v.toList();
	if (sublist.count() != 2)
		return QString();

	bool ok = false;
	DWORD type = sublist.at(0).toUInt(&ok);
	QUuid guid(sublist.at(1).toString());

	if (guid.isNull() || (!ok))
		return QString();

	QString device=guid.toString();
	QString name=QLatin1String("Unknown");
	InputDevice *id = gsw->qhInputDevices.value(guid);
	if (guid == GUID_SysMouse)
		device=QLatin1String("M:");
	else if (guid == GUID_SysKeyboard)
		device=QLatin1String("K:");
	else if (id)
		device=id->name+QLatin1String(":");
	if (id) {
		name=id->qhNames.value(type);
	}
	return device+name;
}

bool GlobalShortcutWin::canSuppress() {
	return true;
}

void GlobalShortcutWin::prepareInput() {
	SetKeyboardState(ucKeyState);
}

void GlobalShortcutWin::customEvent(QEvent *e) {
	if (e->type() == WinEvent::eventType()) {
		WinEvent *we = static_cast<WinEvent *>(e);

		GetKeyboardState(ucKeyState);

		if ((we->msg == WM_KEYDOWN) || (we->msg == WM_KEYUP) || (we->msg == WM_SYSKEYDOWN) || (we->msg == WM_SYSKEYUP)) {
			switch (we->wParam) {
				case VK_LCONTROL:
				case VK_RCONTROL:
					if ((we->msg == WM_KEYDOWN) || (we->msg == WM_SYSKEYDOWN))
						ucKeyState[we->wParam] |= 0x80;
					else {
						ucKeyState[we->wParam] &= 0x7f;
						
						if ((ucKeyState[VK_LCONTROL] & 0x80) || (ucKeyState[VK_RCONTROL] & 0x80)) {
							SetKeyboardState(ucKeyState);
							return;
						}
					}

					we->wParam = VK_CONTROL;
					break;
				case VK_LSHIFT:
				case VK_RSHIFT:
					if ((we->msg == WM_KEYDOWN) || (we->msg == WM_SYSKEYDOWN))
						ucKeyState[we->wParam] |= 0x80;
					else {
						ucKeyState[we->wParam] &= 0x7f;
						
						if ((ucKeyState[VK_LSHIFT] & 0x80) || (ucKeyState[VK_RSHIFT] & 0x80)) {
							SetKeyboardState(ucKeyState);
							return;
						}
					}

					we->wParam = VK_SHIFT;
					break;
					
				case VK_LMENU:
				case VK_RMENU:
					if ((we->msg == WM_KEYDOWN) || (we->msg == WM_SYSKEYDOWN))
						ucKeyState[we->wParam] |= 0x80;
					else {
						ucKeyState[we->wParam] &= 0x7f;
						
						if ((ucKeyState[VK_LMENU] & 0x80) || (ucKeyState[VK_RMENU] & 0x80)) {
							SetKeyboardState(ucKeyState);
							return;
						}
					}

					we->wParam = VK_MENU;
					break;
				default:
					break;
			}

			if ((we->msg == WM_KEYDOWN) || (we->msg == WM_SYSKEYDOWN)) {
				if (ucKeyState[we->wParam] & 0x80)
					we->lParam |= 0x40000000;
				ucKeyState[we->wParam] |= 0x80;

			} else if (we->msg == WM_KEYUP) {
				ucKeyState[we->wParam] &= 0x7f;
			}
			qWarning("SEND %x %04x %08x (%d %d %d)", we->msg, we->wParam, we->lParam, g.mw->qleChat->isActiveWindow(), g.mw->qleChat->isVisible(), g.mw->qleChat->hasFocus());

			SetKeyboardState(ucKeyState);
			::PostMessage(we->hWnd, we->msg, we->wParam, we->lParam);
		} else {
			short dx = (we->lParam & 0xffff);
			short dy = (we->lParam >> 16);
			
			switch(we->msg) {
				case WM_LBUTTONDOWN:
					ucKeyState[VK_LBUTTON] |= 0x80;
					break;
				case WM_LBUTTONUP:
					ucKeyState[VK_LBUTTON] &= 0x7f;
					break;
				case WM_RBUTTONDOWN:
					ucKeyState[VK_RBUTTON] |= 0x80;
					break;
				case WM_RBUTTONUP:
					ucKeyState[VK_RBUTTON] &= 0x7f;
					break;
				case WM_MBUTTONDOWN:
					ucKeyState[VK_MBUTTON] |= 0x80;
					break;
				case WM_MBUTTONUP:
					ucKeyState[VK_MBUTTON] &= 0x7f;
					break;
				case WM_XBUTTONDOWN:
					if (we->wParam == XBUTTON1)
						ucKeyState[VK_XBUTTON1] |= 0x80;
					else if (we->wParam == XBUTTON2)
						ucKeyState[VK_XBUTTON2] |= 0x80;
					break;
				case WM_XBUTTONUP:
					if (we->wParam == XBUTTON1)
						ucKeyState[VK_XBUTTON1] &= 0x7f;
					else if (we->wParam == XBUTTON2)
						ucKeyState[VK_XBUTTON2] &= 0x7f;
					break;
				default:
					break;
			}
			
			we->wParam = 0;
			if (ucKeyState[VK_CONTROL] & 0x80)
				we->wParam |= MK_CONTROL;
			if (ucKeyState[VK_LBUTTON] & 0x80)
				we->wParam |= MK_LBUTTON;
			if (ucKeyState[VK_MBUTTON] & 0x80)
				we->wParam |= MK_MBUTTON;
			if (ucKeyState[VK_RBUTTON] & 0x80)
				we->wParam |= MK_RBUTTON;
			if (ucKeyState[VK_SHIFT] & 0x80)
				we->wParam |= MK_SHIFT;
			if (ucKeyState[VK_XBUTTON1] & 0x80)
				we->wParam |= MK_XBUTTON1;
			if (ucKeyState[VK_XBUTTON2] & 0x80)
				we->wParam |= MK_XBUTTON2;

			int x, y;

			g.ocIntercept->moveMouse(dx, dy, x, y);

			qWarning("Mousy %d %d => %d %d %d %d", dx, dy, x, y, QCursor::pos().x(), QCursor::pos().y());
/*
			QDesktopWidget qdw;
			QRect qr = qdw.screenGeometry();

			x -= qr.x();
			y -= qr.y();

			unsigned int uix = (x * g.ocIntercept->uiWidth) / qr.x();
			unsigned int uiy = (y * g.ocIntercept->uiHeight) / qr.y();

			qWarning("%d, %d => %d, %d", x, y, uix, uiy);
*/			
			unsigned int uix = x;
			unsigned int uiy = y;
			we->lParam = uix | (uiy << 16);
			
			::PostMessage(we->hWnd, we->msg, we->wParam, we->lParam);
			
		}
		e->accept();
		return;
	}
	GlobalShortcutEngine::customEvent(e);
}
