#ifndef WINDHAWK_UTILS_H
#define WINDHAWK_UTILS_H

#include <windows.h>
#include <stdio.h>
#include <string>

// Stub for Wh_Log that will be replaced by Windhawk's actual implementation
inline void Wh_Log(const wchar_t* format, ...)
{
    // This is just a stub - Windhawk will actually implement this
    // The stub does nothing in the development environment
}

// Settings functions
inline int Wh_GetIntSetting(const wchar_t* name)
{
    // Default values based on the mod settings
    if (wcscmp(name, L"zoomPercentage") == 0) return 150;
    if (wcscmp(name, L"zoomRange") == 0) return 3;
    return 0;
}

inline std::wstring Wh_GetStringSetting(const wchar_t* name)
{
    // Default values based on the mod settings
    if (wcscmp(name, L"taskbarFrameClass") == 0) return L"Taskbar.TaskbarFrame";
    return L"";
}

inline bool Wh_GetBoolSetting(const wchar_t* name)
{
    // Add default values for boolean settings if needed
    return false;
}

inline void Wh_SetIntSetting(const wchar_t* name, int value)
{
    // Stub - does nothing in the development environment
}

inline void Wh_SetStringSetting(const wchar_t* name, const wchar_t* value)
{
    // Stub - does nothing in the development environment
}

inline void Wh_SetBoolSetting(const wchar_t* name, bool value)
{
    // Stub - does nothing in the development environment
}

// Hook operations
inline BOOL Wh_ApplyHookOperations()
{
    // This is just a stub - Windhawk will actually implement this
    // In a real environment, this would apply all pending hook operations
    return TRUE;
}

// Module lifecycle functions - these would typically be implemented in the mod.cpp file
// but are declared here for reference

// inline BOOL Wh_ModInit()
// {
//     // This would be implemented in the mod.cpp file
//     return TRUE;
// }

// inline void Wh_ModUninit()
// {
//     // This would be implemented in the mod.cpp file
// }

namespace WindhawkUtils
{
    struct SYMBOL_HOOK
    {
        const wchar_t* symbols[10];
        void** ppOriginalFunction;
        void* pHookFunction;
    };

    inline BOOL HookSymbols(
        HMODULE module,
        SYMBOL_HOOK* hookArray,
        DWORD hookArraySize
    )
    {
        // This is just a stub - Windhawk will actually implement this
        return TRUE;
    }
}

#endif // WINDHAWK_UTILS_H
