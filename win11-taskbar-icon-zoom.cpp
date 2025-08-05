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
#include <functional>
#include <optional>
#include <sstream>
#include <set>       // For std::set
#include <algorithm> // For std::min and std::max
#include <limits>    // For std::numeric_limits
#include <cmath>     // For std::fmin and std::fmax

#undef GetCurrentTime

#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Controls.h>

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;

struct
{
    int zoomPercentage;
    int zoomRange;
    std::wstring taskbarFrameClass;
} g_settings;

std::atomic<bool> g_taskbarViewDllLoaded;

// Function declarations for essential operations
void SafeResetElements();
void ResetMagnificationEffect();

std::optional<winrt::Windows::Foundation::Point> g_currentPointerPos;
std::optional<FrameworkElement> g_hoveredTaskbarFrame;
bool g_magnificationActive;
bool g_applyingMagnification = false; // Flag to prevent re-entrance during magnification
DWORD g_lastMagnificationTime = 0;    // Timestamp of last magnification to prevent rapid fire
std::vector<std::pair<FrameworkElement, double>> g_affectedIcons;

// New approach: spatial mapping instead of hit testing
struct IconInfo
{
    FrameworkElement element;
    double leftX, rightX, centerX;
    double width;
    int index;
};
std::vector<IconInfo> g_iconSpatialMap;
double g_totalRangeLeft = 0.0;
double g_totalRangeRight = 0.0;
bool g_spatialMapInitialized = false;

// Helper function to check if an element is still valid
bool IsElementValid(const FrameworkElement &element)
{
    if (!element)
        return false;

    try
    {
        auto testVal = element.ActualWidth();
        return true;
    }
    catch (...)
    {
        return false;
    }
}

// Helper function to safely reset element references and state
void SafeResetElements()
{
    Wh_Log(L"SafeResetElements: Safely resetting all element references");

    try
    {
        // Reset magnification effects first
        try
        {
            ResetMagnificationEffect();
        }
        catch (...)
        {
            Wh_Log(L"SafeResetElements: Error while resetting magnification effect");
        }

        // Reset all state atomically to prevent partial cleanup
        g_currentPointerPos.reset();
        g_hoveredTaskbarFrame.reset();
        g_affectedIcons.clear();
        g_magnificationActive = false;
        g_applyingMagnification = false; // Clear the flag to prevent deadlock
        g_lastMagnificationTime = 0;     // Reset timing

        // Reset spatial mapping state
        g_iconSpatialMap.clear();
        g_totalRangeLeft = 0.0;
        g_totalRangeRight = 0.0;
        g_spatialMapInitialized = false;

        Wh_Log(L"SafeResetElements: Successfully reset all elements and state");
    }
    catch (const winrt::hresult_error &e)
    {
        Wh_Log(L"SafeResetElements: Error resetting elements: %s", e.message().c_str());
    }
    catch (...)
    {
        Wh_Log(L"SafeResetElements: Unknown error resetting elements");
    }
}

// Calculated values from settings
double g_maxZoom = 1.5;      // Default, will be set from zoomPercentage
double g_siblingsZoom = 1.2; // Default, will be calculated based on maxZoom

FrameworkElement EnumParentElements(
    FrameworkElement element,
    std::function<bool(FrameworkElement)> enumCallback)
{
    auto parent = element;
    while (true)
    {
        parent = Media::VisualTreeHelper::GetParent(parent)
                     .try_as<FrameworkElement>();
        if (!parent)
        {
            return nullptr;
        }

        if (enumCallback(parent))
        {
            return parent;
        }
    }
}

FrameworkElement GetAncestorElementByClassName(FrameworkElement element,
                                               PCWSTR className)
{
    return EnumParentElements(element, [className](FrameworkElement parent)
                              { return winrt::get_class_name(parent) == className; });
}

// Helper function to find a child element by name
FrameworkElement FindChildElementByName(FrameworkElement parent, PCWSTR targetName)
{
    if (!parent)
    {
        return nullptr;
    }

    try
    {
        int childCount = Media::VisualTreeHelper::GetChildrenCount(parent);

        for (int i = 0; i < childCount; i++)
        {
            auto child = Media::VisualTreeHelper::GetChild(parent, i).try_as<FrameworkElement>();
            if (!child)
                continue;

            auto name = child.Name();
            auto className = winrt::get_class_name(child);

            Wh_Log(L"FindChildElementByName: Child %d: class=%s, name=%s",
                   i, className.c_str(), name.c_str());

            if (name == targetName)
            {
                Wh_Log(L"FindChildElementByName: Found %s", targetName);
                return child;
            }
        }

        Wh_Log(L"FindChildElementByName: Could not find child named %s", targetName);
        return nullptr;
    }
    catch (const winrt::hresult_error &e)
    {
        Wh_Log(L"FindChildElementByName: Error searching for %s: %s", targetName, e.message().c_str());
        return nullptr;
    }
    catch (...)
    {
        Wh_Log(L"FindChildElementByName: Unknown error searching for %s", targetName);
        return nullptr;
    }
}

// Forward declarations
void ApplyMagnificationEffect();
bool BuildIconSpatialMap(FrameworkElement taskbarFrame);
void ApplyMagnificationFromMousePosition(double mouseX);
void MagnificationOnHoverStart(FrameworkElement taskbarElement, winrt::Windows::Foundation::Point mousePos);
void MagnificationOnMouseMove(winrt::Windows::Foundation::Point mousePos);
void MagnificationOnHoverStop();

// Hook function declarations
using TaskbarFrame_OnPointerMoved_t = int(WINAPI *)(void *pThis, void *pArgs);
static TaskbarFrame_OnPointerMoved_t OnPointerMoved_Original;
int WINAPI OnPointerMoved_Hook(void *pThis, void *pArgs);

using TaskbarFrame_OnPointerExited_t = int(WINAPI *)(void *pThis, void *pArgs);
static TaskbarFrame_OnPointerExited_t OnPointerExited_Original;
int WINAPI OnPointerExited_Hook(void *pThis, void *pArgs);

// New approach: Build spatial map of all icons on first hover
bool BuildIconSpatialMap(FrameworkElement taskbarFrame)
{
    Wh_Log(L"BuildIconSpatialMap: Building spatial map of taskbar icons");
    g_iconSpatialMap.clear();

    if (!taskbarFrame)
        throw std::runtime_error("BuildIconSpatialMap: No taskbar frame provided");

    FrameworkElement rootGrid = FindChildElementByName(taskbarFrame, L"RootGrid");
    if (!rootGrid)
        throw std::runtime_error("BuildIconSpatialMap: Could not find RootGrid under TaskbarFrame");
    FrameworkElement iconContainer = FindChildElementByName(rootGrid, L"TaskbarFrameRepeater");
    if (!iconContainer)
        throw std::runtime_error("BuildIconSpatialMap: Could not find TaskbarFrameRepeater under RootGrid");

    // Build spatial map of all icons
    int childCount = Media::VisualTreeHelper::GetChildrenCount(iconContainer);
    Wh_Log(L"BuildIconSpatialMap: Icon container has %d children", childCount);

    g_totalRangeLeft = 999999.0;   // Large positive number
    g_totalRangeRight = -999999.0; // Large negative number

    for (int i = 0; i < childCount; i++)
    {
        auto child = Media::VisualTreeHelper::GetChild(iconContainer, i).try_as<FrameworkElement>();
        if (!child)
            continue;

        if (!IsElementValid(child))
            continue;

        auto className = winrt::get_class_name(child);
        Wh_Log(L"BuildIconSpatialMap: Processing child %d: class=%s", i, className.c_str());

        // skip the weather widget which is the first child in with the main app icons
        auto elementName = child.Name();
        if (elementName == L"AugmentedEntryPointButton")
            continue;

        // Get the position and size of this icon
        auto transform = child.TransformToVisual(iconContainer);
        auto iconPos = transform.TransformPoint({0, 0});
        double width = child.ActualWidth();

        Wh_Log(L"BuildIconSpatialMap: Child %d coordinates - X=%.1f, width=%.1f", i, iconPos.X, width);

        if (width <= 0)
        {
            Wh_Log(L"BuildIconSpatialMap: Skipping child %d - zero width", i);
            continue; // Skip elements with no width
        }

        // Skip elements that are clearly off-screen or invalid (negative positions beyond reasonable bounds)
        if (iconPos.X < -100 || iconPos.X > 10000)
        {
            Wh_Log(L"BuildIconSpatialMap: Skipping icon %d with invalid X position %.1f", i, iconPos.X);
            continue;
        }

        IconInfo info{
            child,                     // element
            iconPos.X,                 // leftX
            iconPos.X + width,         // rightX
            iconPos.X + (width / 2.0), // centerX
            width,                     // width
            i                          // index
        };

        g_iconSpatialMap.push_back(info);

        // Update total range
        g_totalRangeLeft = std::fmin(g_totalRangeLeft, info.leftX);
        g_totalRangeRight = std::fmax(g_totalRangeRight, info.rightX);

        Wh_Log(L"BuildIconSpatialMap: Icon %d (%s) at X %.1f-%.1f (center %.1f, width %.1f)",
               i, className.c_str(), info.leftX, info.rightX, info.centerX, info.width);
    }

    // For testing - just mark as initialized even with empty map
    Wh_Log(L"BuildIconSpatialMap: Completed enumeration of %d children", childCount);

    if (g_iconSpatialMap.empty())
    {
        Wh_Log(L"BuildIconSpatialMap: No valid icons found with coordinates");
        return false;
    }

    Wh_Log(L"BuildIconSpatialMap: Built map with %zu icons, range X %.1f to %.1f",
           g_iconSpatialMap.size(), g_totalRangeLeft, g_totalRangeRight);

    g_spatialMapInitialized = true;
    return true;
}

// Apply magnification based on mouse X position using spatial mapping
void ApplyMagnificationFromMousePosition(double mouseX)
{
    if (!g_spatialMapInitialized || g_iconSpatialMap.empty())
    {
        Wh_Log(L"ApplyMagnificationFromMousePosition: Spatial map not initialized");
        return;
    }

    // Store the previous affected icons for cleanup
    auto previouslyAffectedIcons = g_affectedIcons;

    // Clear current affected icons
    g_affectedIcons.clear();

    double maxZoom = g_settings.zoomPercentage / 100.0;
    int maxRange = g_settings.zoomRange;

    // Calculate average icon width from our spatial map for influence distance
    double totalWidth = 0.0;
    int validIcons = 0;
    for (const auto &iconInfo : g_iconSpatialMap)
    {
        if (IsElementValid(iconInfo.element) && iconInfo.width > 0)
        {
            totalWidth += iconInfo.width;
            validIcons++;
        }
    }
    double averageIconWidth = validIcons > 0 ? totalWidth / validIcons : 68.0; // Fallback to 68px if no valid icons

    // Calculate maximum influence distance based on actual icon sizes
    double maxInfluenceDistance = maxRange * averageIconWidth;

    Wh_Log(L"ApplyMagnificationFromMousePosition: Using average icon width %.1f, influence distance %.1f",
           averageIconWidth, maxInfluenceDistance);

    // Build set of currently affected icon elements for quick lookup
    std::set<FrameworkElement> currentlyAffectedElements;

    // Apply distance-based magnification to ALL icons based on mouse position
    for (size_t i = 0; i < g_iconSpatialMap.size(); i++)
    {
        const auto &iconInfo = g_iconSpatialMap[i];

        if (!IsElementValid(iconInfo.element))
            continue;

        // Calculate distance from mouse to the center of this icon
        double distanceToIcon = std::abs(iconInfo.centerX - mouseX);

        // Use this icon's actual half-width for maximum zoom threshold
        double halfIconWidth = iconInfo.width / 2.0;

        // Only apply magnification if mouse is within influence range
        if (distanceToIcon <= maxInfluenceDistance)
        {
            // Calculate zoom factor based on distance using smooth falloff
            double zoomFactor;

            if (distanceToIcon <= halfIconWidth)
            { // Within half of this icon's actual width - maximum zoom
                zoomFactor = maxZoom;
            }
            else
            {
                // Smooth linear falloff from max zoom to 1.0 over the influence distance
                double falloffRatio = (distanceToIcon - halfIconWidth) / (maxInfluenceDistance - halfIconWidth);
                falloffRatio = std::fmin(1.0, std::fmax(0.0, falloffRatio)); // Clamp to [0,1]

                // Apply smooth interpolation: maxZoom at distance 0, 1.0 at max distance
                zoomFactor = maxZoom - ((maxZoom - 1.0) * falloffRatio);
                zoomFactor = std::fmax(1.0, zoomFactor); // Ensure we never go below 1.0
            }

            // Only add to affected icons if zoom is noticeably different from 1.0
            if (zoomFactor > 1.01)
            {
                g_affectedIcons.push_back({iconInfo.element, zoomFactor});
                currentlyAffectedElements.insert(iconInfo.element);

                Wh_Log(L"ApplyMagnificationFromMousePosition: Icon %zu distance=%.1f halfWidth=%.1f zoom=%.3f",
                       i, distanceToIcon, halfIconWidth, zoomFactor);
            }
        }
    }

    // Reset any previously affected icons that are no longer in the current affected set
    for (auto &[prevIcon, prevScale] : previouslyAffectedIcons)
    {
        if (IsElementValid(prevIcon) && currentlyAffectedElements.find(prevIcon) == currentlyAffectedElements.end())
        {
            // This icon was previously affected but is no longer in range - reset it
            try
            {
                auto transform = prevIcon.RenderTransform();
                if (transform)
                {
                    auto scaleTransform = transform.try_as<Media::ScaleTransform>();
                    if (scaleTransform)
                    {
                        scaleTransform.ScaleX(1.0);
                        scaleTransform.ScaleY(1.0);
                        Canvas::SetZIndex(prevIcon, 0); // Reset z-index too
                    }
                }
            }
            catch (...)
            {
                // Ignore errors during cleanup
            }
        }
    }

    // Apply the magnification
    ApplyMagnificationEffect();
}

// Apply magnification effect to all affected icons
void ApplyMagnificationEffect()
{
    if (g_affectedIcons.empty())
    {
        return;
    }

    for (auto &[icon, scaleFactor] : g_affectedIcons)
    {
        if (!icon || !IsElementValid(icon))
        {
            continue;
        }

        // Get class name for filtering
        auto className = winrt::get_class_name(icon);

        // Skip ItemsRepeater - it doesn't scale well and can cause issues
        if (className == L"Microsoft.UI.Xaml.Controls.ItemsRepeater")
        {
            continue;
        }

        // Additional safety check: Skip elements that might not be visible or interactive
        auto visibility = icon.Visibility();
        if (visibility != Visibility::Visible)
        {
            continue;
        }

        // Check if element has reasonable bounds
        auto bounds = icon.ActualWidth();
        if (bounds <= 0 || bounds > 1000)
        { // Reasonable icon size limits
            continue;
        }

        // First check if this element already has a ScaleTransform
        Media::ScaleTransform scaleTransform = nullptr;

        auto existingTransform = icon.RenderTransform();
        if (existingTransform)
        {
            // Try to cast existing transform to ScaleTransform
            scaleTransform = existingTransform.try_as<Media::ScaleTransform>();
        }

        if (!scaleTransform)
        {
            // Create a new scale transform if one doesn't exist
            scaleTransform = Media::ScaleTransform();
            icon.RenderTransform(scaleTransform);

            // Make sure to set the transform origin to center
            icon.RenderTransformOrigin({0.5, 0.5});
        }

        // Apply the scale
        scaleTransform.ScaleX(scaleFactor);
        scaleTransform.ScaleY(scaleFactor);

        // Handle z-order to make sure hovered icons appear on top
        // Higher scale factor should mean higher z-index
        if (scaleFactor > 1.1)
        {
            // Set a higher z-index based on scale factor
            int newZ = 100 + static_cast<int>((scaleFactor - 1.0) * 100);
            Canvas::SetZIndex(icon, newZ);
        }
    }

    g_magnificationActive = true;
}

// Reset magnification effect for all affected icons
void ResetMagnificationEffect()
{
    if (g_affectedIcons.empty())
    {
        return;
    }

    // Create a copy of the icons vector to prevent iterator invalidation
    // if the vector changes during iteration (e.g., due to UI events)
    auto iconsCopy = g_affectedIcons;

    for (auto &[icon, scaleFactor] : iconsCopy)
    {
        if (!icon || !IsElementValid(icon))
        {
            continue;
        }

        auto transform = icon.RenderTransform();
        if (transform)
        {
            auto scaleTransform = transform.try_as<Media::ScaleTransform>();
            if (scaleTransform)
            {
                // Reset the scale to 1.0
                scaleTransform.ScaleX(1.0);
                scaleTransform.ScaleY(1.0);

                // Reset z-index if we set it earlier
                Canvas::SetZIndex(icon, 0);
            }
        }
    }

    g_affectedIcons.clear();
    g_magnificationActive = false;
}

void MagnificationOnHoverStart(FrameworkElement taskbarElement, winrt::Windows::Foundation::Point mousePos)
{
    // Prevent re-entrance during magnification application
    if (g_applyingMagnification)
        return;

    // Add timing-based protection to prevent rapid-fire magnification
    DWORD currentTime = GetTickCount();
    if (currentTime - g_lastMagnificationTime < 16)
        return; // ~60 FPS update rate (16ms)

    try
    {
        if (!taskbarElement)
            throw std::runtime_error("MagnificationOnHoverStart: ERROR - taskbarElement is null, can't apply magnification");

        auto testWidth = taskbarElement.ActualWidth();
        auto testHeight = taskbarElement.ActualHeight();

        auto parent = taskbarElement.Parent();
        if (!parent)
            throw std::runtime_error("MagnificationOnHoverStart: Element has no parent, likely detached from UI");

        g_applyingMagnification = true;

        g_lastMagnificationTime = GetTickCount();

        // Store the element reference
        g_hoveredTaskbarFrame = taskbarElement;
        g_currentPointerPos = mousePos;

        // Build spatial map if not already initialized
        if (!g_spatialMapInitialized)
            g_spatialMapInitialized = BuildIconSpatialMap(taskbarElement); // this will throw if it needs to

        // Apply magnification based on mouse position
        ApplyMagnificationFromMousePosition(mousePos.X);

        g_applyingMagnification = false;

        Wh_Log(L"MagnificationOnHoverStart: Successfully started magnification");
    }
    catch (const std::runtime_error &ex)
    {
        Wh_Log(L"MagnificationOnHoverStart: FATAL ERROR: %s", ex.what());
        SafeResetElements();
    }
    catch (...)
    {
        Wh_Log(L"MagnificationOnHoverStart: Unknown FATAL ERROR");
        SafeResetElements();
    }
}

void MagnificationOnMouseMove(winrt::Windows::Foundation::Point mousePos)
{
    if (!g_spatialMapInitialized || !g_magnificationActive)
    {
        return; // No magnification active
    }

    // Only update if mouse moved significantly to avoid excessive processing
    if (g_currentPointerPos.has_value())
    {
        double deltaX = std::abs(mousePos.X - g_currentPointerPos->X);
        if (deltaX < 1.0)
        { // Very sensitive - update on every pixel movement
            return;
        }
    }

    // Update current pointer position
    g_currentPointerPos = mousePos;

    // Check if mouse is within the taskbar range (with tolerance)
    bool mouseInTaskbarArea = (mousePos.X >= g_totalRangeLeft - 50 && mousePos.X <= g_totalRangeRight + 50);

    if (mouseInTaskbarArea)
    {
        // Reapply magnification based on new mouse position
        ApplyMagnificationFromMousePosition(mousePos.X);
    }
    else
    {
        // Mouse moved outside taskbar area - reset all magnification immediately
        Wh_Log(L"MagnificationOnMouseMove: Mouse outside taskbar area, resetting magnification");
        ResetMagnificationEffect();
    }
}

void MagnificationOnHoverStop()
{
    Wh_Log(L"MagnificationOnHoverStop: start");

    if (!g_magnificationActive)
        return;

    // Create a copy of affected icons to prevent issues if the vector changes
    auto affectedIconsCopy = g_affectedIcons;

    // Clear global state before processing to prevent reentrance issues
    g_affectedIcons.clear();
    g_magnificationActive = false;

    // Process each icon individually with error handling
    for (auto &[icon, scale] : affectedIconsCopy)
    {
        try
        {
            // Skip null or invalid icons
            if (!icon || !IsElementValid(icon))
                continue;

            // Log what we're doing
            auto transform = icon.RenderTransform();
            if (transform)
            {
                auto scaleTransform = transform.try_as<Media::ScaleTransform>();
                if (scaleTransform)
                {
                    // Reset the scale to 1.0
                    scaleTransform.ScaleX(1.0);
                    scaleTransform.ScaleY(1.0);
                }
            }
            Canvas::SetZIndex(icon, 0);
        }
        catch (...)
        {
            Wh_Log(L"MagnificationOnHoverStop: Failed to reset elements after error");
        }
    }

    // Call our safe element reset function to clean everything up
    SafeResetElements();

    Wh_Log(L"MagnificationOnHoverStop: end");
}

// Implementation of hook functions - using the already declared variables from above
int WINAPI OnPointerMoved_Hook(void *pThis, void *pArgs)
{
    try
    {
        Wh_Log(L"TaskbarFrame_OnPointerMoved: start");

        // Try to get the FrameworkElement
        FrameworkElement element = nullptr;
        ((IUnknown *)pThis)->QueryInterface(winrt::guid_of<FrameworkElement>(), winrt::put_abi(element));

        Input::PointerRoutedEventArgs args = nullptr;
        ((IUnknown *)pArgs)->QueryInterface(winrt::guid_of<Input::PointerRoutedEventArgs>(), winrt::put_abi(args));

        // Try to read element properties if we have a valid element
        if (!element || !args)
            throw std::runtime_error("element or args not as expected");

        auto className = winrt::get_class_name(element);
        auto name = element.Name();

        if (className != g_settings.taskbarFrameClass)
            throw std::runtime_error("element or args not as expected");

        auto pointerPos = args.GetCurrentPoint(element).Position();

        if (!g_spatialMapInitialized)
            g_spatialMapInitialized = BuildIconSpatialMap(element);

        // Only trigger magnification if we have a valid spatial map and are within icon range
        if (g_spatialMapInitialized && !g_iconSpatialMap.empty())
        {
            // Check if mouse is within the taskbar icon area (with some tolerance)
            if (pointerPos.X >= g_totalRangeLeft - 50 && pointerPos.X <= g_totalRangeRight + 50)
            {
                Wh_Log(L"TaskbarFrame_OnPointerMoved: Mouse in icon area, triggering magnification");

                // Use mouse move for more responsive updates if magnification is already active
                if (g_magnificationActive)
                    MagnificationOnMouseMove(pointerPos);
                else
                    MagnificationOnHoverStart(element, pointerPos);
            }
            else
            {
                Wh_Log(L"TaskbarFrame_OnPointerMoved: Mouse outside icon area (X=%.1f, range=%.1f to %.1f)", pointerPos.X, g_totalRangeLeft, g_totalRangeRight);

                // Reset magnification if mouse moves outside taskbar area
                if (g_magnificationActive)
                    MagnificationOnHoverStop();
            }
        }
    }
    catch (const std::runtime_error &ex)
    {
        Wh_Log(L"OnPointerMoved_Hook: FATAL ERROR: %s", ex.what());
    }
    catch (...)
    {
        Wh_Log(L"OnPointerMoved_Hook: Unknown FATAL ERROR");
    }

    // Always call the original function
    return OnPointerMoved_Original(pThis, pArgs);
}

// Implementation of the pointer exited hook function - using the already declared variable from above
int WINAPI OnPointerExited_Hook(void *pThis, void *pArgs)
{
    try
    {
        MagnificationOnHoverStop();
    }
    catch (...)
    {
        Wh_Log(L"TaskbarFrame_OnPointerExited: Unknown error stopping magnification");
    }

    // Always call the original function
    return OnPointerExited_Original(pThis, pArgs);
}

bool HookTaskbarViewDllSymbols(HMODULE module)
{
    Wh_Log(L"HookTaskbarViewDllSymbols: Attempting to hook taskbar symbols");

    // Log what module we're inspecting
    WCHAR modulePath[MAX_PATH];
    GetModuleFileName(module, modulePath, MAX_PATH);
    Wh_Log(L"HookTaskbarViewDllSymbols: Checking module %s", modulePath);

    // Ensure settings are loaded before proceeding
    if (g_settings.taskbarFrameClass.empty())
    {
        Wh_Log(L"HookTaskbarViewDllSymbols: TaskbarFrameClass setting not loaded, using default");
        g_settings.taskbarFrameClass = L"Taskbar.TaskbarFrame";
    }

    // Track if we've hooked anything successfully
    bool hookedAny = false;

    {
        // Parse the configured taskbar frame class to get the namespace and class name parts
        std::wstring configuredClass = g_settings.taskbarFrameClass;

        // Split the class name by the dot to get namespace and class parts
        // Default to Taskbar.TaskbarFrame format if parsing fails
        std::wstring namespacePart = L"Taskbar";
        std::wstring classPart = L"TaskbarFrame";

        size_t dotPos = configuredClass.find(L'.');
        if (dotPos != std::wstring::npos)
        {
            namespacePart = configuredClass.substr(0, dotPos);
            classPart = configuredClass.substr(dotPos + 1);
        }

        Wh_Log(L"Parsed taskbar frame class: namespace=%s, class=%s",
               namespacePart.c_str(), classPart.c_str());

        // Hook the TaskbarFrame OnPointerMoved and OnPointerExited events
        // Only using pattern1 and pattern5 as they are the only ones that work
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

        if (HookSymbols(module, taskbarFrameHooks, ARRAYSIZE(taskbarFrameHooks)))
        {
            Wh_Log(L"HookTaskbarViewDllSymbols: Successfully hooked TaskbarFrame OnPointerMoved and OnPointerExited");
            hookedAny = true;
        }
        else
        {
            Wh_Log(L"HookTaskbarViewDllSymbols: Failed to hook TaskbarFrame patterns");
        }
    }

    return hookedAny;
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
        {
            Wh_ApplyHookOperations();
        }
    }
}

using LoadLibraryExW_t = decltype(&LoadLibraryExW);
LoadLibraryExW_t LoadLibraryExW_Original;
HMODULE WINAPI LoadLibraryExW_Hook(LPCWSTR lpLibFileName,
                                   HANDLE hFile,
                                   DWORD dwFlags)
{
    HMODULE module = LoadLibraryExW_Original(lpLibFileName, hFile, dwFlags);
    if (module)
    {
        HandleLoadedModuleIfTaskbarView(module, lpLibFileName);
    }

    return module;
}

void LoadSettings()
{
    g_settings.zoomPercentage = Wh_GetIntSetting(L"zoomPercentage");
    g_settings.zoomRange = Wh_GetIntSetting(L"zoomRange");
    g_settings.taskbarFrameClass = Wh_GetStringSetting(L"taskbarFrameClass");

    // Calculate zoom values from settings
    g_maxZoom = g_settings.zoomPercentage / 100.0;
    g_siblingsZoom = 1.0 + ((g_maxZoom - 1.0) * 0.5); // 50% of max zoom for siblings

    Wh_Log(L"Settings loaded: zoomPercentage=%d, zoomRange=%d, taskbarFrameClass=%s",
           g_settings.zoomPercentage, g_settings.zoomRange, g_settings.taskbarFrameClass.c_str());
    Wh_Log(L"Calculated values: maxZoom=%.2f, siblingsZoom=%.2f", g_maxZoom, g_siblingsZoom);
}

BOOL Wh_ModInit()
{
    Wh_Log(L"=== Initializing Windows 11 Taskbar Icon Zoom Mod ===");

    LoadSettings();
    Wh_Log(L"Settings loaded: zoomPercentage=%d, zoomRange=%d, taskbarFrameClass=%s",
           g_settings.zoomPercentage, g_settings.zoomRange, g_settings.taskbarFrameClass.c_str());

    g_magnificationActive = false;
    g_applyingMagnification = false; // Initialize the flag
    g_lastMagnificationTime = 0;     // Initialize timing

    if (HMODULE taskbarViewModule = GetTaskbarViewModuleHandle())
    {
        g_taskbarViewDllLoaded = true;
        Wh_Log(L"Taskbar view module found on init");

        if (!HookTaskbarViewDllSymbols(taskbarViewModule))
        {
            Wh_Log(L"ERROR: Failed to hook taskbar view symbols");
            return FALSE;
        }
        else
        {
            Wh_Log(L"Successfully hooked taskbar view symbols");
            return TRUE;
        }
    }
    else
    {
        Wh_Log(L"Taskbar view module not loaded yet, will hook on load");

        HMODULE kernelBaseModule = GetModuleHandle(L"kernelbase.dll");
        if (!kernelBaseModule)
        {
            Wh_Log(L"ERROR: Failed to get kernelbase.dll module handle");
            return FALSE;
        }

        auto pKernelBaseLoadLibraryExW =
            (decltype(&LoadLibraryExW))GetProcAddress(kernelBaseModule,
                                                      "LoadLibraryExW");
        if (!pKernelBaseLoadLibraryExW)
        {
            Wh_Log(L"ERROR: Failed to get LoadLibraryExW address");
            return FALSE;
        }

        // Create a hook for LoadLibraryExW
        WindhawkUtils::SYMBOL_HOOK kernelBaseHooks[] = {
            {
                {L"LoadLibraryExW",
                 LR"(LoadLibraryExW)",
                 LR"(__imp_LoadLibraryExW)"},
                (void **)&LoadLibraryExW_Original,
                (void *)LoadLibraryExW_Hook,
            },
        };

        if (HookSymbols(kernelBaseModule, kernelBaseHooks, ARRAYSIZE(kernelBaseHooks)))
        {
            Wh_Log(L"Successfully hooked LoadLibraryExW to catch taskbar view module load");
        }
        else
        {
            Wh_Log(L"ERROR: Failed to hook LoadLibraryExW");
            return FALSE;
        }
    }

    Wh_Log(L"=== Initialization complete ===");
    return TRUE;
}

void Wh_ModAfterInit()
{
    Wh_Log(L"After init");

    if (!g_taskbarViewDllLoaded)
    {
        if (HMODULE taskbarViewModule = GetTaskbarViewModuleHandle())
        {
            if (!g_taskbarViewDllLoaded.exchange(true))
            {
                Wh_Log(L"Got Taskbar.View.dll");

                if (HookTaskbarViewDllSymbols(taskbarViewModule))
                {
                    Wh_ApplyHookOperations();
                }
            }
        }
    }
}

void Wh_ModUninit()
{
    Wh_Log(L"Uninitializing");

    // Make sure we reset any active magnification before unloading
    if (g_magnificationActive)
    {
        MagnificationOnHoverStop();
    }
}

void Wh_ModSettingsChanged()
{
    Wh_Log(L"Settings changed");

    // Reset any active magnification before applying new settings
    if (g_magnificationActive)
    {
        MagnificationOnHoverStop();
    }

    LoadSettings();
}