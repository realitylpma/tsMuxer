#include "doviLib.h"

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace
{
// dovi_tool's conversion mode 2 is "profile 7 to profile 8.1". The other modes convert between
// other profiles and are not what this feature is for.
constexpr uint8_t MODE_P7_TO_P81 = 2;

#if defined(_WIN32) || defined(_WIN64)
// The published Windows build ships exactly this file name and nothing else.
const char* const LIB_NAMES[] = {"dovi.dll"};
#elif defined(__APPLE__)
const char* const LIB_NAMES[] = {"libdovi.3.dylib", "libdovi.dylib"};
#else
const char* const LIB_NAMES[] = {"libdovi.so.3", "libdovi.so"};
#endif

void* openLib(const char* name)
{
#if defined(_WIN32) || defined(_WIN64)
    return LoadLibraryA(name);
#else
    return dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
}

void closeLib(void* h)
{
    if (!h)
        return;
#if defined(_WIN32) || defined(_WIN64)
    FreeLibrary(static_cast<HMODULE>(h));
#else
    dlclose(h);
#endif
}

void* symbol(void* h, const char* name)
{
#if defined(_WIN32) || defined(_WIN64)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(h), name));
#else
    return dlsym(h, name);
#endif
}
}  // namespace

const char* DoviLib::libraryName() { return LIB_NAMES[0]; }

DoviLib& DoviLib::instance()
{
    static DoviLib lib;
    return lib;
}

DoviLib::DoviLib()
{
    for (const char* name : LIB_NAMES)
    {
        m_handle = openLib(name);
        if (m_handle)
            break;
    }
    if (!m_handle)
    {
        m_loadError = std::string(libraryName()) + " was not found";
        return;
    }

    // All six or none: a partial bind would fail later, in the middle of a mux, which is a much
    // worse place to discover it than here.
    m_parse = reinterpret_cast<ParseUnspec62Fn>(symbol(m_handle, "dovi_parse_unspec62_nalu"));
    m_getError = reinterpret_cast<GetErrorFn>(symbol(m_handle, "dovi_rpu_get_error"));
    m_convert = reinterpret_cast<ConvertWithModeFn>(symbol(m_handle, "dovi_convert_rpu_with_mode"));
    m_write = reinterpret_cast<WriteUnspec62Fn>(symbol(m_handle, "dovi_write_unspec62_nalu"));
    m_dataFree = reinterpret_cast<DataFreeFn>(symbol(m_handle, "dovi_data_free"));
    m_rpuFree = reinterpret_cast<RpuFreeFn>(symbol(m_handle, "dovi_rpu_free"));

    if (m_parse && m_getError && m_convert && m_write && m_dataFree && m_rpuFree)
    {
        m_ok = true;
        return;
    }
    m_loadError = std::string(libraryName()) + " was found but does not export the expected functions";
    closeLib(m_handle);
    m_handle = nullptr;
}

DoviLib::~DoviLib()
{
    closeLib(m_handle);
    m_handle = nullptr;
}

bool DoviLib::convertRpuToProfile81(const uint8_t* nal, size_t len, std::vector<uint8_t>& out, std::string& err)
{
    if (!m_ok)
    {
        err = m_loadError;
        return false;
    }
    if (nal == nullptr || len < 3)
    {
        err = "RPU too short to parse";
        return false;
    }

    void* rpu = m_parse(nal, len);
    if (rpu == nullptr)
    {
        err = "the RPU could not be parsed";
        return false;
    }
    // libdovi reports a parse failure through the object rather than a null pointer, so the error
    // has to be asked for even when a pointer came back.
    if (const char* e = m_getError(rpu); e != nullptr)
    {
        err = e;
        m_rpuFree(rpu);
        return false;
    }

    if (m_convert(rpu, MODE_P7_TO_P81) != 0)
    {
        const char* e = m_getError(rpu);
        err = e ? e : "conversion to profile 8.1 failed";
        m_rpuFree(rpu);
        return false;
    }

    const DoviData* written = m_write(rpu);
    if (written == nullptr || written->data == nullptr || written->len == 0)
    {
        const char* e = m_getError(rpu);
        err = e ? e : "the converted RPU could not be written";
        if (written)
            m_dataFree(written);
        m_rpuFree(rpu);
        return false;
    }

    out.assign(written->data, written->data + written->len);
    m_dataFree(written);
    m_rpuFree(rpu);
    return true;
}
