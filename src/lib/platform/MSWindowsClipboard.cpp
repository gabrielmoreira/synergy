/*
 * Deskflow -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2002 Chris Schoeneman
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "platform/MSWindowsClipboard.h"

#include "arch/win32/ArchMiscWindows.h"
#include "base/Log.h"
#include "platform/MSWindowsClipboardBitmapConverter.h"
#include "platform/MSWindowsClipboardFacade.h"
#include "platform/MSWindowsClipboardHTMLConverter.h"
#include "platform/MSWindowsClipboardTextConverter.h"
#include "platform/MSWindowsClipboardUTF16Converter.h"

// GDI+ for PNG/JPEG encoding of clipboard bitmaps
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objidl.h>
#pragma warning(push)
#pragma warning(disable : 4458) // declaration of 'x' hides class member
#include <gdiplus.h>
#pragma warning(pop)
#pragma comment(lib, "gdiplus.lib")

#include <cmath>
#include <cstdlib>
#include <memory>
#include <vector>

//
// GDI+ lifecycle management (lazy init, lives for the process lifetime)
//

namespace {

class GdiplusGuard
{
public:
  GdiplusGuard()
  {
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&m_token, &input, nullptr);
  }
  ~GdiplusGuard()
  {
    Gdiplus::GdiplusShutdown(m_token);
  }

private:
  ULONG_PTR m_token = 0;
};

void ensureGdiplus()
{
  // Intentionally leaked — GDI+ must outlive all GDI+ objects.
  static GdiplusGuard *s_guard = new GdiplusGuard();
  (void)s_guard;
}

// Helper: get GDI+ encoder CLSID for a given MIME type (e.g. "image/png")
bool getEncoderClsid(const wchar_t *format, CLSID *pClsid)
{
  UINT num = 0, size = 0;
  Gdiplus::GetImageEncodersSize(&num, &size);
  if (size == 0)
    return false;

  auto *buf = reinterpret_cast<Gdiplus::ImageCodecInfo *>(new char[size]);
  Gdiplus::GetImageEncoders(num, size, buf);

  bool found = false;
  for (UINT i = 0; i < num; ++i) {
    if (wcscmp(buf[i].MimeType, format) == 0) {
      *pClsid = buf[i].Clsid;
      found = true;
      break;
    }
  }
  delete[] reinterpret_cast<char *>(buf);
  return found;
}

// Helper: write a GDI+ Bitmap to an in-memory IStream, return the raw bytes
bool bitmapToStream(Gdiplus::Bitmap &bmp, const CLSID &encoderClsid, const Gdiplus::EncoderParameters *params,
                    std::vector<BYTE> &outBytes)
{
  IStream *stream = nullptr;
  if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)))
    return false;

  Gdiplus::Status status = bmp.Save(stream, &encoderClsid, params);
  if (status != Gdiplus::Ok) {
    stream->Release();
    return false;
  }

  STATSTG stat{};
  stream->Stat(&stat, STATFLAG_NONAME);
  ULARGE_INTEGER size = stat.cbSize;

  if (size.QuadPart == 0) {
    stream->Release();
    return false;
  }

  outBytes.resize(static_cast<size_t>(size.QuadPart));
  LARGE_INTEGER zero{};
  stream->Seek(zero, STREAM_SEEK_SET, nullptr);
  ULONG read = 0;
  stream->Read(outBytes.data(), static_cast<ULONG>(outBytes.size()), &read);
  stream->Release();

  return read == static_cast<ULONG>(outBytes.size());
}

// Helper: allocate a moveable HGLOBAL from a byte buffer
HGLOBAL bytesToHGlobal(const std::vector<BYTE> &bytes)
{
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
  if (!h)
    return nullptr;
  void *p = GlobalLock(h);
  if (!p) {
    GlobalFree(h);
    return nullptr;
  }
  memcpy(p, bytes.data(), bytes.size());
  GlobalUnlock(h);
  return h;
}

// Detects and fixes the BI_BITFIELDS mask-stripping bug introduced by the old
// OSXClipboardBMPConverter::toIClipboard() implementation.
//
// Background: Deskflow's internal kBitmap format is a raw DIB (no BMP file header).
// Old versions of the macOS converter stripped the 12 mask bytes from BI_BITFIELDS
// DIBs but left biCompression=BI_BITFIELDS in the header — an inconsistent state
// that confuses GDI+. Newer versions (post OSX fix) preserve the full structure.
//
// This function detects which case we have and returns:
//   outBfOffBits — the correct BMP file pixel offset for GDI+ to decode properly
//   outFixedDib  — either the original dibData (if well-formed), or a patched copy
//                  with biCompression changed to BI_RGB (if masks were stripped).
void prepareDibForGdiPlus(
    const String     &dibData,
    const BITMAPINFOHEADER *infoHdr,
    DWORD            &outBfOffBits,
    const char      **outDibPtr,
    size_t           &outDibSize,
    String           &scratchBuf)
{
  const DWORD biSize      = infoHdr->biSize;
  const DWORD compression = infoHdr->biCompression;
  const WORD  bpp         = infoHdr->biBitCount;
  const DWORD clrUsed     = infoHdr->biClrUsed;

  DWORD paletteEntries = (bpp <= 8) ? (clrUsed ? clrUsed : (1u << bpp)) : 0;
  DWORD paletteSize    = paletteEntries * sizeof(RGBQUAD);

  const DWORD BI_BITFIELDS_VAL      = 3;
  const DWORD BI_ALPHABITFIELDS_VAL = 6;

  bool isBitfields = (biSize == 40) &&
                     (compression == BI_BITFIELDS_VAL || compression == BI_ALPHABITFIELDS_VAL);

  if (!isBitfields) {
    // Standard BI_RGB or other — no fixup needed
    outBfOffBits = 14 + biSize + paletteSize;
    *outDibPtr   = dibData.data();
    outDibSize   = dibData.size();
    return;
  }

  // BI_BITFIELDS: detect whether the 12 mask bytes are actually in the buffer.
  // Strategy: use (dib_size - expected_pixels_size) to find where pixels actually start.
  bool masksPresent = false;
  if (infoHdr->biWidth > 0 && infoHdr->biHeight != 0 && bpp > 0) {
    DWORD stride     = ((infoHdr->biWidth * bpp + 31) / 32) * 4;
    DWORD pixelBytes = stride * (DWORD)std::abs(infoHdr->biHeight);
    if (dibData.size() > pixelBytes) {
      size_t actualPixelOffset = dibData.size() - pixelBytes;
      // With masks: pixelOffset should be ≥ 40 + 12 = 52
      masksPresent = (actualPixelOffset >= biSize + 12);
    }
  }

  if (masksPresent) {
    // Well-formed DIB — masks are present, use it as-is
    LOG((CLOG_DEBUG "clipboard: BI_BITFIELDS DIB is well-formed (masks present)"));
    outBfOffBits = 14 + biSize + 12 + paletteSize;
    *outDibPtr   = dibData.data();
    outDibSize   = dibData.size();
  } else {
    // Old Synergy bug: masks were stripped but biCompression still says BI_BITFIELDS.
    // Patch biCompression → BI_RGB in a scratch copy so GDI+ reads pixels correctly.
    LOG((CLOG_DEBUG "clipboard: BI_BITFIELDS DIB missing mask bytes (pre-fix Synergy), "
                    "patching biCompression to BI_RGB for GDI+ decode"));
    scratchBuf = dibData;
    reinterpret_cast<BITMAPINFOHEADER *>(&scratchBuf[0])->biCompression = BI_RGB;
    outBfOffBits = 14 + biSize + paletteSize; // no masks → pixels right after header
    *outDibPtr   = scratchBuf.data();
    outDibSize   = scratchBuf.size();
  }
}

enum class OriginHint {
  Unknown,
  PNG, // Likely a screenshot or window capture (32bpp/BITFIELDS)
  JPEG // Likely a photo (24bpp)
};

OriginHint detectOrigin(const BITMAPINFOHEADER *infoHdr)
{
  if (infoHdr->biBitCount == 32 || infoHdr->biCompression == 3 /* BI_BITFIELDS */) {
    return OriginHint::PNG;
  }
  if (infoHdr->biBitCount == 24) {
    return OriginHint::JPEG;
  }
  return OriginHint::Unknown;
}

// Trims transparent edges from a 32bpp ARGB bitmap.
// Ported from ClipFixSynergy's Img_TrimTransparent.
std::unique_ptr<Gdiplus::Bitmap> trimTransparentEdges(Gdiplus::Bitmap *src, BYTE alphaThreshold = 1)
{
  if (!src || src->GetPixelFormat() != PixelFormat32bppARGB)
    return nullptr;

  const int w = src->GetWidth();
  const int h = src->GetHeight();

  int minX = w, minY = h, maxX = -1, maxY = -1;

  Gdiplus::BitmapData data;
  Gdiplus::Rect       rect(0, 0, w, h);
  if (src->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) != Gdiplus::Ok)
    return nullptr;

  const BYTE *pixels = static_cast<const BYTE *>(data.Scan0);
  for (int y = 0; y < h; ++y) {
    const BYTE *row = pixels + (y * data.Stride);
    for (int x = 0; x < w; ++x) {
      BYTE a = row[x * 4 + 3]; // ARGB
      if (a >= alphaThreshold) {
        if (x < minX)
          minX = x;
        if (y < minY)
          minY = y;
        if (x > maxX)
          maxX = x;
        if (y > maxY)
          maxY = y;
      }
    }
  }
  src->UnlockBits(&data);

  if (maxX < minX || maxY < minY) {
    // Completely transparent: return a 1x1 empty bitmap
    return std::make_unique<Gdiplus::Bitmap>(1, 1, PixelFormat32bppARGB);
  }

  // Only trim if there's actually something to trim
  if (minX == 0 && minY == 0 && maxX == w - 1 && maxY == h - 1)
    return nullptr;

  int outW = (maxX - minX) + 1;
  int outH = (maxY - minY) + 1;

  auto dst = std::make_unique<Gdiplus::Bitmap>(outW, outH, PixelFormat32bppARGB);
  Gdiplus::Graphics g(dst.get());
  g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
  g.DrawImage(src, 0, 0, minX, minY, outW, outH, Gdiplus::UnitPixel);

  return dst;
}

// Inject PNG and JPEG clipboard formats derived from a raw DIB.
// The clipboard must already be open.
void injectPngAndJpeg(const String &dibData)
{
  if (dibData.size() < sizeof(BITMAPINFOHEADER))
    return;

  ensureGdiplus();

  const auto *infoHdr = reinterpret_cast<const BITMAPINFOHEADER *>(dibData.data());
  OriginHint  origin  = detectOrigin(infoHdr);

  // Detect format and apply fixup for the old BI_BITFIELDS mask-stripping bug if needed
  DWORD       bfOffBits = 0;
  const char *dibPtr    = nullptr;
  size_t      dibSize   = 0;
  String      scratchBuf;
  prepareDibForGdiPlus(dibData, infoHdr, bfOffBits, &dibPtr, dibSize, scratchBuf);

  // Reconstruct a BMP file in memory (14-byte file header + DIB data)
  const size_t bmpSize = 14 + dibSize;
  HGLOBAL      hMem    = GlobalAlloc(GMEM_MOVEABLE, bmpSize);
  if (!hMem)
    return;

  {
    void *p = GlobalLock(hMem);
    if (!p) {
      GlobalFree(hMem);
      return;
    }
    auto *dst = reinterpret_cast<BYTE *>(p);
    dst[0]    = 'B';
    dst[1]    = 'M';
    *reinterpret_cast<DWORD *>(dst + 2)  = static_cast<DWORD>(bmpSize);
    *reinterpret_cast<WORD *>(dst + 6)   = 0;
    *reinterpret_cast<WORD *>(dst + 8)   = 0;
    *reinterpret_cast<DWORD *>(dst + 10) = bfOffBits;
    memcpy(dst + 14, dibPtr, dibSize);
    GlobalUnlock(hMem);
  }

  IStream *stream = nullptr;
  if (FAILED(CreateStreamOnHGlobal(hMem, TRUE, &stream)))
    return;

  // Load into GDI+ Bitmap
  auto gdiBmp = std::unique_ptr<Gdiplus::Bitmap>(Gdiplus::Bitmap::FromStream(stream));
  stream->Release(); // releases hMem (fDeleteOnRelease)

  if (!gdiBmp || gdiBmp->GetLastStatus() != Gdiplus::Ok) {
    LOG((CLOG_WARN "clipboard: GDI+ failed to load DIB for PNG/JPEG injection"));
    return;
  }

  // Apply Trim if it's a 32bpp image (likely macOS window capture with large shadows)
#ifdef SYNERGY_USE_CLIPBOARD_TRIM
  if (gdiBmp->GetPixelFormat() == PixelFormat32bppARGB) {
    auto trimmed = trimTransparentEdges(gdiBmp.get());
    if (trimmed) {
      LOG((CLOG_INFO "clipboard: trimmed transparent edges (%dx%d -> %dx%d)",
           gdiBmp->GetWidth(), gdiBmp->GetHeight(), trimmed->GetWidth(), trimmed->GetHeight()));
      gdiBmp = std::move(trimmed);
    }
  }
#endif

  // --- Encoding helper lambda ---
  auto injectFormat = [&](const wchar_t *mime, const Gdiplus::EncoderParameters *params, const char *label) {
    CLSID clsid;
    if (getEncoderClsid(mime, &clsid)) {
      std::vector<BYTE> bytes;
      bool              isJpeg = (wcscmp(mime, L"image/jpeg") == 0);

      if (isJpeg) {
        // JPEG doesn't support transparency. Flatten to 24bpp RGB on white.
        UINT            w = gdiBmp->GetWidth();
        UINT            h = gdiBmp->GetHeight();
        Gdiplus::Bitmap flat24(w, h, PixelFormat24bppRGB);
        Gdiplus::Graphics g(&flat24);
        g.Clear(Gdiplus::Color(255, 255, 255, 255));
        g.DrawImage(gdiBmp.get(), 0, 0, (INT)w, (INT)h);
        if (!bitmapToStream(flat24, clsid, params, bytes))
          return;
      } else {
        if (!bitmapToStream(*gdiBmp, clsid, params, bytes))
          return;
      }

      UINT cf1 = RegisterClipboardFormat(label);
      UINT cf2 = (wcscmp(mime, L"image/png") == 0) ? RegisterClipboardFormat(TEXT("image/png"))
                                                   : RegisterClipboardFormat(TEXT("image/jpeg"));

      HGLOBAL h1 = bytesToHGlobal(bytes);
      HGLOBAL h2 = bytesToHGlobal(bytes);
      if (h1)
        SetClipboardData(cf1, h1);
      if (h2)
        SetClipboardData(cf2, h2);

      LOG((CLOG_INFO "clipboard: injected %s (%zu bytes) into clipboard", label, bytes.size()));
    }
  };

  // --- Encoder Params ---
  Gdiplus::EncoderParameters jpegParams;
  jpegParams.Count                      = 1;
  jpegParams.Parameter[0].Guid          = Gdiplus::EncoderQuality;
  jpegParams.Parameter[0].Type          = Gdiplus::EncoderParameterValueTypeLong;
  jpegParams.Parameter[0].NumberOfValues = 1;
  ULONG jpegQuality                     = 95;
  jpegParams.Parameter[0].Value         = &jpegQuality;

  // Prioritize injection based on origin heuristic
  if (origin == OriginHint::JPEG) {
    // Handle JPEG first
    injectFormat(L"image/jpeg", &jpegParams, "image/jpeg");
    injectFormat(L"image/png", nullptr, "PNG");
  } else {
    // Default or PNG origin: Handle PNG first
    injectFormat(L"image/png", nullptr, "PNG");
    injectFormat(L"image/jpeg", &jpegParams, "image/jpeg");
  }
}

} // anonymous namespace

//
// MSWindowsClipboard
//

UINT MSWindowsClipboard::s_ownershipFormat = 0;

MSWindowsClipboard::MSWindowsClipboard(HWND window)
    : m_window(window),
      m_time(0),
      m_facade(new MSWindowsClipboardFacade()),
      m_deleteFacade(true)
{
  // add converters, most desired first
  m_converters.push_back(new MSWindowsClipboardUTF16Converter);
  m_converters.push_back(new MSWindowsClipboardBitmapConverter);
  m_converters.push_back(new MSWindowsClipboardHTMLConverter);
}

MSWindowsClipboard::~MSWindowsClipboard()
{
  clearConverters();

  // dependency injection causes confusion over ownership, so we need
  // logic to decide whether or not we delete the facade. there must
  // be a more elegant way of doing this.
  if (m_deleteFacade)
    delete m_facade;
}

void MSWindowsClipboard::setFacade(IMSWindowsClipboardFacade &facade)
{
  delete m_facade;
  m_facade = &facade;
  m_deleteFacade = false;
}

bool MSWindowsClipboard::emptyUnowned()
{
  LOG((CLOG_DEBUG "empty clipboard"));

  // empty the clipboard (and take ownership)
  if (!EmptyClipboard()) {
    // unable to cause this in integ tests, but this error has never
    // actually been reported by users.
    LOG((CLOG_DEBUG "failed to grab clipboard"));
    return false;
  }

  return true;
}

bool MSWindowsClipboard::empty()
{
  if (!emptyUnowned()) {
    return false;
  }

  // mark clipboard as being owned by deskflow
  HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, 1);
  if (NULL == SetClipboardData(getOwnershipFormat(), data)) {
    LOG((CLOG_DEBUG "failed to set clipboard data"));
    GlobalFree(data);
    return false;
  }

  return true;
}

void MSWindowsClipboard::add(EFormat format, const String &data)
{
  bool isSucceeded = false;
  // convert data to win32 form
  for (ConverterList::const_iterator index = m_converters.begin(); index != m_converters.end(); ++index) {
    IMSWindowsClipboardConverter *converter = *index;

    // skip converters for other formats
    if (converter->getFormat() == format) {
      HANDLE win32Data = converter->fromIClipboard(data);
      if (win32Data != NULL) {
        LOG((CLOG_DEBUG "add %d bytes to clipboard format: %d", data.size(), format));
        m_facade->write(win32Data, converter->getWin32Format());
        isSucceeded = true;

        // For bitmap data, also inject PNG and JPEG so browsers and modern
        // apps can paste images correctly (Synergy only sends CF_DIB by default).
        if (format == IClipboard::kBitmap) {
          injectPngAndJpeg(data);
        }

        break;
      } else {
        LOG((CLOG_DEBUG "failed to convert clipboard data to platform format"));
      }
    }
  }

  if (!isSucceeded) {
    LOG((CLOG_DEBUG "missed clipboard data convert for format: %d", format));
  }
}

bool MSWindowsClipboard::open(Time time) const
{
  LOG((CLOG_DEBUG "open clipboard"));

  if (!OpenClipboard(m_window)) {
    LOG((CLOG_WARN "failed to open clipboard: %d", GetLastError()));
    return false;
  }

  m_time = time;

  return true;
}

void MSWindowsClipboard::close() const
{
  LOG((CLOG_DEBUG "close clipboard"));
  CloseClipboard();
}

IClipboard::Time MSWindowsClipboard::getTime() const
{
  return m_time;
}

bool MSWindowsClipboard::has(EFormat format) const
{
  for (ConverterList::const_iterator index = m_converters.begin(); index != m_converters.end(); ++index) {
    IMSWindowsClipboardConverter *converter = *index;
    if (converter->getFormat() == format) {
      if (IsClipboardFormatAvailable(converter->getWin32Format())) {
        return true;
      }
    }
  }
  return false;
}

String MSWindowsClipboard::get(EFormat format) const
{
  // find the converter for the first clipboard format we can handle
  IMSWindowsClipboardConverter *converter = NULL;
  for (ConverterList::const_iterator index = m_converters.begin(); index != m_converters.end(); ++index) {

    converter = *index;
    if (converter->getFormat() == format) {
      break;
    }
    converter = NULL;
  }

  // if no converter then we don't recognize any formats
  if (converter == NULL) {
    LOG((CLOG_WARN "no converter for format %d", format));
    return String();
  }

  // get a handle to the clipboard data
  HANDLE win32Data = GetClipboardData(converter->getWin32Format());
  if (win32Data == NULL) {
    // nb: can't cause this using integ tests; this is only caused when
    // the selected converter returns an invalid format -- which you
    // cannot cause using public functions.
    return String();
  }

  // convert
  return converter->toIClipboard(win32Data);
}

void MSWindowsClipboard::clearConverters()
{
  for (ConverterList::iterator index = m_converters.begin(); index != m_converters.end(); ++index) {
    delete *index;
  }
  m_converters.clear();
}

bool MSWindowsClipboard::isOwnedByDeskflow()
{
  // create ownership format if we haven't yet
  if (s_ownershipFormat == 0) {
    s_ownershipFormat = RegisterClipboardFormat(TEXT(DESKFLOW_APP_NAME "Ownership"));
  }
  return (IsClipboardFormatAvailable(getOwnershipFormat()) != 0);
}

UINT MSWindowsClipboard::getOwnershipFormat()
{
  // create ownership format if we haven't yet
  if (s_ownershipFormat == 0) {
    s_ownershipFormat = RegisterClipboardFormat(TEXT(DESKFLOW_APP_NAME "Ownership"));
  }

  // return the format
  return s_ownershipFormat;
}
