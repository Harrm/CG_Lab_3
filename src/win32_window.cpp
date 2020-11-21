#include "win32_window.h"

HWND Win32Window::hwnd = nullptr;

int Win32Window::Run(Renderer* pRenderer, HINSTANCE hInstance, int nCmdShow)
{
	WNDCLASSEX window_class{};
	window_class.cbSize = sizeof(window_class);
	window_class.style = CS_HREDRAW | CS_VREDRAW;
	window_class.lpfnWndProc = WindowProc;
	window_class.hInstance = hInstance;
	window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
	window_class.lpszClassName = L"DXSampleClass";
	RegisterClassEx(&window_class);
	RECT window_rect = { 
		.left = 0l, 
		.top = 0l,
		.right = static_cast<LONG>(pRenderer->GetWidth()),
		.bottom = static_cast<LONG>(pRenderer->GetHeight()) };
	AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, false);
	hwnd = CreateWindow(
		window_class.lpszClassName,
		pRenderer->GetTitle(),
		WS_OVERLAPPEDWINDOW, 
		CW_USEDEFAULT, CW_USEDEFAULT, 
		window_rect.right - window_rect.left,
		window_rect.bottom - window_rect.top, 
		nullptr, nullptr, hInstance, pRenderer);
	pRenderer->OnInit();
	ShowWindow(hwnd, nCmdShow);

	MSG msg{};
	while (msg.message != WM_QUIT) {
		if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}
	return static_cast<int>(msg.wParam);
}

LRESULT Win32Window::WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	Renderer* pRender = reinterpret_cast<Renderer*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));

	switch (message)
	{
	case WM_CREATE:
	{
		// Save the Renderer* passed in to CreateWindow.
		LPCREATESTRUCT pCreateStruct = reinterpret_cast<LPCREATESTRUCT>(lParam);
		SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pCreateStruct->lpCreateParams));
	}	return 0;
	case WM_PAINT:
		if (pRender) {
			pRender->OnUpdate();
			pRender->OnRender();
		}
		return 0;
	case WM_KEYDOWN:
		if (pRender) {
			pRender->OnKeyDown(static_cast<UINT8>(wParam));
		}
		return 0;

	case WM_KEYUP:
		if (pRender) {
			pRender->OnKeyUp(static_cast<UINT8>(wParam));
		}
		return 0;

	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}

	// Handle any messages the switch statement didn't.
	return DefWindowProc(hWnd, message, wParam, lParam);
}
