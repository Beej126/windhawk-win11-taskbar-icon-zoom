// ==WindhawkMod==
// @id              win11-taskbar-icon-zoom
// @name            Windows 11 Taskbar Icon Zoom
// @description     Adds a magnification effect to Windows 11 taskbar icons when hovered like the macOS Dock.
// @version         0.1
// @author          Beej
// @github          https://github.com/beej126/windhawk-win11-taskbar-icon-zoom
// @homepage        beej126.github.io
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject
// ==/WindhawkMod==

// Source code is published under The GNU General Public License v3.0.
//
// For bug reports and feature requests, please open an issue here:
// https://github.com/ramensoftware/windhawk-mods/issues
//
// For pull requests, development takes place here:
// https://github.com/m417z/my-windhawk-mods

// ==WindhawkModReadme==
/*
# Windows 11 Taskbar Icon Zoom

Adds a magnification effect to Windows 11 taskbar icons when hovered, similar to the
macOS Dock. When you hover over taskbar icons, they'll smoothly enlarge along with
nearby icons to create a fluid magnification effect.

Customize the magnification amount and range in the settings.

Only Windows 11 is supported.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- zoomPercentage: 150
  $name: Zoom percentage
  $description: >-
    The maximum magnification percentage (e.g. 150 means 1.5x normal size)
- zoomRange: 3
  $name: Zoom range
  $description: >-
    Number of icons to either side that will receive reduced magnification
- taskbarFrameClass: "Taskbar.TaskbarFrame"
  $name: Taskbar frame class name
  $description: >-
    The class name of the taskbar frame element. Use UWPSpy to find the correct class name for your Windows version.
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>

#include <atomic>
#include <cmath> // For std::fmin and std::fmax

#undef GetCurrentTime

#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.Composition.h>

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Hosting;
using namespace winrt::Windows::Foundation::Numerics;
using namespace winrt::Windows::UI::Composition;

struct
{
    int zoomPercentage;
    int zoomRange;
    std::wstring taskbarFrameClass;
} g_settings;

struct IconInfo
{
    FrameworkElement element;
    double leftX, rightX, centerX;
    double width;
    int index;
};

std::atomic<bool> g_taskbarViewDllLoaded;
double g_currentMouseX = 0;
bool g_magnificationActive = false;
bool g_applyingMagnification = false; // Flag to prevent re-entrance during magnification
DWORD g_lastMagnificationTime = 0;    // Timestamp of last magnification to prevent rapid fire
std::vector<std::pair<FrameworkElement, double>> g_affectedIcons;
std::vector<IconInfo> g_iconSpatialMap;
double g_totalRangeLeft = 0.0;
double g_totalRangeRight = 0.0;
bool g_spatialMapInitialized = false;
double g_maxZoom = 1.5;      // Default, will be set from zoomPercentage
double g_siblingsZoom = 1.2; // Default, will be calculated based on maxZoom

FrameworkElement FindChildElementByName(FrameworkElement parent, PCWSTR targetName);

// Build spatial map of all icons on first hover
bool BuildIconSpatialMap(FrameworkElement taskbarFrame)
{
    g_iconSpatialMap.clear();
    if (!taskbarFrame)
        return false;

    FrameworkElement rootGrid = FindChildElementByName(taskbarFrame, L"RootGrid");
    if (!rootGrid)
        return false;
    FrameworkElement iconContainer = FindChildElementByName(rootGrid, L"TaskbarFrameRepeater");
    if (!iconContainer)
        return false;

    int childCount = Media::VisualTreeHelper::GetChildrenCount(iconContainer);
    g_totalRangeLeft = 999999.0;
    g_totalRangeRight = -999999.0;

    for (int i = 0; i < childCount; i++)
    {
        auto child = Media::VisualTreeHelper::GetChild(iconContainer, i).try_as<FrameworkElement>();
        if (!child)
            continue;

        auto elementName = child.Name();
        if (elementName == L"AugmentedEntryPointButton")
            continue; // Skip weather widget

        auto transform = child.TransformToVisual(iconContainer);
        auto iconPos = transform.TransformPoint({0, 0});
        double width = child.ActualWidth();

        if (width <= 0 || iconPos.X < -100 || iconPos.X > 10000)
            continue;

        IconInfo info{child, iconPos.X, iconPos.X + width, iconPos.X + (width / 2.0), width, i};
        g_iconSpatialMap.push_back(info);

        g_totalRangeLeft = std::fmin(g_totalRangeLeft, info.leftX);
        g_totalRangeRight = std::fmax(g_totalRangeRight, info.rightX);
    }

    if (g_iconSpatialMap.empty())
    {
        MessageBox(nullptr, (L"Couldn't find icons under expected hierarchy: " + g_settings.taskbarFrameClass + L" > RootGrid > TaskbarFrameRepeater").c_str(), L"Fatal Error", MB_OK | MB_ICONERROR);
        return false;
    }

    g_spatialMapInitialized = true;
    return true;
}

// Reset magnification effect for all affected icons
void ResetMagnificationEffect(bool fullReset = false)
{
    auto iconsCopy = g_affectedIcons; // auto + assign is indeed a deep copy so it's safe from g_affectedIcons being changed on another thread

    g_affectedIcons.clear();
    g_magnificationActive = false;

    for (auto &[icon, scaleFactor] : iconsCopy)
    {
        if (!icon)
            continue;

        auto transform = icon.RenderTransform();
        if (transform)
        {
            auto scaleTransform = transform.try_as<Media::ScaleTransform>();
            if (scaleTransform)
            {
                scaleTransform.ScaleX(1.0);
                scaleTransform.ScaleY(1.0);
                Canvas::SetZIndex(icon, 0);
                icon.RenderTransformOrigin({0.5f, 0.5f});
            }
        }
    }

    g_currentMouseX = 0;
    g_affectedIcons.clear();
    g_magnificationActive = false;
    g_applyingMagnification = false;
    g_lastMagnificationTime = 0;
    g_iconSpatialMap.clear();
    g_totalRangeLeft = 0.0;
    g_totalRangeRight = 0.0;
    g_spatialMapInitialized = false;
}

void ApplyMagnificationFromMousePosition(FrameworkElement element, double mouseX)
{
    // Throttle mouse movements to avoid excessive updates
    DWORD currentTime = GetTickCount();
    if (currentTime - g_lastMagnificationTime < 16)
        return; // ~60 FPS update rate

    g_lastMagnificationTime = currentTime;

    if (g_currentMouseX > 0)
    {
        double deltaX = std::abs(mouseX - g_currentMouseX);
        if (deltaX < 1.0)
            return;
    }

    if (!g_spatialMapInitialized)
        g_spatialMapInitialized = BuildIconSpatialMap(element);

    g_currentMouseX = mouseX;
    bool mouseInTaskbarArea = (mouseX >= g_totalRangeLeft - 50 && mouseX <= g_totalRangeRight + 50);
    if (!mouseInTaskbarArea)
    {
        ResetMagnificationEffect(false);
        return;
    }

    g_affectedIcons.clear();

    double maxZoom = g_settings.zoomPercentage / 100.0;
    int maxRange = g_settings.zoomRange;

    // Calculate average icon width for influence distance
    double totalWidth = 0.0;
    int validIcons = 0;
    for (const auto &iconInfo : g_iconSpatialMap)
    {
        if (iconInfo.element && iconInfo.width > 0)
        {
            totalWidth += iconInfo.width;
            validIcons++;
        }
    }
    double averageIconWidth = validIcons > 0 ? totalWidth / validIcons : 68.0;
    double maxInfluenceDistance = maxRange * averageIconWidth;

    // Process all icons in one loop - apply magnification or reset as needed
    for (size_t i = 0; i < g_iconSpatialMap.size(); i++)
    {
        const auto &iconInfo = g_iconSpatialMap[i];
        auto icon = iconInfo.element;
        if (!icon)
            continue;

        auto visibility = icon.Visibility();
        if (visibility != Visibility::Visible)
            continue;

        auto bounds = icon.ActualWidth();
        if (bounds <= 0 || bounds > 1000)
            continue;

        double distanceToIcon = std::abs(iconInfo.centerX - mouseX);
        double halfIconWidth = iconInfo.width / 2.0;
        float scaleFactor = 1.0;

        // Calculate zoom factor if within influence range
        if (distanceToIcon <= maxInfluenceDistance)
        {
            if (distanceToIcon <= halfIconWidth)
            {
                scaleFactor = maxZoom;
            }
            else
            {
                double falloffRatio = (distanceToIcon - halfIconWidth) / (maxInfluenceDistance - halfIconWidth);
                falloffRatio = std::fmin(1.0, std::fmax(0.0, falloffRatio));
                scaleFactor = maxZoom - ((maxZoom - 1.0) * falloffRatio);
                scaleFactor = std::fmax(1.0, scaleFactor);
            }
        }
        int zIndex = scaleFactor > 1.1 ? 100 + static_cast<int>((scaleFactor - 1.0) * 100) : 0;

        Media::ScaleTransform scaleTransform = nullptr;
        auto existingTransform = icon.RenderTransform();
        if (existingTransform)
        {
            scaleTransform = existingTransform.try_as<Media::ScaleTransform>();
        }
        if (!scaleTransform)
        {
            scaleTransform = Media::ScaleTransform();
            icon.RenderTransform(scaleTransform);
        }
        // Set transform origin to bottom-center so icons grow upward from baseline
        icon.RenderTransformOrigin({0.5f, scaleFactor > 1.01 ? 1.0f : 0.5f});

        //here's the beef!
        scaleTransform.ScaleX(scaleFactor);
        scaleTransform.ScaleY(scaleFactor);

        Canvas::SetZIndex(icon, zIndex);

        // composition approach to avoid clipping didn't pan out...
        //   finally AI admitted that nothing is going to render outside of the taskbar HWND
        //   recommended approach is to create another container with unconstrained dimensions and copy all the icons there where they can fully display

        // auto visual = ElementCompositionPreview::GetElementVisual(icon);
        // auto size = icon.ActualSize(); // Or use known width/height
        // float centerY = (scaleFactor > 1.01f) ? size.x : size.y / 2.0f;
        // visual.CenterPoint({size.x / 2.0f, centerY, 0.0f});

        // visual.Scale({scaleFactor, scaleFactor, 1.0f});

        // Track affected icons for cleanup
        if (scaleFactor > 1.01)
        {
            g_affectedIcons.push_back({icon, scaleFactor});
        }
    }

    g_magnificationActive = !g_affectedIcons.empty();
}

/************************************************************************************************************
 * hooking mouse move */

using TaskbarFrame_OnPointerMoved_t = int(WINAPI *)(void *pThis, void *pArgs);
static TaskbarFrame_OnPointerMoved_t OnPointerMoved_Original;

using TaskbarFrame_OnPointerExited_t = int(WINAPI *)(void *pThis, void *pArgs);
static TaskbarFrame_OnPointerExited_t OnPointerExited_Original;

int WINAPI OnPointerMoved_Hook(void *pThis, void *pArgs)
{
    FrameworkElement element = nullptr;
    ((IUnknown *)pThis)->QueryInterface(winrt::guid_of<FrameworkElement>(), winrt::put_abi(element));

    Input::PointerRoutedEventArgs args = nullptr;
    ((IUnknown *)pArgs)->QueryInterface(winrt::guid_of<Input::PointerRoutedEventArgs>(), winrt::put_abi(args));

    if (element && args)
    {
        auto className = winrt::get_class_name(element);

        if (className == g_settings.taskbarFrameClass)
        {
            auto pointerPos = args.GetCurrentPoint(element).Position();
            ApplyMagnificationFromMousePosition(element, pointerPos.X);
        }
    }

    return OnPointerMoved_Original(pThis, pArgs);
}

int WINAPI OnPointerExited_Hook(void *pThis, void *pArgs)
{
    ResetMagnificationEffect(true);
    return OnPointerExited_Original(pThis, pArgs);
}

bool HookTaskbarViewDllSymbols(HMODULE module)
{
    if (g_settings.taskbarFrameClass.empty())
        g_settings.taskbarFrameClass = L"Taskbar.TaskbarFrame";

    std::wstring configuredClass = g_settings.taskbarFrameClass;
    std::wstring namespacePart = L"Taskbar";
    std::wstring classPart = L"TaskbarFrame";

    size_t dotPos = configuredClass.find(L'.');
    if (dotPos != std::wstring::npos)
    {
        namespacePart = configuredClass.substr(0, dotPos);
        classPart = configuredClass.substr(dotPos + 1);
    }

    WindhawkUtils::SYMBOL_HOOK taskbarFrameHooks[] = {
        {
            {(LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::)" +
              namespacePart + LR"(::implementation::)" + classPart +
              LR"(,struct winrt::Windows::UI::Xaml::Controls::IControlOverrides>::OnPointerMoved(void *))")
                 .c_str()},
            (void **)&OnPointerMoved_Original,
            (void *)OnPointerMoved_Hook,
        },
        {
            {(LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::)" +
              namespacePart + LR"(::implementation::)" + classPart +
              LR"(,struct winrt::Windows::UI::Xaml::Controls::IControlOverrides>::OnPointerExited(void *))")
                 .c_str()},
            (void **)&OnPointerExited_Original,
            (void *)OnPointerExited_Hook,
        }};

    return HookSymbols(module, taskbarFrameHooks, ARRAYSIZE(taskbarFrameHooks));
}

HMODULE GetTaskbarViewModuleHandle()
{
    HMODULE module = GetModuleHandle(L"Taskbar.View.dll");
    if (!module)
    {
        module = GetModuleHandle(L"ExplorerExtensions.dll");
    }

    return module;
}

void HandleLoadedModuleIfTaskbarView(HMODULE module, LPCWSTR lpLibFileName)
{
    if (!g_taskbarViewDllLoaded && GetTaskbarViewModuleHandle() == module &&
        !g_taskbarViewDllLoaded.exchange(true))
    {
        Wh_Log(L"Loaded %s", lpLibFileName);

        if (HookTaskbarViewDllSymbols(module))
            Wh_ApplyHookOperations();
    }
}

using LoadLibraryExW_t = decltype(&LoadLibraryExW);
LoadLibraryExW_t LoadLibraryExW_Original;
HMODULE WINAPI LoadLibraryExW_Hook(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
{
    HMODULE module = LoadLibraryExW_Original(lpLibFileName, hFile, dwFlags);
    if (module)
        HandleLoadedModuleIfTaskbarView(module, lpLibFileName);
    return module;
}

/*****************************************************************************************************************************************
 * module lifecycle
 */
void LoadSettings()
{
    g_settings.zoomPercentage = Wh_GetIntSetting(L"zoomPercentage");
    g_settings.zoomRange = Wh_GetIntSetting(L"zoomRange");
    g_settings.taskbarFrameClass = Wh_GetStringSetting(L"taskbarFrameClass");
    g_maxZoom = g_settings.zoomPercentage / 100.0;
    g_siblingsZoom = 1.0 + ((g_maxZoom - 1.0) * 0.5);
}

BOOL Wh_ModInit()
{
    LoadSettings();
    g_magnificationActive = false;
    g_applyingMagnification = false;
    g_lastMagnificationTime = 0;

    if (HMODULE taskbarViewModule = GetTaskbarViewModuleHandle())
    {
        g_taskbarViewDllLoaded = true;
        if (!HookTaskbarViewDllSymbols(taskbarViewModule))
        {
            Wh_Log(L"Failed to hook Taskbar.View.dll symbols");
            return FALSE;
        }
    }
    else
    {
        Wh_Log(L"Taskbar view module not loaded yet, will hook on load");

        HMODULE kernelBaseModule = GetModuleHandle(L"kernelbase.dll");
        if (!kernelBaseModule)
            return FALSE;

        auto pKernelBaseLoadLibraryExW = (decltype(&LoadLibraryExW))GetProcAddress(kernelBaseModule, "LoadLibraryExW");
        if (!pKernelBaseLoadLibraryExW)
            return FALSE;

        WindhawkUtils::SYMBOL_HOOK kernelBaseHooks[] = {
            {{L"LoadLibraryExW", LR"(LoadLibraryExW)", LR"(__imp_LoadLibraryExW)"},
             (void **)&LoadLibraryExW_Original,
             (void *)LoadLibraryExW_Hook}};

        if (!HookSymbols(kernelBaseModule, kernelBaseHooks, ARRAYSIZE(kernelBaseHooks)))
            return FALSE;
    }

    Wh_Log(L"Successfully initialized.");

    return TRUE;
}

void Wh_ModAfterInit()
{
    HMODULE taskbarViewModule = GetTaskbarViewModuleHandle();
    if (
        !g_taskbarViewDllLoaded && !g_taskbarViewDllLoaded.exchange(true) && HookTaskbarViewDllSymbols(taskbarViewModule))
    {
        Wh_ApplyHookOperations();
    }
}

void Wh_ModUninit()
{
    ResetMagnificationEffect(true);
}

void Wh_ModSettingsChanged()
{
    ResetMagnificationEffect(true);
    LoadSettings();
}

/***************************************************************************************************************************
 * helpers
 */

// Helper function to find a child element by name
FrameworkElement FindChildElementByName(FrameworkElement parent, PCWSTR targetName)
{
    if (!parent)
        return nullptr;

    int childCount = Media::VisualTreeHelper::GetChildrenCount(parent);
    for (int i = 0; i < childCount; i++)
    {
        auto child = Media::VisualTreeHelper::GetChild(parent, i).try_as<FrameworkElement>();
        if (child && child.Name() == targetName)
            return child;
    }
    return nullptr;
}
