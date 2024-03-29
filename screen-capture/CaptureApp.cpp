#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <tchar.h>

#include "Defs.h"
#include "DDAImpl.h"
#include "Preproc.h"
#include "NvEncoder/NvEncoderD3D11.h"

#define BTN_CPT  100

static TCHAR szWindowClass[] = _T("CaptureApp");

// The string that appears in the application's title bar.
static TCHAR szTitle[] = _T("Remote Capture");

HINSTANCE hInst;

// Forward declarations of functions included in this code module:
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

class DemoApplication
{
	/// Demo Application Core class
#define returnIfError(x)\
    if (FAILED(x))\
    {\
        printf(__FUNCTION__": Line %d, File %s Returning error 0x%08x\n", __LINE__, __FILE__, x);\
        return x;\
    }

private:
	/// DDA wrapper object, defined in DDAImpl.h
	DDAImpl *pDDAWrapper = nullptr;
	/// PreProcesor for encoding. Defined in Preproc.h
	/// Preprocessingis required if captured images are of different size than encWidthxencHeight
	/// This application always uses this preprocessor
	RGBToNV12 *pColorConv = nullptr;
	/// NVENCODE API wrapper. Defined in NvEncoderD3D11.h. This class is imported from NVIDIA Video SDK
	NvEncoderD3D11 *pEnc = nullptr;
	/// D3D11 device context used for the operations demonstrated in this application
	ID3D11Device *pD3DDev = nullptr;
	/// D3D11 device context
	ID3D11DeviceContext *pCtx = nullptr;
	/// D3D11 RGB Texture2D object that recieves the captured image from DDA
	ID3D11Texture2D *pDupTex2D = nullptr;
	/// D3D11 YUV420 Texture2D object that sends the image to NVENC for video encoding
	ID3D11Texture2D *pEncBuf = nullptr;
	/// Output video bitstream file handle
	FILE *fp = nullptr;
	/// Failure count from Capture API
	UINT failCount = 0;
	/// Video output dimensions
	const static UINT encWidth = 1920;
	const static UINT encHeight = 1080;
	/// turn off preproc, let NVENCODE API handle colorspace conversion
	const static bool bNoVPBlt = false;
	/// Video output file name
	const char fnameBase[64] = "DDATest_%d.h264";
	/// Encoded video bitstream packet in CPU memory
	std::vector<std::vector<uint8_t>> vPacket;
	/// NVENCODEAPI session intialization parameters
	NV_ENC_INITIALIZE_PARAMS encInitParams = { 0 };
	/// NVENCODEAPI video encoding configuration parameters
	NV_ENC_CONFIG encConfig = { 0 };
private:
	/// Initialize DXGI pipeline
	HRESULT InitDXGI()
	{
		HRESULT hr = S_OK;
		/// Driver types supported
		D3D_DRIVER_TYPE DriverTypes[] =
		{
			D3D_DRIVER_TYPE_HARDWARE,
			D3D_DRIVER_TYPE_WARP,
			D3D_DRIVER_TYPE_REFERENCE,
		};
		UINT NumDriverTypes = ARRAYSIZE(DriverTypes);

		/// Feature levels supported
		D3D_FEATURE_LEVEL FeatureLevels[] =
		{
			D3D_FEATURE_LEVEL_11_0,
			D3D_FEATURE_LEVEL_10_1,
			D3D_FEATURE_LEVEL_10_0,
			D3D_FEATURE_LEVEL_9_1
		};
		UINT NumFeatureLevels = ARRAYSIZE(FeatureLevels);
		D3D_FEATURE_LEVEL FeatureLevel = D3D_FEATURE_LEVEL_11_0;

		/// Create device
		for (UINT DriverTypeIndex = 0; DriverTypeIndex < NumDriverTypes; ++DriverTypeIndex)
		{
			hr = D3D11CreateDevice(nullptr, DriverTypes[DriverTypeIndex], nullptr, /*D3D11_CREATE_DEVICE_DEBUG*/0, FeatureLevels, NumFeatureLevels,
				D3D11_SDK_VERSION, &pD3DDev, &FeatureLevel, &pCtx);
			if (SUCCEEDED(hr))
			{
				// Device creation succeeded, no need to loop anymore
				break;
			}
		}
		return hr;
	}

	/// Initialize DDA handler
	HRESULT InitDup()
	{
		HRESULT hr = S_OK;
		if (!pDDAWrapper)
		{
			pDDAWrapper = new DDAImpl(pD3DDev, pCtx);
			hr = pDDAWrapper->Init();
			returnIfError(hr);
		}
		return hr;
	}

	/// Initialize NVENCODEAPI wrapper
	HRESULT InitEnc()
	{
		if (!pEnc)
		{
			DWORD w = bNoVPBlt ? pDDAWrapper->getWidth() : encWidth;
			DWORD h = bNoVPBlt ? pDDAWrapper->getHeight() : encHeight;
			NV_ENC_BUFFER_FORMAT fmt = bNoVPBlt ? NV_ENC_BUFFER_FORMAT_ARGB : NV_ENC_BUFFER_FORMAT_NV12;
			pEnc = new NvEncoderD3D11(pD3DDev, w, h, fmt);
			if (!pEnc)
			{
				returnIfError(E_FAIL);
			}

			ZeroMemory(&encInitParams, sizeof(encInitParams));
			ZeroMemory(&encConfig, sizeof(encConfig));
			encInitParams.encodeConfig = &encConfig;
			encInitParams.encodeWidth = w;
			encInitParams.encodeHeight = h;
			encInitParams.maxEncodeWidth = pDDAWrapper->getWidth();
			encInitParams.maxEncodeHeight = pDDAWrapper->getHeight();
			encConfig.gopLength = 5;

			try
			{
				pEnc->CreateDefaultEncoderParams(&encInitParams, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_LOW_LATENCY_HP_GUID);
				pEnc->CreateEncoder(&encInitParams);
			}
			catch (...)
			{
				returnIfError(E_FAIL);
			}
		}
		return S_OK;
	}

	/// Initialize preprocessor
	HRESULT InitColorConv()
	{
		if (!pColorConv)
		{
			pColorConv = new RGBToNV12(pD3DDev, pCtx);
			HRESULT hr = pColorConv->Init();
			returnIfError(hr);
		}
		return S_OK;
	}

	/// Initialize Video output file
	HRESULT InitOutFile()
	{
		if (!fp)
		{
			char fname[64] = { 0 };
			sprintf_s(fname, (const char *)fnameBase, failCount);
			errno_t err = fopen_s(&fp, fname, "wb");
			returnIfError(err);
		}
		return S_OK;
	}

	/// Write encoded video output to file
	void WriteEncOutput()
	{
		int nFrame = 0;
		nFrame = (int)vPacket.size();
		for (std::vector<uint8_t> &packet : vPacket)
		{
			fwrite(packet.data(), packet.size(), 1, fp);
		}
	}

public:
	/// Initialize demo application
	HRESULT Init()
	{
		HRESULT hr = S_OK;

		hr = InitDXGI();
		returnIfError(hr);

		hr = InitDup();
		returnIfError(hr);


		hr = InitEnc();
		returnIfError(hr);

		hr = InitColorConv();
		returnIfError(hr);

		hr = InitOutFile();
		returnIfError(hr);
		return hr;
	}

	/// Capture a frame using DDA
	HRESULT Capture(int wait)
	{
		HRESULT hr = pDDAWrapper->GetCapturedFrame(&pDupTex2D, wait); // Release after preproc
		if (FAILED(hr))
		{
			failCount++;
		}
		return hr;
	}

	/// Preprocess captured frame
	HRESULT Preproc()
	{
		HRESULT hr = S_OK;
		const NvEncInputFrame *pEncInput = pEnc->GetNextInputFrame();
		pEncBuf = (ID3D11Texture2D *)pEncInput->inputPtr;
		if (bNoVPBlt)
		{
			pCtx->CopySubresourceRegion(pEncBuf, D3D11CalcSubresource(0, 0, 1), 0, 0, 0, pDupTex2D, 0, NULL);
		}
		else
		{
			hr = pColorConv->Convert(pDupTex2D, pEncBuf);
		}
		SAFE_RELEASE(pDupTex2D);
		returnIfError(hr);

		pEncBuf->AddRef();  // Release after encode
		return hr;
	}

	/// Encode the captured frame using NVENCODEAPI
	HRESULT Encode()
	{
		HRESULT hr = S_OK;
		try
		{
			pEnc->EncodeFrame(vPacket);
			WriteEncOutput();
		}
		catch (...)
		{
			hr = E_FAIL;
		}
		SAFE_RELEASE(pEncBuf);
		return hr;
	}

	/// Release all resources
	void Cleanup(bool bDelete = true)
	{
		if (pDDAWrapper)
		{
			pDDAWrapper->Cleanup();
			delete pDDAWrapper;
			pDDAWrapper = nullptr;
		}

		if (pColorConv)
		{
			pColorConv->Cleanup();
		}

		if (pEnc)
		{
			ZeroMemory(&encInitParams, sizeof(NV_ENC_INITIALIZE_PARAMS));
			ZeroMemory(&encConfig, sizeof(NV_ENC_CONFIG));
		}

		SAFE_RELEASE(pDupTex2D);
		if (bDelete)
		{
			if (pEnc)
			{
				/// Flush the encoder and write all output to file before destroying the encoder
				pEnc->EndEncode(vPacket);
				WriteEncOutput();
				pEnc->DestroyEncoder();
				if (bDelete)
				{
					delete pEnc;
					pEnc = nullptr;
				}

				ZeroMemory(&encInitParams, sizeof(NV_ENC_INITIALIZE_PARAMS));
				ZeroMemory(&encConfig, sizeof(NV_ENC_CONFIG));
			}

			if (pColorConv)
			{
				delete pColorConv;
				pColorConv = nullptr;
			}
			SAFE_RELEASE(pD3DDev);
			SAFE_RELEASE(pCtx);
		}
	}
	DemoApplication() {}
	~DemoApplication()
	{
		Cleanup(true);
	}
};

/// Demo 60 FPS (approx.) capture
int Grab60FPS(int nFrames)
{
	const int WAIT_BASE = 17;
	DemoApplication Demo;
	HRESULT hr = S_OK;
	int capturedFrames = 0;
	LARGE_INTEGER start = { 0 };
	LARGE_INTEGER end = { 0 };
	LARGE_INTEGER interval = { 0 };
	LARGE_INTEGER freq = { 0 };
	int wait = WAIT_BASE;

	QueryPerformanceFrequency(&freq);

	/// Reset waiting time for the next screen capture attempt
#define RESET_WAIT_TIME(start, end, interval, freq)         \
    QueryPerformanceCounter(&end);                          \
    interval.QuadPart = end.QuadPart - start.QuadPart;      \
    MICROSEC_TIME(interval, freq);                          \
    wait = (int)(WAIT_BASE - (interval.QuadPart * 1000));

	/// Initialize Demo app
	hr = Demo.Init();
	if (FAILED(hr))
	{
		printf("Initialization failed with error 0x%08x\n", hr);
		return -1;
	}

	/// Run capture loop
	do
	{
		/// get start timestamp. 
		/// use this to adjust the waiting period in each capture attempt to approximately attempt 60 captures in a second
		QueryPerformanceCounter(&start);
		/// Get a frame from DDA
		hr = Demo.Capture(wait);
		if (hr == DXGI_ERROR_WAIT_TIMEOUT)
		{
			/// retry if there was no new update to the screen during our specific timeout interval
			/// reset our waiting time
			RESET_WAIT_TIME(start, end, interval, freq);
			continue;
		}
		else
		{
			if (FAILED(hr))
			{
				/// Re-try with a new DDA object
				printf("Captrue failed with error 0x%08x. Re-create DDA and try again.\n", hr);
				Demo.Cleanup();
				hr = Demo.Init();
				if (FAILED(hr))
				{
					/// Could not initialize DDA, bail out/
					printf("Failed to Init DDDemo. return error 0x%08x\n", hr);
					return -1;
				}
				RESET_WAIT_TIME(start, end, interval, freq);
				QueryPerformanceCounter(&start);
				/// Get a frame from DDA
				Demo.Capture(wait);
			}
			RESET_WAIT_TIME(start, end, interval, freq);
			/// Preprocess for encoding
			hr = Demo.Preproc();
			if (FAILED(hr))
			{
				printf("Preproc failed with error 0x%08x\n", hr);
				return -1;
			}
			hr = Demo.Encode();
			if (FAILED(hr))
			{
				printf("Encode failed with error 0x%08x\n", hr);
				return -1;
			}
			capturedFrames++;
		}
	} while (capturedFrames <= nFrames);

	return 0;
}

int CALLBACK WinMain(
	_In_ HINSTANCE hInstance,
	_In_ HINSTANCE hPrevInstance,
	_In_ LPSTR     lpCmdLine,
	_In_ int       nCmdShow
) {
	WNDCLASSEX wcex;

	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.style = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc = WndProc;
	wcex.cbClsExtra = 0;
	wcex.cbWndExtra = 0;
	wcex.hInstance = hInstance;
	wcex.hIcon = LoadIcon(hInstance, IDI_APPLICATION);
	wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
	wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wcex.lpszMenuName = NULL;
	wcex.lpszClassName = szWindowClass;
	wcex.hIconSm = LoadIcon(wcex.hInstance, IDI_APPLICATION);

	if (!RegisterClassEx(&wcex))
	{
		MessageBox(NULL,
			_T("Call to RegisterClassEx failed!"),
			_T("Windows Desktop Guided Tour"),
			NULL);

		return 1;
	}

	// Store instance handle in our global variable
	hInst = hInstance;

	HWND hWnd = CreateWindow(
		szWindowClass,
		szTitle,
		WS_ICONIC,
		CW_USEDEFAULT, CW_USEDEFAULT,
		280, 280,
		NULL,
		NULL,
		hInstance,
		NULL
	);

	if (!hWnd)
	{
		MessageBox(NULL,
			_T("Call to CreateWindow failed!"),
			_T("Windows Desktop Guided Tour"),
			NULL);

		return 1;
	}

	HWND hwndButton = CreateWindow(
		L"BUTTON",  // Predefined class; Unicode assumed 
		L"Capture",      // Button text 
		WS_VISIBLE | WS_CHILD,  // Styles 
		10,         // x position 
		10,         // y position 
		240,        // Button width
		220,        // Button height
		hWnd,     // Parent window
		(HMENU)BTN_CPT,       // No menu.
		(HINSTANCE)GetWindowLongPtr(hWnd, GWLP_HINSTANCE),
		NULL);      // Pointer not needed.

	ShowWindow(hWnd, nCmdShow);
	UpdateWindow(hWnd);

	// Main message loop:
	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	return (int)msg.wParam;
};

LRESULT CALLBACK WndProc(
	_In_ HWND   hWnd,
	_In_ UINT   message,
	_In_ WPARAM wParam,
	_In_ LPARAM lParam
) {
	PAINTSTRUCT ps;
	HDC hdc;
	TCHAR greeting[] = _T("Hello, Windows desktop!");

	switch (message)
	{
	case WM_COMMAND: {
		if (LOWORD(wParam) == BTN_CPT) {
			OutputDebugStringW(L"Capture\n");
			Grab60FPS(120);
		}
		break;
	}
	case WM_INITDIALOG: {
		SetLayeredWindowAttributes(hWnd, RGB(0, 0, 0), 0, LWA_ALPHA);
		RedrawWindow(hWnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE);
		return true;
	}
	case WM_DESTROY:
		PostQuitMessage(0);
		break;
	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
		break;
	}

	return 0;
};