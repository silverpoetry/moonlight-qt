#include "clipboardvirtualfiles_win.h"

#include <Limelight.h>

#include <QVector>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <objidl.h>
#include <shldisp.h>
#include <shlobj.h>
#include <thread>
#include <windows.h>

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

namespace ClipboardVirtualFiles {
    namespace {
        constexpr quint32 MaximumChunkBytes =
                LI_CLIPBOARD_MAX_FILE_CHUNK_BYTES;

        struct FileEntry
        {
            QString path;
            quint8 type;
            quint64 size;
            quint64 modifiedTimeMs;
        };

        HGLOBAL copyToGlobalMemory(const void* source, size_t size)
        {
            HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
            if (memory == nullptr) {
                return nullptr;
            }
            void* destination = GlobalLock(memory);
            if (destination == nullptr) {
                GlobalFree(memory);
                return nullptr;
            }
            std::memcpy(destination, source, size);
            GlobalUnlock(memory);
            return memory;
        }

        FILETIME unixTimeToFileTime(quint64 milliseconds)
        {
            constexpr quint64 WindowsEpoch = Q_UINT64_C(116444736000000000);
            constexpr quint64 TicksPerMillisecond = 10000;
            const quint64 ticks =
                    milliseconds <=
                        ((std::numeric_limits<quint64>::max)() - WindowsEpoch) /
                            TicksPerMillisecond ?
                        WindowsEpoch + milliseconds * TicksPerMillisecond :
                        WindowsEpoch;
            ULARGE_INTEGER value = {};
            value.QuadPart = ticks;
            return {value.LowPart, value.HighPart};
        }

        bool decodeManifest(const QByteArray& manifest,
                            QVector<FileEntry>& entries)
        {
            LI_CLIPBOARD_FILE_MANIFEST_HEADER header;
            if (!LiIsValidClipboardFileManifest(
                        reinterpret_cast<const uint8_t*>(manifest.constData()),
                        static_cast<size_t>(manifest.size())) ||
                    !LiDecodeClipboardFileManifestHeader(
                        reinterpret_cast<const uint8_t*>(manifest.constData()),
                        static_cast<size_t>(manifest.size()),
                        &header)) {
                return false;
            }

            entries.reserve(static_cast<int>(header.entryCount));
            size_t offset = LI_CLIPBOARD_FILE_MANIFEST_HEADER_SIZE;
            for (quint32 index = 0; index < header.entryCount; index++) {
                LI_CLIPBOARD_FILE_MANIFEST_ENTRY decoded;
                if (!LiDecodeClipboardFileManifestEntry(
                            reinterpret_cast<const uint8_t*>(
                                manifest.constData()),
                            static_cast<size_t>(manifest.size()),
                            &offset,
                            &decoded)) {
                    return false;
                }
                QString path = QString::fromUtf8(
                            reinterpret_cast<const char*>(decoded.path),
                            static_cast<int>(decoded.pathLength));
                path.replace('/', '\\');
                if (path.isEmpty() || path.size() >= MAX_PATH) {
                    return false;
                }
                entries.append({
                    std::move(path),
                    decoded.type,
                    decoded.size,
                    decoded.modifiedTimeMs,
                });
            }
            return offset == static_cast<size_t>(manifest.size()) &&
                    !entries.isEmpty();
        }

        class VirtualFileStream final : public IStream
        {
        public:
            VirtualFileStream(quint32 fileIndex,
                              quint64 size,
                              QString name,
                              ReadCallback readCallback)
                : m_FileIndex(fileIndex),
                  m_Size(size),
                  m_Name(std::move(name)),
                  m_ReadCallback(std::move(readCallback))
            {
            }

            HRESULT STDMETHODCALLTYPE QueryInterface(
                    REFIID iid, void** object) override
            {
                if (object == nullptr) {
                    return E_POINTER;
                }
                if (iid == IID_IUnknown ||
                        iid == IID_ISequentialStream ||
                        iid == IID_IStream) {
                    *object = static_cast<IStream*>(this);
                    AddRef();
                    return S_OK;
                }
                *object = nullptr;
                return E_NOINTERFACE;
            }

            ULONG STDMETHODCALLTYPE AddRef() override
            {
                return ++m_References;
            }

            ULONG STDMETHODCALLTYPE Release() override
            {
                const ULONG remaining = --m_References;
                if (remaining == 0) {
                    delete this;
                }
                return remaining;
            }

            HRESULT STDMETHODCALLTYPE Read(
                    void* destination,
                    ULONG requested,
                    ULONG* bytesRead) override
            {
                if (bytesRead != nullptr) {
                    *bytesRead = 0;
                }
                if (requested != 0 && destination == nullptr) {
                    return STG_E_INVALIDPOINTER;
                }
                if (requested == 0) {
                    return S_OK;
                }

                auto* output = static_cast<char*>(destination);
                ULONG totalRead = 0;
                try {
                    while (totalRead < requested && m_Position < m_Size) {
                        const quint32 length = static_cast<quint32>(
                                    qMin<quint64>(
                                        qMin<quint64>(
                                            requested - totalRead,
                                            m_Size - m_Position),
                                        MaximumChunkBytes));
                        const QByteArray bytes = m_ReadCallback(
                                    m_FileIndex, m_Position, length);
                        if (bytes.size() != static_cast<int>(length)) {
                            if (bytesRead != nullptr) {
                                *bytesRead = totalRead;
                            }
                            return STG_E_READFAULT;
                        }
                        std::memcpy(output + totalRead,
                                    bytes.constData(),
                                    static_cast<size_t>(bytes.size()));
                        m_Position += static_cast<quint64>(bytes.size());
                        totalRead += static_cast<ULONG>(bytes.size());
                    }
                }
                catch (...) {
                    if (bytesRead != nullptr) {
                        *bytesRead = totalRead;
                    }
                    return STG_E_READFAULT;
                }

                if (bytesRead != nullptr) {
                    *bytesRead = totalRead;
                }
                return totalRead == requested ? S_OK : S_FALSE;
            }

            HRESULT STDMETHODCALLTYPE Write(
                    const void*, ULONG, ULONG*) override
            {
                return STG_E_ACCESSDENIED;
            }

            HRESULT STDMETHODCALLTYPE Seek(
                    LARGE_INTEGER move,
                    DWORD origin,
                    ULARGE_INTEGER* newPosition) override
            {
                qint64 base;
                switch (origin) {
                case STREAM_SEEK_SET:
                    base = 0;
                    break;
                case STREAM_SEEK_CUR:
                    if (m_Position >
                            static_cast<quint64>(
                                (std::numeric_limits<qint64>::max)())) {
                        return STG_E_INVALIDFUNCTION;
                    }
                    base = static_cast<qint64>(m_Position);
                    break;
                case STREAM_SEEK_END:
                    if (m_Size >
                            static_cast<quint64>(
                                (std::numeric_limits<qint64>::max)())) {
                        return STG_E_INVALIDFUNCTION;
                    }
                    base = static_cast<qint64>(m_Size);
                    break;
                default:
                    return STG_E_INVALIDFUNCTION;
                }
                if ((move.QuadPart < 0 && base < -move.QuadPart) ||
                        (move.QuadPart > 0 &&
                         base > (std::numeric_limits<qint64>::max)() -
                            move.QuadPart)) {
                    return STG_E_INVALIDFUNCTION;
                }
                const qint64 target = base + move.QuadPart;
                if (target < 0) {
                    return STG_E_INVALIDFUNCTION;
                }
                m_Position = static_cast<quint64>(target);
                if (newPosition != nullptr) {
                    newPosition->QuadPart = m_Position;
                }
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override
            {
                return STG_E_ACCESSDENIED;
            }

            HRESULT STDMETHODCALLTYPE CopyTo(
                    IStream* destination,
                    ULARGE_INTEGER count,
                    ULARGE_INTEGER* bytesRead,
                    ULARGE_INTEGER* bytesWritten) override
            {
                if (destination == nullptr) {
                    return STG_E_INVALIDPOINTER;
                }
                if (bytesRead != nullptr) {
                    bytesRead->QuadPart = 0;
                }
                if (bytesWritten != nullptr) {
                    bytesWritten->QuadPart = 0;
                }

                QByteArray buffer(1024 * 1024, Qt::Uninitialized);
                quint64 remaining = count.QuadPart;
                while (remaining != 0) {
                    const ULONG requested = static_cast<ULONG>(
                                qMin<quint64>(
                                    remaining,
                                    static_cast<quint64>(buffer.size())));
                    ULONG read = 0;
                    const HRESULT readResult =
                            Read(buffer.data(), requested, &read);
                    if (FAILED(readResult)) {
                        return readResult;
                    }
                    if (read == 0) {
                        return S_FALSE;
                    }
                    ULONG written = 0;
                    const HRESULT writeResult =
                            destination->Write(buffer.data(), read, &written);
                    if (FAILED(writeResult) || written != read) {
                        return STG_E_WRITEFAULT;
                    }
                    remaining -= read;
                    if (bytesRead != nullptr) {
                        bytesRead->QuadPart += read;
                    }
                    if (bytesWritten != nullptr) {
                        bytesWritten->QuadPart += written;
                    }
                    if (readResult == S_FALSE) {
                        return S_FALSE;
                    }
                }
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE Commit(DWORD) override
            {
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE Revert() override
            {
                return STG_E_REVERTED;
            }

            HRESULT STDMETHODCALLTYPE LockRegion(
                    ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override
            {
                return STG_E_INVALIDFUNCTION;
            }

            HRESULT STDMETHODCALLTYPE UnlockRegion(
                    ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override
            {
                return STG_E_INVALIDFUNCTION;
            }

            HRESULT STDMETHODCALLTYPE Stat(
                    STATSTG* status, DWORD flags) override
            {
                if (status == nullptr) {
                    return STG_E_INVALIDPOINTER;
                }
                *status = {};
                status->type = STGTY_STREAM;
                status->cbSize.QuadPart = m_Size;
                status->grfMode = STGM_READ;
                if ((flags & STATFLAG_NONAME) == 0) {
                    const std::wstring name = m_Name.toStdWString();
                    const size_t bytes =
                            (name.size() + 1) * sizeof(wchar_t);
                    status->pwcsName =
                            static_cast<wchar_t*>(CoTaskMemAlloc(bytes));
                    if (status->pwcsName == nullptr) {
                        return STG_E_INSUFFICIENTMEMORY;
                    }
                    std::memcpy(status->pwcsName, name.c_str(), bytes);
                }
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE Clone(IStream** stream) override
            {
                if (stream == nullptr) {
                    return E_POINTER;
                }
                auto* clone = new (std::nothrow) VirtualFileStream(
                            m_FileIndex,
                            m_Size,
                            m_Name,
                            m_ReadCallback);
                if (clone == nullptr) {
                    return E_OUTOFMEMORY;
                }
                clone->m_Position = m_Position;
                *stream = clone;
                return S_OK;
            }

        private:
            std::atomic_ulong m_References {1};
            quint32 m_FileIndex;
            quint64 m_Size;
            QString m_Name;
            ReadCallback m_ReadCallback;
            quint64 m_Position {0};
        };

        class FormatEnumerator final : public IEnumFORMATETC
        {
        public:
            explicit FormatEnumerator(QVector<FORMATETC> formats)
                : m_Formats(std::move(formats))
            {
            }

            HRESULT STDMETHODCALLTYPE QueryInterface(
                    REFIID iid, void** object) override
            {
                if (object == nullptr) {
                    return E_POINTER;
                }
                if (iid == IID_IUnknown || iid == IID_IEnumFORMATETC) {
                    *object = static_cast<IEnumFORMATETC*>(this);
                    AddRef();
                    return S_OK;
                }
                *object = nullptr;
                return E_NOINTERFACE;
            }

            ULONG STDMETHODCALLTYPE AddRef() override
            {
                return ++m_References;
            }

            ULONG STDMETHODCALLTYPE Release() override
            {
                const ULONG remaining = --m_References;
                if (remaining == 0) {
                    delete this;
                }
                return remaining;
            }

            HRESULT STDMETHODCALLTYPE Next(
                    ULONG count,
                    FORMATETC* formats,
                    ULONG* fetched) override
            {
                if (formats == nullptr ||
                        (count != 1 && fetched == nullptr)) {
                    return E_INVALIDARG;
                }
                ULONG copied = 0;
                while (copied < count &&
                       m_Position < static_cast<size_t>(m_Formats.size())) {
                    formats[copied] = m_Formats.at(
                                static_cast<int>(m_Position));
                    formats[copied].ptd = nullptr;
                    copied++;
                    m_Position++;
                }
                if (fetched != nullptr) {
                    *fetched = copied;
                }
                return copied == count ? S_OK : S_FALSE;
            }

            HRESULT STDMETHODCALLTYPE Skip(ULONG count) override
            {
                const size_t remaining =
                        static_cast<size_t>(m_Formats.size()) - m_Position;
                const size_t skipped = qMin<size_t>(count, remaining);
                m_Position += skipped;
                return skipped == count ? S_OK : S_FALSE;
            }

            HRESULT STDMETHODCALLTYPE Reset() override
            {
                m_Position = 0;
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE Clone(
                    IEnumFORMATETC** enumerator) override
            {
                if (enumerator == nullptr) {
                    return E_POINTER;
                }
                auto* clone =
                        new (std::nothrow) FormatEnumerator(m_Formats);
                if (clone == nullptr) {
                    return E_OUTOFMEMORY;
                }
                clone->m_Position = m_Position;
                *enumerator = clone;
                return S_OK;
            }

        private:
            std::atomic_ulong m_References {1};
            QVector<FORMATETC> m_Formats;
            size_t m_Position {0};
        };

        class VirtualFileDataObject final :
                public IDataObject,
                public IDataObjectAsyncCapability
        {
        public:
            VirtualFileDataObject(QByteArray markerData,
                                  ManifestCallback manifestCallback,
                                  ReadCallback readCallback,
                                  UINT markerFormat)
                : m_MarkerData(std::move(markerData)),
                  m_ManifestCallback(std::move(manifestCallback)),
                  m_ReadCallback(std::move(readCallback)),
                  m_DescriptorFormat(
                      RegisterClipboardFormatW(L"FileGroupDescriptorW")),
                  m_ContentsFormat(
                      RegisterClipboardFormatW(L"FileContents")),
                  m_PreferredDropEffectFormat(
                      RegisterClipboardFormatW(L"Preferred DropEffect")),
                  m_MarkerFormat(markerFormat)
            {
            }

            bool isValid() const
            {
                return !m_MarkerData.isEmpty() &&
                        m_ManifestCallback &&
                        m_ReadCallback &&
                        m_DescriptorFormat != 0 &&
                        m_ContentsFormat != 0 &&
                        m_PreferredDropEffectFormat != 0 &&
                        m_MarkerFormat != 0;
            }

            HRESULT STDMETHODCALLTYPE QueryInterface(
                    REFIID iid, void** object) override
            {
                if (object == nullptr) {
                    return E_POINTER;
                }
                if (iid == IID_IUnknown || iid == IID_IDataObject) {
                    *object = static_cast<IDataObject*>(this);
                    AddRef();
                    return S_OK;
                }
                if (iid == IID_IDataObjectAsyncCapability) {
                    *object =
                            static_cast<IDataObjectAsyncCapability*>(this);
                    AddRef();
                    return S_OK;
                }
                *object = nullptr;
                return E_NOINTERFACE;
            }

            ULONG STDMETHODCALLTYPE AddRef() override
            {
                return ++m_References;
            }

            ULONG STDMETHODCALLTYPE Release() override
            {
                const ULONG remaining = --m_References;
                if (remaining == 0) {
                    delete this;
                }
                return remaining;
            }

            HRESULT STDMETHODCALLTYPE GetData(
                    FORMATETC* format, STGMEDIUM* medium) override
            {
                if (format == nullptr || medium == nullptr) {
                    return E_POINTER;
                }
                *medium = {};
                const HRESULT query = QueryGetData(format);
                if (FAILED(query)) {
                    return query;
                }

                if (format->cfFormat == m_DescriptorFormat) {
                    if (!ensureManifest()) {
                        return STG_E_READFAULT;
                    }
                    return getDescriptors(*medium);
                }
                if (format->cfFormat == m_ContentsFormat) {
                    if (!ensureManifest()) {
                        return STG_E_READFAULT;
                    }
                    const int index = format->lindex;
                    if (index < 0 ||
                            index >= m_Entries.size() ||
                            m_Entries.at(index).type !=
                                LI_CLIPBOARD_FILE_TYPE_REGULAR) {
                        return DV_E_LINDEX;
                    }
                    const FileEntry& entry = m_Entries.at(index);
                    auto* stream = new (std::nothrow) VirtualFileStream(
                                static_cast<quint32>(index),
                                entry.size,
                                entry.path,
                                m_ReadCallback);
                    if (stream == nullptr) {
                        return E_OUTOFMEMORY;
                    }
                    medium->tymed = TYMED_ISTREAM;
                    medium->pstm = stream;
                    return S_OK;
                }
                if (format->cfFormat == m_PreferredDropEffectFormat) {
                    const DWORD effect = DROPEFFECT_COPY;
                    medium->hGlobal =
                            copyToGlobalMemory(&effect, sizeof(effect));
                }
                else if (format->cfFormat == m_MarkerFormat) {
                    medium->hGlobal = copyToGlobalMemory(
                                m_MarkerData.constData(),
                                static_cast<size_t>(m_MarkerData.size()));
                }
                if (medium->hGlobal == nullptr) {
                    return E_OUTOFMEMORY;
                }
                medium->tymed = TYMED_HGLOBAL;
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE GetDataHere(
                    FORMATETC*, STGMEDIUM*) override
            {
                return DATA_E_FORMATETC;
            }

            HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override
            {
                if (format == nullptr) {
                    return E_POINTER;
                }
                if (format->dwAspect != DVASPECT_CONTENT) {
                    return DV_E_DVASPECT;
                }
                if (format->cfFormat == m_DescriptorFormat) {
                    return (format->tymed & TYMED_HGLOBAL) != 0 ?
                                S_OK :
                                DV_E_TYMED;
                }
                if (format->cfFormat == m_ContentsFormat) {
                    if ((format->tymed & TYMED_ISTREAM) == 0) {
                        return DV_E_TYMED;
                    }
                    if (format->lindex < 0) {
                        return DV_E_LINDEX;
                    }
                    return S_OK;
                }
                if (format->cfFormat == m_PreferredDropEffectFormat ||
                        format->cfFormat == m_MarkerFormat) {
                    return (format->tymed & TYMED_HGLOBAL) != 0 ?
                                S_OK :
                                DV_E_TYMED;
                }
                return DV_E_FORMATETC;
            }

            HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(
                    FORMATETC*, FORMATETC* output) override
            {
                if (output == nullptr) {
                    return E_POINTER;
                }
                output->ptd = nullptr;
                return DATA_S_SAMEFORMATETC;
            }

            HRESULT STDMETHODCALLTYPE SetData(
                    FORMATETC*, STGMEDIUM*, BOOL) override
            {
                return E_NOTIMPL;
            }

            HRESULT STDMETHODCALLTYPE EnumFormatEtc(
                    DWORD direction,
                    IEnumFORMATETC** enumerator) override
            {
                if (enumerator == nullptr) {
                    return E_POINTER;
                }
                if (direction != DATADIR_GET) {
                    *enumerator = nullptr;
                    return E_NOTIMPL;
                }
                QVector<FORMATETC> formats {
                    {
                        static_cast<CLIPFORMAT>(m_DescriptorFormat),
                        nullptr,
                        DVASPECT_CONTENT,
                        -1,
                        TYMED_HGLOBAL,
                    },
                    {
                        static_cast<CLIPFORMAT>(m_ContentsFormat),
                        nullptr,
                        DVASPECT_CONTENT,
                        -1,
                        TYMED_ISTREAM,
                    },
                    {
                        static_cast<CLIPFORMAT>(
                            m_PreferredDropEffectFormat),
                        nullptr,
                        DVASPECT_CONTENT,
                        -1,
                        TYMED_HGLOBAL,
                    },
                    {
                        static_cast<CLIPFORMAT>(m_MarkerFormat),
                        nullptr,
                        DVASPECT_CONTENT,
                        -1,
                        TYMED_HGLOBAL,
                    },
                };
                *enumerator =
                        new (std::nothrow) FormatEnumerator(
                            std::move(formats));
                return *enumerator != nullptr ? S_OK : E_OUTOFMEMORY;
            }

            HRESULT STDMETHODCALLTYPE DAdvise(
                    FORMATETC*, DWORD, IAdviseSink*, DWORD*) override
            {
                return OLE_E_ADVISENOTSUPPORTED;
            }

            HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override
            {
                return OLE_E_ADVISENOTSUPPORTED;
            }

            HRESULT STDMETHODCALLTYPE EnumDAdvise(
                    IEnumSTATDATA**) override
            {
                return OLE_E_ADVISENOTSUPPORTED;
            }

            HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL enabled) override
            {
                m_AsyncMode = enabled != FALSE;
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL* enabled) override
            {
                if (enabled == nullptr) {
                    return E_POINTER;
                }
                *enabled = m_AsyncMode ? TRUE : FALSE;
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx*) override
            {
                m_InOperation = true;
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE InOperation(BOOL* inOperation) override
            {
                if (inOperation == nullptr) {
                    return E_POINTER;
                }
                *inOperation = m_InOperation ? TRUE : FALSE;
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE EndOperation(
                    HRESULT, IBindCtx*, DWORD) override
            {
                m_InOperation = false;
                return S_OK;
            }

        private:
            bool ensureManifest()
            {
                {
                    std::unique_lock<std::mutex> lock(m_ManifestMutex);
                    if (m_ManifestState == ManifestState::Ready) {
                        return true;
                    }
                    if (m_ManifestState == ManifestState::Failed) {
                        return false;
                    }
                    if (m_ManifestState == ManifestState::Loading) {
                        m_ManifestReady.wait(lock, [this]() {
                            return m_ManifestState !=
                                    ManifestState::Loading;
                        });
                        return m_ManifestState == ManifestState::Ready;
                    }
                    m_ManifestState = ManifestState::Loading;
                }

                QVector<FileEntry> entries;
                const QByteArray manifest = m_ManifestCallback();
                const bool valid = decodeManifest(manifest, entries);
                {
                    std::lock_guard<std::mutex> lock(m_ManifestMutex);
                    if (valid) {
                        m_Entries = std::move(entries);
                        m_ManifestState = ManifestState::Ready;
                    }
                    else {
                        m_ManifestState = ManifestState::Failed;
                    }
                }
                m_ManifestReady.notify_all();
                return valid;
            }

            HRESULT getDescriptors(STGMEDIUM& medium) const
            {
                const size_t size =
                        offsetof(FILEGROUPDESCRIPTORW, fgd) +
                        static_cast<size_t>(m_Entries.size()) *
                            sizeof(FILEDESCRIPTORW);
                HGLOBAL memory =
                        GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
                if (memory == nullptr) {
                    return E_OUTOFMEMORY;
                }
                auto* group =
                        static_cast<FILEGROUPDESCRIPTORW*>(
                            GlobalLock(memory));
                if (group == nullptr) {
                    GlobalFree(memory);
                    return E_OUTOFMEMORY;
                }
                group->cItems = static_cast<UINT>(m_Entries.size());
                for (int index = 0; index < m_Entries.size(); index++) {
                    const FileEntry& entry = m_Entries.at(index);
                    FILEDESCRIPTORW& descriptor = group->fgd[index];
                    descriptor.dwFlags =
                            FD_ATTRIBUTES |
                            FD_PROGRESSUI |
                            FD_UNICODE;
                    descriptor.dwFileAttributes =
                            entry.type ==
                                LI_CLIPBOARD_FILE_TYPE_DIRECTORY ?
                                FILE_ATTRIBUTE_DIRECTORY :
                                FILE_ATTRIBUTE_NORMAL;
                    if (entry.type ==
                            LI_CLIPBOARD_FILE_TYPE_REGULAR) {
                        descriptor.dwFlags |= FD_FILESIZE;
                        descriptor.nFileSizeHigh =
                                static_cast<DWORD>(entry.size >> 32);
                        descriptor.nFileSizeLow =
                                static_cast<DWORD>(entry.size);
                    }
                    if (entry.modifiedTimeMs != 0) {
                        descriptor.dwFlags |= FD_WRITESTIME;
                        descriptor.ftLastWriteTime =
                                unixTimeToFileTime(
                                    entry.modifiedTimeMs);
                    }
                    const std::wstring path = entry.path.toStdWString();
                    std::copy(path.begin(),
                              path.end(),
                              descriptor.cFileName);
                    descriptor.cFileName[path.size()] = L'\0';
                }
                GlobalUnlock(memory);
                medium.tymed = TYMED_HGLOBAL;
                medium.hGlobal = memory;
                return S_OK;
            }

            enum class ManifestState {
                Empty,
                Loading,
                Ready,
                Failed,
            };

            std::atomic_ulong m_References {1};
            QVector<FileEntry> m_Entries;
            QByteArray m_MarkerData;
            ManifestCallback m_ManifestCallback;
            ReadCallback m_ReadCallback;
            std::mutex m_ManifestMutex;
            std::condition_variable m_ManifestReady;
            ManifestState m_ManifestState {ManifestState::Empty};
            UINT m_DescriptorFormat;
            UINT m_ContentsFormat;
            UINT m_PreferredDropEffectFormat;
            UINT m_MarkerFormat;
            std::atomic_bool m_AsyncMode {true};
            std::atomic_bool m_InOperation {false};
        };

        class OleClipboardBroker
        {
        public:
            OleClipboardBroker()
                : m_WakeEvent(
                      CreateEventW(nullptr, TRUE, FALSE, nullptr)),
                  m_Thread([this]() {
                      run();
                  })
            {
                std::unique_lock<std::mutex> lock(m_Mutex);
                m_Ready.wait(lock, [this]() {
                    return m_Initialized;
                });
            }

            ~OleClipboardBroker()
            {
                {
                    std::lock_guard<std::mutex> lock(m_Mutex);
                    m_Stopping = true;
                }
                if (m_WakeEvent != nullptr) {
                    SetEvent(m_WakeEvent);
                }
                if (m_Thread.joinable()) {
                    m_Thread.join();
                }
                if (m_WakeEvent != nullptr) {
                    CloseHandle(m_WakeEvent);
                }
            }

            bool set(IDataObject* object)
            {
                if (object == nullptr || !m_OleAvailable) {
                    if (object != nullptr) {
                        object->Release();
                    }
                    return false;
                }

                const auto job = std::make_shared<Job>();
                job->object = object;
                {
                    std::lock_guard<std::mutex> lock(m_Mutex);
                    m_Jobs.push_back(job);
                    if (!SetEvent(m_WakeEvent)) {
                        m_Jobs.pop_back();
                        object->Release();
                        return false;
                    }
                }
                return true;
            }

        private:
            struct Job
            {
                IDataObject* object {nullptr};
            };

            void run()
            {
                if (m_WakeEvent == nullptr) {
                    std::lock_guard<std::mutex> lock(m_Mutex);
                    m_Initialized = true;
                    m_Ready.notify_all();
                    return;
                }

                const HRESULT initializeResult =
                        OleInitialize(nullptr);
                {
                    std::lock_guard<std::mutex> lock(m_Mutex);
                    m_OleAvailable = SUCCEEDED(initializeResult);
                    m_Initialized = true;
                }
                m_Ready.notify_all();
                if (!m_OleAvailable) {
                    return;
                }

                for (;;) {
                    const DWORD waitResult =
                            MsgWaitForMultipleObjects(
                                1,
                                &m_WakeEvent,
                                FALSE,
                                INFINITE,
                                QS_ALLINPUT);
                    if (waitResult == WAIT_OBJECT_0) {
                        std::deque<std::shared_ptr<Job>> jobs;
                        bool stopping;
                        {
                            std::lock_guard<std::mutex> lock(m_Mutex);
                            jobs.swap(m_Jobs);
                            stopping = m_Stopping;
                            ResetEvent(m_WakeEvent);
                        }
                        for (const auto& job : jobs) {
                            OleSetClipboard(job->object);
                            job->object->Release();
                        }
                        if (stopping) {
                            break;
                        }
                    }
                    else if (waitResult == WAIT_OBJECT_0 + 1) {
                        MSG message;
                        while (PeekMessageW(
                                   &message,
                                   nullptr,
                                   0,
                                   0,
                                   PM_REMOVE)) {
                            TranslateMessage(&message);
                            DispatchMessageW(&message);
                        }
                    }
                    else {
                        break;
                    }
                }
                OleUninitialize();
            }

            HANDLE m_WakeEvent;
            std::thread m_Thread;
            std::mutex m_Mutex;
            std::condition_variable m_Ready;
            std::deque<std::shared_ptr<Job>> m_Jobs;
            bool m_Initialized {false};
            bool m_OleAvailable {false};
            bool m_Stopping {false};
        };

        OleClipboardBroker& clipboardBroker()
        {
            static OleClipboardBroker broker;
            return broker;
        }
    }

    bool setRemoteFiles(const QByteArray& markerMime,
                        const QByteArray& markerData,
                        ManifestCallback manifestCallback,
                        ReadCallback readCallback)
    {
        const QString markerName = QString::fromLatin1(markerMime);
        const UINT markerFormat =
                RegisterClipboardFormatW(
                    reinterpret_cast<LPCWSTR>(markerName.utf16()));
        if (markerFormat == 0 ||
                markerData.isEmpty() ||
                !manifestCallback ||
                !readCallback) {
            return false;
        }

        auto* object = new (std::nothrow) VirtualFileDataObject(
                    markerData,
                    std::move(manifestCallback),
                    std::move(readCallback),
                    markerFormat);
        if (object == nullptr) {
            return false;
        }
        if (!object->isValid()) {
            object->Release();
            return false;
        }

        return clipboardBroker().set(object);
    }

    bool hasMarker(const QByteArray& markerMime)
    {
        const QString markerName = QString::fromLatin1(markerMime);
        const UINT markerFormat =
                RegisterClipboardFormatW(
                    reinterpret_cast<LPCWSTR>(markerName.utf16()));
        return markerFormat != 0 &&
                IsClipboardFormatAvailable(markerFormat) != FALSE;
    }

    bool hasRemoteFiles(const QByteArray& markerMime)
    {
        const UINT descriptorFormat =
                RegisterClipboardFormatW(L"FileGroupDescriptorW");
        return hasMarker(markerMime) &&
                descriptorFormat != 0 &&
                IsClipboardFormatAvailable(descriptorFormat) != FALSE;
    }
}
