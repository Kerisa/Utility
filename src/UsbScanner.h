#pragma once

#include <functional>
#include <string>

class UsbScanner
{
	class UsbScannerImpl;
	class UsbScannerImpl2;

public:
	static UsbScanner *GetInstance();
	static void Destroy();

	~UsbScanner();

	bool Start(std::function<void(const std::string & str)> receiver, int timeoutThreshold = 300);
	void Stop();
	UsbScannerImpl *GetImpl();

private:
	UsbScannerImpl *mImpl{ nullptr };
	UsbScannerImpl2 *mImpl2{ nullptr };

private:
	UsbScanner();
	UsbScanner(const UsbScanner &) = delete;
	UsbScanner& operator=(const UsbScanner &) = delete;

	static UsbScanner *msInstance;
};