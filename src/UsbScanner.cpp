#include "stdafx.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
#include "UsbScanner.h"
#include <windows.h>

using namespace std;

namespace Detail
{
	constexpr wchar_t WndClassName[] = L"UsbScanner{9CE30E91-79BE-445E-932C-626DA485DE42}";

	LRESULT CALLBACK LowLevelKeyboardProc(
		_In_ int    nCode,
		_In_ WPARAM wParam,
		_In_ LPARAM lParam
	);
}


class UsbScanner::UsbScannerImpl
{
public:
	UsbScannerImpl();
	~UsbScannerImpl();

	bool Start(std::function<void(const string &)> receiver, int timeoutThreshold, unsigned long tid);
	void Stop();
	void Flush();
	void SaveChar(uint16_t vkey, uint16_t scancode, bool keydown);

	int mTimeoutThreshold{ 0 };
	std::function<void(const string &)> mReceiver;

	HWND										mWnd{ NULL };
	HHOOK										mHook{ NULL };
	std::thread									mProcess;
	std::mutex									mConditionLock;
	std::condition_variable						mCondition;
	chrono::time_point<chrono::steady_clock>	mLastReceiveTime;
	string										mDataBuffer;
	bool										mStop{ true };
	bool										mReady{ false };
};



UsbScanner::UsbScannerImpl::UsbScannerImpl()
{
	mDataBuffer.reserve(1024);
	mLastReceiveTime = chrono::steady_clock::now();
}

UsbScanner::UsbScannerImpl::~UsbScannerImpl()
{
	Stop();
}

void UsbScanner::UsbScannerImpl::Stop()
{
	if (mStop)
		return;

	mStop = true;
	mProcess.join();
	UnhookWindowsHookEx(mHook);
	SendMessage(mWnd, WM_DESTROY, 0, 0);
	mWnd = NULL;
	mHook = NULL;

	if (!mDataBuffer.empty())
	{
		Flush();
	}
}

void UsbScanner::UsbScannerImpl::Flush()
{
	mReceiver(mDataBuffer);
	mDataBuffer.clear();
}

void UsbScanner::UsbScannerImpl::SaveChar(uint16_t vkey, uint16_t scancode, bool keydown)
{
	auto diff = chrono::steady_clock::now() - mLastReceiveTime;
	if (!mDataBuffer.empty() && diff > chrono::milliseconds(mTimeoutThreshold))
	{
		Flush();
	}

	char name[255] = { 0 };
	if (GetKeyNameTextA(scancode << 16, name, _countof(name)) == 0)
		return;

	if (vkey == VK_SPACE)
		name[0] = ' ';
	else if (strlen(name) > 1)
		return;

	SHORT cap2 = !!(GetKeyState(VK_CAPITAL) & 1);
	SHORT shift2 = !!(GetKeyState(VK_LSHIFT) & 0x8000);

	// GetKeyNameText 返回的是大写字母
	name[0] = cap2 ? toupper(name[0]) : tolower(name[0]);

	if (shift2)
	{
		if (islower(name[0]))
			name[0] -= 32;
		else if (isupper(name[0]))
			name[0] += 32;
		else
		{
			string l = "`1234567890-=[];',./\\";
			string u = "~!@#$%^&*()_+{}:\"<>?|";
			size_t pos = l.find_first_of(name[0]);
			if (pos != string::npos)
				name[0] = u[pos];
		}
	}

	if (keydown)
	{
		mLastReceiveTime = chrono::steady_clock::now();
		mDataBuffer.push_back(name[0]);
	}
}

bool UsbScanner::UsbScannerImpl::Start(std::function<void(const string &)> receiver, int timeoutThreshold, unsigned long tid)
{
	if (!mStop)
		return false;

	assert(tid == 0);
	mTimeoutThreshold = timeoutThreshold;
	mReceiver = receiver;
	
	mStop = false;
	mReady = false;
	mProcess = std::thread([this, tid]() {

		WNDCLASSEXW wcex;
		wcex.cbSize = sizeof(WNDCLASSEX);

		wcex.style = CS_HREDRAW | CS_VREDRAW;
		wcex.lpfnWndProc = DefWindowProc;
		wcex.cbClsExtra = 0;
		wcex.cbWndExtra = 0;
		wcex.hInstance = GetModuleHandle(NULL);
		wcex.hIcon = NULL;
		wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
		wcex.hbrBackground = NULL;
		wcex.lpszMenuName = NULL;
		wcex.lpszClassName = Detail::WndClassName;
		wcex.hIconSm = NULL;
		RegisterClassExW(&wcex);

		mWnd = CreateWindowW(Detail::WndClassName, L"", WS_OVERLAPPEDWINDOW,
			1, 0, 1, 1, nullptr, nullptr, GetModuleHandle(NULL), nullptr);
		if (mWnd != NULL)
		{
			UpdateWindow(mWnd);
			ShowWindow(mWnd, SW_HIDE);

			mHook = SetWindowsHookEx(WH_KEYBOARD_LL, Detail::LowLevelKeyboardProc, GetModuleHandle(NULL), tid);
			if (mHook != NULL)
				mReady = true;
			else
				mStop = true;
		}
		else
		{
			mStop = true;
		}


		{
			std::unique_lock<std::mutex> lock(mConditionLock);
			mCondition.notify_one();
		}

		MSG msg;
		while (!mStop)
		{
			if (PeekMessage(&msg, mWnd, NULL, NULL, PM_NOREMOVE) == 0)
			{
				if (!mDataBuffer.empty() && chrono::steady_clock::now() - mLastReceiveTime > chrono::milliseconds(mTimeoutThreshold))
				{
					Flush();
				}
				else
				{
					this_thread::sleep_for(chrono::milliseconds(1));
				}
			}
			else
			{				
				GetMessage(&msg, mWnd, NULL, NULL);
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
		}
	});

	std::unique_lock<std::mutex> lock(mConditionLock);
	mCondition.wait(lock, [this] { return mReady || mStop; });
	if (mStop)
	{
		mProcess.detach();
		return false;
	}
	return true;
}


namespace Detail
{
	LRESULT CALLBACK LowLevelKeyboardProc(
		_In_ int    nCode,
		_In_ WPARAM wParam,
		_In_ LPARAM lParam
	)
	{
		KBDLLHOOKSTRUCT *hookInfo = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
		if (UsbScanner::GetInstance()->GetImpl())
		{
			UsbScanner::GetInstance()->GetImpl()->SaveChar(hookInfo->vkCode, hookInfo->scanCode, !(hookInfo->flags & 0x80));
		}

		return CallNextHookEx(NULL, nCode, wParam, lParam);
	}
}


////////////////////////////////////////////////////////////////////////////////


class UsbScanner::UsbScannerImpl2
{
public:
	UsbScannerImpl2();
	~UsbScannerImpl2();

	bool Start(std::function<void(const string &)> receiver, int timeoutThreshold, unsigned long tid);
	void Stop();
	void Flush();
	void SaveChar(uint16_t vkey, uint16_t scancode, bool keydown);

	int mTimeoutThreshold{ 0 };
	std::function<void(const string &)> mReceiver;

	HWND										mWnd{ NULL };
	HHOOK										mHook{ NULL };
	std::thread									mProcess;
	std::mutex									mConditionLock;
	std::condition_variable						mCondition;
	chrono::time_point<chrono::steady_clock>	mLastReceiveTime;
	string										mDataBuffer;
	bool										mStop{ true };
	bool										mReady{ false };
};



UsbScanner::UsbScannerImpl2::UsbScannerImpl2()
{
	mDataBuffer.reserve(1024);
	mLastReceiveTime = chrono::steady_clock::now();
}

UsbScanner::UsbScannerImpl2::~UsbScannerImpl2()
{
	Stop();
}

void UsbScanner::UsbScannerImpl2::Stop()
{
	if (mStop)
		return;

	mStop = true;
	mProcess.join();
	UnhookWindowsHookEx(mHook);
	SendMessage(mWnd, WM_DESTROY, 0, 0);
	mWnd = NULL;
	mHook = NULL;

	if (!mDataBuffer.empty())
	{
		Flush();
	}
}

void UsbScanner::UsbScannerImpl2::Flush()
{
	mReceiver(mDataBuffer);
	mDataBuffer.clear();
}

void UsbScanner::UsbScannerImpl2::SaveChar(uint16_t vkey, uint16_t scancode, bool keydown)
{
	auto diff = chrono::steady_clock::now() - mLastReceiveTime;
	if (!mDataBuffer.empty() && diff > chrono::milliseconds(mTimeoutThreshold))
	{
		Flush();
	}

	char name[255] = { 0 };
	if (GetKeyNameTextA(scancode << 16, name, _countof(name)) == 0)
		return;

	if (vkey == VK_SPACE)
		name[0] = ' ';
	else if (vkey == VK_RETURN)
		name[0] = '\n';
	else if (strlen(name) > 1)
		return;

	SHORT cap2 = !!(GetKeyState(VK_CAPITAL) & 1);
	SHORT shift2 = !!(GetKeyState(VK_LSHIFT) & 0x8000) || !!(GetKeyState(VK_RSHIFT) & 0x8000);

	// GetKeyNameText 返回的是大写字母
	name[0] = cap2 ? toupper(name[0]) : tolower(name[0]);

	if (shift2)
	{
		if (islower(name[0]))
			name[0] -= 32;
		else if (isupper(name[0]))
			name[0] += 32;
		else
		{
			string l = "`1234567890-=[];',./\\";
			string u = "~!@#$%^&*()_+{}:\"<>?|";
			size_t pos = l.find_first_of(name[0]);
			if (pos != string::npos)
				name[0] = u[pos];
		}
	}

	if (keydown)
	{
		mLastReceiveTime = chrono::steady_clock::now();
		mDataBuffer.push_back(name[0]);
	}
}

bool UsbScanner::UsbScannerImpl2::Start(std::function<void(const string &)> receiver, int timeoutThreshold, unsigned long tid)
{
	if (!mStop)
		return false;

	assert(tid == 0);
	mTimeoutThreshold = timeoutThreshold;
	mReceiver = receiver;

	mStop = false;
	mReady = false;
	mProcess = std::thread([this, tid]() {

		WNDCLASSEXW wcex;
		wcex.cbSize = sizeof(WNDCLASSEX);

		wcex.style = CS_HREDRAW | CS_VREDRAW;
		wcex.lpfnWndProc = DefWindowProc;
		wcex.cbClsExtra = 0;
		wcex.cbWndExtra = 0;
		wcex.hInstance = GetModuleHandle(NULL);
		wcex.hIcon = NULL;
		wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
		wcex.hbrBackground = NULL;
		wcex.lpszMenuName = NULL;
		wcex.lpszClassName = Detail::WndClassName;
		wcex.hIconSm = NULL;
		RegisterClassExW(&wcex);

		mWnd = CreateWindowW(Detail::WndClassName, L"", WS_OVERLAPPEDWINDOW,
			1, 0, 1, 1, nullptr, nullptr, GetModuleHandle(NULL), nullptr);
		if (mWnd != NULL)
		{
			UpdateWindow(mWnd);
			ShowWindow(mWnd, SW_HIDE);

			RAWINPUTDEVICE Rid[1];
			Rid[0].usUsagePage = 0x01;
			Rid[0].usUsage = 0x06;
			Rid[0].dwFlags = RIDEV_INPUTSINK;
			Rid[0].hwndTarget = mWnd;

			if (RegisterRawInputDevices(Rid, 1, sizeof(Rid[0])) == FALSE)
				mStop = true;
			else
				mReady = true;
		}
		else
		{
			mStop = true;
		}


		{
			std::unique_lock<std::mutex> lock(mConditionLock);
			mCondition.notify_one();
		}

		MSG msg;
		while (!mStop)
		{
			if (PeekMessage(&msg, mWnd, NULL, NULL, PM_NOREMOVE) == 0)
			{
				if (!mDataBuffer.empty() && chrono::steady_clock::now() - mLastReceiveTime > chrono::milliseconds(mTimeoutThreshold))
				{
					Flush();
				}
				else
				{
					this_thread::sleep_for(chrono::milliseconds(1));
				}
			}
			else
			{
				GetMessage(&msg, mWnd, NULL, NULL);
				if (msg.message != WM_INPUT)
				{
					TranslateMessage(&msg);
					DispatchMessage(&msg);
				}
				else
				{
					UINT dwSize;
					GetRawInputData((HRAWINPUT)msg.lParam, RID_INPUT, NULL, &dwSize,
						sizeof(RAWINPUTHEADER));
					vector<BYTE> lpb;
					lpb.resize(dwSize);

					if (GetRawInputData((HRAWINPUT)msg.lParam, RID_INPUT, lpb.data(), &dwSize,
						sizeof(RAWINPUTHEADER)) != dwSize)
						throw exception("GetRawInputData does not return correct size !\n");

					RAWINPUT* raw = (RAWINPUT*)lpb.data();
					if (raw->header.dwType == RIM_TYPEKEYBOARD)
					{
						SaveChar(
							raw->data.keyboard.VKey,
							raw->data.keyboard.MakeCode,
							raw->data.keyboard.Flags == RI_KEY_MAKE
						);
					}
				}
			}
		}
	});

	std::unique_lock<std::mutex> lock(mConditionLock);
	mCondition.wait(lock, [this] { return mReady || mStop; });
	if (mStop)
	{
		mProcess.detach();
		return false;
	}
	return true;
}


////////////////////////////////////////////////////////////////////////////////


UsbScanner *UsbScanner::msInstance;


UsbScanner * UsbScanner::GetInstance()
{
	if (!msInstance)
	{
		UsbScanner *tmp = new UsbScanner();
		if (!msInstance)
		{
			msInstance = tmp;
		}
		else
		{
			delete tmp;
		}
	}
	return msInstance;
}

void UsbScanner::Destroy()
{
	delete msInstance;
}

UsbScanner::UsbScanner()
{
	//mImpl = new UsbScannerImpl();
	mImpl2 = new UsbScannerImpl2();
}

UsbScanner::~UsbScanner()
{
	//delete mImpl;
	delete mImpl2;
}

bool UsbScanner::Start(std::function<void(const std::string & str)> receiver, int timeoutThreshold)
{
	return mImpl2->Start(receiver, timeoutThreshold, 0);
}

void UsbScanner::Stop()
{
	mImpl2->Stop();
}

UsbScanner::UsbScannerImpl * UsbScanner::GetImpl()
{
	return mImpl;
}
