#include <windows.h>

#ifndef DXGI_ERROR_UNSUPPORTED
#define DXGI_ERROR_UNSUPPORTED ((HRESULT)0x887A0004L)
#endif

#ifndef E_NOTIMPL
#define E_NOTIMPL ((HRESULT)0x80004001L)
#endif

extern "C" {

__declspec(dllexport) HRESULT WINAPI D3D12CreateDevice(
    IUnknown* pAdapter,
    DWORD MinimumFeatureLevel,
    REFIID riid,
    void** ppDevice)
{
    (void)pAdapter;
    (void)MinimumFeatureLevel;
    (void)riid;
    if (ppDevice) {
        *ppDevice = NULL;
    }
    // Return DXGI_ERROR_UNSUPPORTED (0x887A0004) to notify callers (such as
    // Qt RHI, ANGLE, or Direct3D loaders) that Direct3D 12 hardware acceleration
    // is unsupported on this operating system / display adapter. This allows
    // applications to safely fall back to Direct3D 11, OpenGL, or Software rasterization
    // instead of crashing on missing DLL entry points during process startup.
    return DXGI_ERROR_UNSUPPORTED;
}

__declspec(dllexport) HRESULT WINAPI D3D12GetDebugInterface(
    REFIID riid,
    void** ppvDebug)
{
    (void)riid;
    if (ppvDebug) {
        *ppvDebug = NULL;
    }
    return E_NOTIMPL;
}

__declspec(dllexport) HRESULT WINAPI D3D12SerializeRootSignature(
    const void* pRootSignature,
    DWORD Version,
    void** ppBlob,
    void** ppErrorBlob)
{
    (void)pRootSignature;
    (void)Version;
    if (ppBlob) {
        *ppBlob = NULL;
    }
    if (ppErrorBlob) {
        *ppErrorBlob = NULL;
    }
    return E_NOTIMPL;
}

__declspec(dllexport) HRESULT WINAPI D3D12SerializeVersionedRootSignature(
    const void* pRootSignatureDesc,
    void** ppBlob,
    void** ppErrorBlob)
{
    (void)pRootSignatureDesc;
    if (ppBlob) {
        *ppBlob = NULL;
    }
    if (ppErrorBlob) {
        *ppErrorBlob = NULL;
    }
    return E_NOTIMPL;
}

__declspec(dllexport) HRESULT WINAPI D3D12CreateRootSignatureDeserializer(
    const void* pSrcData,
    size_t SrcDataSizeInBytes,
    REFIID pRootSignatureDeserializerInterface,
    void** ppRootSignatureDeserializer)
{
    (void)pSrcData;
    (void)SrcDataSizeInBytes;
    (void)pRootSignatureDeserializerInterface;
    if (ppRootSignatureDeserializer) {
        *ppRootSignatureDeserializer = NULL;
    }
    return E_NOTIMPL;
}

__declspec(dllexport) HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializer(
    const void* pSrcData,
    size_t SrcDataSizeInBytes,
    REFIID pRootSignatureDeserializerInterface,
    void** ppRootSignatureDeserializer)
{
    (void)pSrcData;
    (void)SrcDataSizeInBytes;
    (void)pRootSignatureDeserializerInterface;
    if (ppRootSignatureDeserializer) {
        *ppRootSignatureDeserializer = NULL;
    }
    return E_NOTIMPL;
}

__declspec(dllexport) HRESULT WINAPI D3D12EnableExperimentalFeatures(
    UINT NumFeatures,
    const IID* pIIDs,
    void* pConfigurationStructs,
    UINT* pConfigurationStructSizes)
{
    (void)NumFeatures;
    (void)pIIDs;
    (void)pConfigurationStructs;
    (void)pConfigurationStructSizes;
    return E_NOTIMPL;
}

__declspec(dllexport) HRESULT WINAPI D3D12GetInterface(
    REFCLSID rclsid,
    REFIID riid,
    void** ppv)
{
    (void)rclsid;
    (void)riid;
    if (ppv) {
        *ppv = NULL;
    }
    return E_NOTIMPL;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)hinstDLL;
    (void)fdwReason;
    (void)lpvReserved;
    return TRUE;
}

} // extern "C"
