// =============================================================================
// dllmain.cpp - KH_WGCCapture (Threaded + message pump)
// 功能：后台线程 WGC 采集，立即暴露分辨率，双缓冲提供像素数据
// 版本：增加了消息循环以处理 WinRT 事件，确保 FrameArrived 回调触发
// =============================================================================

#include <windows.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <thread>
#include <atomic>
#include <mutex>

// WinRT / WGC
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

// D3D11 / DXGI
#include <d3d11.h>
#include <dxgi1_2.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowsapp.lib")

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

// -----------------------------------------------------------------------------
// 日志（写入 %TEMP%\wgc_log.txt）
// -----------------------------------------------------------------------------
static std::string GetLogPath()
{
    char tempPath[MAX_PATH];
    GetTempPathA(MAX_PATH, tempPath);
    return std::string(tempPath) + "wgc_log.txt";
}

static void WriteLog(const std::string& msg)
{
    std::ofstream log(GetLogPath(), std::ios::app);
    log << msg << std::endl;
}

static void ClearLog()
{
    std::ofstream log(GetLogPath(), std::ios::trunc);
    log << "=== WGC Log ===" << std::endl;
}

// -----------------------------------------------------------------------------
// 线程安全的全局状态
// -----------------------------------------------------------------------------
static std::thread                g_workerThread;
static std::atomic<bool>          g_workerRunning{ false };
static std::atomic<bool>          g_initialized{ false };      // 采集会话已建立
static std::atomic<int>           g_width{ 0 };                 // 当前分辨率（原子）
static std::atomic<int>           g_height{ 0 };
static std::atomic<bool>          g_newFrame{ false };

// 双缓冲像素数据
static std::vector<uint8_t>       g_frontBuffer;
static std::vector<uint8_t>       g_backBuffer;
static std::mutex                  g_bufferMutex;

// 后台线程私有上下文（仅工作线程访问）
struct WorkerContext
{
    com_ptr<ID3D11Device>          device;
    com_ptr<ID3D11DeviceContext>   context;
    IDirect3DDevice                rtDevice{ nullptr };
    Direct3D11CaptureFramePool      framePool{ nullptr };
    GraphicsCaptureSession          session{ nullptr };
    GraphicsCaptureItem             item{ nullptr };
    com_ptr<ID3D11Texture2D>        stagingTex;
    int                             width = 0;
    int                             height = 0;
};
static WorkerContext* g_ctx = nullptr;  // 仅工作线程读写

// 帧计数器，用于限制日志频率
static std::atomic<int> s_frameCounter{ 0 };

// -----------------------------------------------------------------------------
// 辅助：从 D3D11 设备创建 WinRT 设备
// -----------------------------------------------------------------------------
static IDirect3DDevice CreateRtDevice(ID3D11Device* device)
{
    com_ptr<IDXGIDevice> dxgiDevice;
    device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()));
    com_ptr<IInspectable> inspectable;
    check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));
    return inspectable.as<IDirect3DDevice>();
}

// -----------------------------------------------------------------------------
// 后台工作线程
// -----------------------------------------------------------------------------
static void WorkerThreadProc(int adapterIndex)
{
    WriteLog("WorkerThread started, adapterIndex=" + std::to_string(adapterIndex));

    // 强制初始化为 MTA（解决 0x80010106）
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    WriteLog("Worker: init_apartment OK");

    WorkerContext ctx;
    g_ctx = &ctx;

    try
    {
        // ---- 1. 枚举适配器 ----
        com_ptr<IDXGIFactory1> factory;
        check_hresult(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())));

        com_ptr<IDXGIAdapter1> selectedAdapter;
        int realIndex = 0;
        com_ptr<IDXGIAdapter1> adapter;
        UINT i = 0;
        while (factory->EnumAdapters1(i++, adapter.put()) != DXGI_ERROR_NOT_FOUND)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { adapter = nullptr; continue; }
            if (realIndex == adapterIndex)
            {
                selectedAdapter = adapter;
                break;
            }
            realIndex++;
            adapter = nullptr;
        }
        if (!selectedAdapter)
            throw std::runtime_error("Adapter not found");

        // ---- 2. 创建 D3D11 设备 ----
        D3D_FEATURE_LEVEL featureLevel;
        check_hresult(D3D11CreateDevice(
            selectedAdapter.get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0,
            D3D11_SDK_VERSION,
            ctx.device.put(),
            &featureLevel,
            ctx.context.put()
        ));
        ctx.rtDevice = CreateRtDevice(ctx.device.get());

        // ---- 3. 获取主显示器 ----
        HMONITOR hmon = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        if (!hmon)
            throw std::runtime_error("No monitor found");
        WriteLog("Worker: Monitor obtained");

        // ---- 4. 创建 CaptureItem ----
        auto interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        check_hresult(interop->CreateForMonitor(
            hmon,
            guid_of<GraphicsCaptureItem>(),
            put_abi(ctx.item)
        ));
        WriteLog("Worker: CaptureItem created");

        // ---- 5. 获取分辨率并立即更新全局变量 ----
        auto size = ctx.item.Size();
        ctx.width = size.Width;
        ctx.height = size.Height;
        g_width = ctx.width;      // 立即暴露，供 WGC_GetSize 使用
        g_height = ctx.height;
        WriteLog("Worker: initial resolution = " + std::to_string(ctx.width) + "x" + std::to_string(ctx.height));

        // ---- 6. 创建 staging 纹理 ----
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = ctx.width;
        desc.Height = ctx.height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ctx.device->CreateTexture2D(&desc, nullptr, ctx.stagingTex.put());
        WriteLog("Worker: staging texture created");

        // ---- 7. 创建帧池并注册回调 ----
        ctx.framePool = Direct3D11CaptureFramePool::Create(
            ctx.rtDevice,
            DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2,
            size
        );
        WriteLog("Worker: FramePool created");

        ctx.framePool.FrameArrived([](Direct3D11CaptureFramePool const& pool, winrt::Windows::Foundation::IInspectable const&)
            {
                int frameNum = ++s_frameCounter;
                // 每30帧写一次日志，避免日志过多
                if (frameNum % 30 == 0)
                    WriteLog("FrameArrived: #" + std::to_string(frameNum));

                WorkerContext* pCtx = g_ctx;
                if (!pCtx)
                {
                    if (frameNum % 30 == 0) WriteLog("FrameArrived: pCtx null");
                    return;
                }

                auto frame = pool.TryGetNextFrame();
                if (!frame)
                {
                    if (frameNum % 30 == 0) WriteLog("FrameArrived: frame null");
                    return;
                }

                auto surface = frame.Surface();
                com_ptr<IDXGISurface> dxgiSurface;
                {
                    auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                    access->GetInterface(IID_PPV_ARGS(dxgiSurface.put()));
                }
                com_ptr<ID3D11Texture2D> srcTex;
                dxgiSurface->QueryInterface(IID_PPV_ARGS(srcTex.put()));

                // 分辨率变化处理
                auto size = frame.ContentSize();
                if (size.Width != pCtx->width || size.Height != pCtx->height)
                {
                    if (frameNum % 30 == 0) WriteLog("FrameArrived: resolution changed to " + std::to_string(size.Width) + "x" + std::to_string(size.Height));
                    // 重建 staging 纹理
                    pCtx->stagingTex = nullptr;
                    D3D11_TEXTURE2D_DESC desc{};
                    desc.Width = size.Width;
                    desc.Height = size.Height;
                    desc.MipLevels = 1;
                    desc.ArraySize = 1;
                    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    desc.SampleDesc.Count = 1;
                    desc.Usage = D3D11_USAGE_STAGING;
                    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    pCtx->device->CreateTexture2D(&desc, nullptr, pCtx->stagingTex.put());
                    pCtx->width = size.Width;
                    pCtx->height = size.Height;

                    // 更新全局分辨率
                    g_width = size.Width;
                    g_height = size.Height;

                    // 调整帧池
                    pool.Recreate(pCtx->rtDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
                }

                // 拷贝到 staging 纹理
                pCtx->context->CopyResource(pCtx->stagingTex.get(), srcTex.get());

                // 将 staging 纹理数据读入系统内存（双缓冲）
                D3D11_MAPPED_SUBRESOURCE mapped{};
                HRESULT hr = pCtx->context->Map(pCtx->stagingTex.get(), 0, D3D11_MAP_READ, 0, &mapped);
                if (SUCCEEDED(hr))
                {
                    if (frameNum % 30 == 0) WriteLog("FrameArrived: Map succeeded");

                    int w = pCtx->width;
                    int h = pCtx->height;
                    size_t dataSize = w * h * 4;

                    if (g_backBuffer.size() != dataSize)
                        g_backBuffer.resize(dataSize);

                    uint8_t* dst = g_backBuffer.data();
                    for (int y = 0; y < h; ++y)
                    {
                        const uint8_t* src = (const uint8_t*)mapped.pData + y * mapped.RowPitch;
                        memcpy(dst + y * w * 4, src, w * 4);
                    }
                    pCtx->context->Unmap(pCtx->stagingTex.get(), 0);

                    // 交换前后缓冲区
                    {
                        std::lock_guard<std::mutex> lock(g_bufferMutex);
                        std::swap(g_frontBuffer, g_backBuffer);
                    }
                    g_newFrame = true;

                    if (frameNum % 30 == 0) WriteLog("FrameArrived: newFrame set");
                }
                else
                {
                    if (frameNum % 30 == 0) WriteLog("FrameArrived: Map failed, HRESULT=0x" + [&]() {
                        std::ostringstream ss; ss << std::hex << hr; return ss.str(); }());
                }
            });

        // ---- 8. 开始采集 ----
        ctx.session = ctx.framePool.CreateCaptureSession(ctx.item);
        ctx.session.IsBorderRequired(false);
        ctx.session.StartCapture();
        WriteLog("Worker: Capture started");

        g_initialized = true;
        WriteLog("Worker: WGC capture started successfully");
    }
    catch (const winrt::hresult_error& e)
    {
        std::ostringstream ss;
        ss << "Worker: WinRT exception HRESULT=0x" << std::hex << (uint32_t)e.code();
        WriteLog(ss.str());
        g_initialized = false;
    }
    catch (const std::exception& e)
    {
        WriteLog("Worker: std::exception " + std::string(e.what()));
        g_initialized = false;
    }

    // ---------- 关键修改：添加消息循环 ----------
    // 创建退出事件
    HANDLE hExitEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    // 消息循环 + 退出事件等待
    MSG msg;
    while (g_workerRunning)
    {
        // 处理所有待处理的消息（非阻塞）
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        // 等待退出事件，超时 10ms 以便及时响应新消息
        WaitForSingleObject(hExitEvent, 10);
    }
    CloseHandle(hExitEvent);

    // 清理资源
    ctx.session = nullptr;
    ctx.framePool = nullptr;
    ctx.item = nullptr;
    ctx.rtDevice = nullptr;
    ctx.stagingTex = nullptr;
    ctx.context = nullptr;
    ctx.device = nullptr;

    winrt::uninit_apartment();
    WriteLog("Worker thread exiting");
}

// -----------------------------------------------------------------------------
// 导出函数（全部 __stdcall）
// -----------------------------------------------------------------------------
extern "C"
{
    __declspec(dllexport) int __stdcall WGC_GetAdapterCount()
    {
        com_ptr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())))) return 0;
        int count = 0;
        com_ptr<IDXGIAdapter1> adapter;
        UINT i = 0;
        while (factory->EnumAdapters1(i++, adapter.put()) != DXGI_ERROR_NOT_FOUND)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                count++;
            adapter = nullptr;
        }
        return count;
    }

    __declspec(dllexport) void __stdcall WGC_GetAdapterName(int index, wchar_t* outName, int bufLen)
    {
        com_ptr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())))) return;
        int realIndex = 0;
        com_ptr<IDXGIAdapter1> adapter;
        UINT i = 0;
        while (factory->EnumAdapters1(i++, adapter.put()) != DXGI_ERROR_NOT_FOUND)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { adapter = nullptr; continue; }
            if (realIndex == index)
            {
                wcsncpy_s(outName, bufLen, desc.Description, _TRUNCATE);
                return;
            }
            realIndex++;
            adapter = nullptr;
        }
    }

    __declspec(dllexport) int __stdcall WGC_GetDisplayAdapter()
    {
        com_ptr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())))) return 0;
        int realIndex = 0;
        com_ptr<IDXGIAdapter1> adapter;
        UINT i = 0;
        while (factory->EnumAdapters1(i++, adapter.put()) != DXGI_ERROR_NOT_FOUND)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { adapter = nullptr; continue; }
            com_ptr<IDXGIOutput> output;
            if (adapter->EnumOutputs(0, output.put()) != DXGI_ERROR_NOT_FOUND)
                return realIndex;
            realIndex++;
            adapter = nullptr;
        }
        return 0;
    }

    __declspec(dllexport) int __stdcall WGC_Init(int adapterIndex)
    {
        ClearLog();
        WriteLog("WGC_Init called, adapterIndex=" + std::to_string(adapterIndex));

        if (g_workerRunning)
        {
            WriteLog("Worker already running");
            return g_initialized ? 1 : 0;
        }

        g_workerRunning = true;
        g_initialized = false;

        // 启动后台线程
        g_workerThread = std::thread(WorkerThreadProc, adapterIndex);

        // 等待初始化完成且分辨率有效（最多 5 秒）
        int waitCount = 0;
        while (!g_initialized && waitCount < 50)
        {
            Sleep(100);
            waitCount++;
        }
        if (!g_initialized)
        {
            WriteLog("WGC_Init failed (init timeout)");
            return 0;
        }

        // 再等待分辨率有效（最多 1 秒）
        waitCount = 0;
        while (g_width == 0 && waitCount < 10)
        {
            Sleep(100);
            waitCount++;
        }

        if (g_width == 0)
            WriteLog("Warning: resolution still 0 after init, but continuing");
        else
            WriteLog("WGC_Init success, resolution=" + std::to_string(g_width) + "x" + std::to_string(g_height));

        return 1;
    }

    __declspec(dllexport) void __stdcall WGC_GetSize(int* w, int* h)
    {
        *w = g_width;
        *h = g_height;
    }

    __declspec(dllexport) int __stdcall WGC_GetFrame(unsigned char* buffer, int bufferSize)
    {
        if (!g_initialized || !g_newFrame) return 0;

        int w = g_width;
        int h = g_height;
        int required = w * h * 4;
        if (bufferSize < required) return 0;

        {
            std::lock_guard<std::mutex> lock(g_bufferMutex);
            if (g_frontBuffer.empty() || g_frontBuffer.size() != required)
                return 0;
            memcpy(buffer, g_frontBuffer.data(), required);
        }

        g_newFrame = false;
        return 1;
    }

    __declspec(dllexport) int __stdcall WGC_GetFrameCenter(unsigned char* buffer, int cropSize)
    {
        if (!g_initialized || !g_newFrame) return 0;

        int w = g_width;
        int h = g_height;
        if (cropSize > w || cropSize > h) return 0;

        int required = cropSize * cropSize * 4;
        int startX = (w - cropSize) / 2;
        int startY = (h - cropSize) / 2;

        {
            std::lock_guard<std::mutex> lock(g_bufferMutex);
            if (g_frontBuffer.empty() || g_frontBuffer.size() != w * h * 4)
                return 0;

            const uint8_t* src = g_frontBuffer.data();
            uint8_t* dst = buffer;
            for (int y = 0; y < cropSize; ++y)
            {
                const uint8_t* rowSrc = src + ((startY + y) * w + startX) * 4;
                memcpy(dst + y * cropSize * 4, rowSrc, cropSize * 4);
            }
        }

        g_newFrame = false;
        return 1;
    }

    __declspec(dllexport) void __stdcall WGC_Release()
    {
        WriteLog("WGC_Release called");
        g_workerRunning = false;
        g_initialized = false;

        if (g_workerThread.joinable())
            g_workerThread.join();

        {
            std::lock_guard<std::mutex> lock(g_bufferMutex);
            g_frontBuffer.clear();
            g_backBuffer.clear();
        }
        g_width = 0;
        g_height = 0;
        g_newFrame = false;
        WriteLog("WGC_Release done");
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    return TRUE;
}