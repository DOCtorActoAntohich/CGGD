#include "window.h"

#include <windowsx.h>

using namespace cg::utils;

HWND window::hwnd = nullptr;

bool window::pressed_w = false;
bool window::pressed_s = false;
bool window::pressed_a = false;
bool window::pressed_d = false;


constexpr int KeyCode_W = 87;
constexpr int KeyCode_S = 83;
constexpr int KeyCode_A = 65;
constexpr int KeyCode_D = 68;

constexpr float MOVEMENT_SPEED = 10.0f;


int cg::utils::window::run(cg::renderer::renderer* renderer, HINSTANCE hinstance, int ncmdshow)
{
    // Initialize the window class.
    WNDCLASSEX window_class = {};
    window_class.cbSize = sizeof(WNDCLASSEX);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = hinstance;
    window_class.hCursor = LoadCursor(NULL, IDC_ARROW);
    window_class.lpszClassName = L"DXSampleClass";
    RegisterClassEx(&window_class);

    // Create the window and store a handle to it.
    RECT window_rect = {0, 0, static_cast<LONG>(renderer->get_width()),
                        static_cast<LONG>(renderer->get_height())};
    AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindow(
            window_class.lpszClassName, L"DX12 renderer", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, window_rect.right - window_rect.left,
            window_rect.bottom - window_rect.top, nullptr, nullptr, hinstance, renderer);

    // Initialize the sample. OnInit is defined in each child-implementation of DXSample.
    renderer->init();
    ShowWindow(hwnd, ncmdshow);
    // Main sample loop.
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    renderer->destroy();
    // Return this part of the WM_QUIT message to Windows.
    return static_cast<int>(msg.wParam);
}

LRESULT cg::utils::window::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    cg::renderer::renderer* renderer = reinterpret_cast<cg::renderer::renderer*>(
            GetWindowLongPtr(hwnd, GWLP_USERDATA));

    switch (message) {
        case WM_CREATE: {
            // Save the Renderer* passed in to CreateWindow.
            LPCREATESTRUCT pCreateStruct = reinterpret_cast<LPCREATESTRUCT>(lparam);
            SetWindowLongPtr(
                    hwnd, GWLP_USERDATA,
                    reinterpret_cast<LONG_PTR>(pCreateStruct->lpCreateParams));
        }
            return 0;

        case WM_PAINT: {
            if (renderer) {
                if (window::pressed_w) {
                    renderer->move_forward(MOVEMENT_SPEED);
                }
                if (window::pressed_s) {
                    renderer->move_backward(MOVEMENT_SPEED);
                }
                if (window::pressed_a) {
                    renderer->move_left(MOVEMENT_SPEED);
                }
                if (window::pressed_d) {
                    renderer->move_right(MOVEMENT_SPEED);
                }
                renderer->update();
                renderer->render();
            }
        }
            return 0;

        case WM_KEYDOWN: {
            if (renderer) {
                switch (static_cast<UINT8>(wparam)) {
                    case KeyCode_W:
                        window::pressed_w = true;
                        break;
                    case KeyCode_S:
                        window::pressed_s = true;

                        break;
                    case KeyCode_A:
                        window::pressed_a = true;
                        break;
                    case KeyCode_D:
                        window::pressed_d = true;
                        break;
                }
            }
        }
            return 0;

        case WM_KEYUP: {
            if (renderer) {
                switch (static_cast<UINT8>(wparam)) {
                    case KeyCode_W:
                        window::pressed_w = false;
                        break;
                    case KeyCode_S:
                        window::pressed_s = false;
                        break;
                    case KeyCode_A:
                        window::pressed_a = false;
                        break;
                    case KeyCode_D:
                        window::pressed_d = false;
                        break;
                }
            }
        }
            return 0;

        case WM_MOUSEMOVE: {
            if (renderer) {
                short x_pos = GET_X_LPARAM(lparam);
                short y_pos = GET_Y_LPARAM(lparam);

                // Could've been better but I have no idea how to make it work.
                renderer->move_yaw(x_pos / 2.0f);
                renderer->move_pitch(-(y_pos / 2.0f));
            }
        }
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }

    // Handle any messages the switch statement didn't.
    return DefWindowProc(hwnd, message, wparam, lparam);
}
