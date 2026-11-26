#include "engine/editor/NetworkChartPng.h"

#include <stdexcept>

#include "engine/core/AtomicFile.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <vector>

#endif

namespace engine::editor {

#if defined(_WIN32)
namespace {

constexpr int MaxImageSide = 4'096;
constexpr std::uint64_t MaxImagePixels = 4'194'304;
constexpr std::uint64_t MaxPngBytes = MaxImagePixels * 4 + 262'144;

void check(Gdiplus::Status status, const char* error) {
    if (status != Gdiplus::Ok) throw std::runtime_error(error);
}

class GdiPlusSession final {
public:
    GdiPlusSession() {
        Gdiplus::GdiplusStartupInput input;
        check(Gdiplus::GdiplusStartup(&m_token, &input, nullptr), "Could not start the PNG renderer.");
    }
    ~GdiPlusSession() { Gdiplus::GdiplusShutdown(m_token); }
    GdiPlusSession(const GdiPlusSession&) = delete;
    GdiPlusSession& operator=(const GdiPlusSession&) = delete;

private:
    ULONG_PTR m_token{};
};

Gdiplus::Color gdiColor(ImU32 color) {
    const auto channel = [color](int shift) { return static_cast<BYTE>((color >> shift) & 0xffU); };
    return {channel(IM_COL32_A_SHIFT), channel(IM_COL32_R_SHIFT), channel(IM_COL32_G_SHIFT), channel(IM_COL32_B_SHIFT)};
}

Gdiplus::PointF gdiPoint(ImVec2 point) { return {point.x, point.y}; }

Gdiplus::RectF gdiRect(ImVec2 min, ImVec2 max) { return {min.x, min.y, max.x - min.x, max.y - min.y}; }

std::wstring wideText(const char* utf8) {
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, nullptr, 0);
    if (count == 0) throw std::runtime_error("A chart label is not valid UTF-8.");
    std::wstring text(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, text.data(), count) != count) {
        throw std::runtime_error("Could not convert a chart label.");
    }
    text.pop_back();
    return text;
}

CLSID pngEncoder() {
    UINT count{}, size{};
    check(Gdiplus::GetImageEncodersSize(&count, &size), "Could not query image encoders.");
    if (count == 0 || size == 0 || size > 1'048'576) throw std::runtime_error("The Windows PNG encoder is unavailable.");
    std::vector<std::max_align_t> storage((size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
    check(Gdiplus::GetImageEncoders(count, size, encoders), "Could not read the image encoder list.");
    for (UINT index = 0; index < count; ++index) {
        if (encoders[index].MimeType && std::wcscmp(encoders[index].MimeType, L"image/png") == 0) return encoders[index].Clsid;
    }
    throw std::runtime_error("The Windows PNG encoder is unavailable.");
}

}

struct NetworkChartPng::Impl {
    GdiPlusSession session;
    Gdiplus::Bitmap bitmap;
    Gdiplus::Graphics graphics;
    std::unique_ptr<Gdiplus::Font> font;
    std::vector<Gdiplus::GraphicsState> clipStates;

    Impl(int width, int height) : bitmap(width, height, PixelFormat32bppARGB), graphics(&bitmap) {
        check(bitmap.GetLastStatus(), "Could not allocate the PNG canvas.");
        check(graphics.GetLastStatus(), "Could not create the PNG renderer.");
        check(graphics.SetPageUnit(Gdiplus::UnitPixel), "Could not set PNG pixel units.");
        check(graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias), "Could not enable antialiased charts.");
        check(graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit), "Could not set chart text rendering.");
    }
};

NetworkChartPng::NetworkChartPng(int width, int height) {
    if (width <= 0 || height <= 0 || width > MaxImageSide || height > MaxImageSide ||
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) > MaxImagePixels) {
        throw std::invalid_argument("Invalid chart image dimensions.");
    }
    m_impl = std::make_unique<Impl>(width, height);
    setTextSize(16.0F);
}

NetworkChartPng::~NetworkChartPng() = default;

void NetworkChartPng::AddRectFilled(ImVec2 min, ImVec2 max, ImU32 color) {
    Gdiplus::SolidBrush brush(gdiColor(color));
    check(m_impl->graphics.FillRectangle(&brush, gdiRect(min, max)), "Could not draw a chart rectangle.");
}

void NetworkChartPng::AddLine(ImVec2 a, ImVec2 b, ImU32 color, float thickness) {
    Gdiplus::Pen pen(gdiColor(color), thickness);
    check(m_impl->graphics.DrawLine(&pen, gdiPoint(a), gdiPoint(b)), "Could not draw a chart line.");
}

void NetworkChartPng::AddPolyline(const ImVec2* points, int count, ImU32 color, ImDrawFlags flags, float thickness) {
    if (count < 2) return;
    std::vector<Gdiplus::PointF> line;
    line.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) line.push_back(gdiPoint(points[index]));
    Gdiplus::Pen pen(gdiColor(color), thickness);
    check(pen.SetLineJoin(Gdiplus::LineJoinRound), "Could not set chart line joins.");
    check((flags & ImDrawFlags_Closed) != 0 && count >= 3 ? m_impl->graphics.DrawPolygon(&pen, line.data(), count)
                                                       : m_impl->graphics.DrawLines(&pen, line.data(), count),
        "Could not draw a chart series.");
}

void NetworkChartPng::AddCircle(ImVec2 center, float radius, ImU32 color, int) {
    Gdiplus::Pen pen(gdiColor(color));
    check(m_impl->graphics.DrawEllipse(&pen, center.x - radius, center.y - radius, radius * 2.0F, radius * 2.0F),
        "Could not draw a chart marker.");
}

void NetworkChartPng::AddCircleFilled(ImVec2 center, float radius, ImU32 color, int) {
    Gdiplus::SolidBrush brush(gdiColor(color));
    check(m_impl->graphics.FillEllipse(&brush, center.x - radius, center.y - radius, radius * 2.0F, radius * 2.0F),
        "Could not draw a chart marker.");
}

void NetworkChartPng::PushClipRect(ImVec2 min, ImVec2 max, bool intersect) {
    const auto state = m_impl->graphics.Save();
    check(m_impl->graphics.GetLastStatus(), "Could not save chart clipping state.");
    m_impl->clipStates.push_back(state);
    check(m_impl->graphics.SetClip(gdiRect(min, max), intersect ? Gdiplus::CombineModeIntersect : Gdiplus::CombineModeReplace),
        "Could not clip the chart.");
}

void NetworkChartPng::PopClipRect() {
    if (m_impl->clipStates.empty()) throw std::logic_error("Chart clipping state is empty.");
    check(m_impl->graphics.Restore(m_impl->clipStates.back()), "Could not restore chart clipping state.");
    m_impl->clipStates.pop_back();
}

void NetworkChartPng::AddText(ImVec2 pos, ImU32 color, const char* utf8) {
    const auto text = wideText(utf8);
    if (text.empty()) return;
    Gdiplus::SolidBrush brush(gdiColor(color));
    check(m_impl->graphics.DrawString(text.c_str(), -1, m_impl->font.get(), gdiPoint(pos),
        Gdiplus::StringFormat::GenericTypographic(), &brush), "Could not draw a chart label.");
}

float NetworkChartPng::textWidth(const char* utf8) const {
    const auto text = wideText(utf8);
    if (text.empty()) return 0.0F;
    Gdiplus::RectF bounds;
    check(m_impl->graphics.MeasureString(text.c_str(), -1, m_impl->font.get(), Gdiplus::PointF{},
        Gdiplus::StringFormat::GenericTypographic(), &bounds), "Could not measure a chart label.");
    return bounds.Width;
}

float NetworkChartPng::textHeight() const { return m_impl->font->GetHeight(&m_impl->graphics); }

void NetworkChartPng::setTextSize(float pixels) {
    const Gdiplus::FontFamily family(L"Segoe UI");
    const auto* selected = family.GetLastStatus() == Gdiplus::Ok ? &family : Gdiplus::FontFamily::GenericSansSerif();
    auto font = std::make_unique<Gdiplus::Font>(selected, pixels, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    check(font->GetLastStatus(), "Could not create the chart font.");
    m_impl->font = std::move(font);
}

std::optional<std::string> NetworkChartPng::encode(std::string& error) {
    error.clear();
    try {
        const auto encoder = pngEncoder();
        Microsoft::WRL::ComPtr<IStream> stream;
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.GetAddressOf()))) {
            error = "Could not allocate the PNG output stream.";
            return std::nullopt;
        }
        m_impl->graphics.Flush(Gdiplus::FlushIntentionSync);
        check(m_impl->bitmap.Save(stream.Get(), &encoder), "Could not encode the PNG image.");
        STATSTG stat{};
        if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart == 0 || stat.cbSize.QuadPart > MaxPngBytes ||
            FAILED(stream->Seek(LARGE_INTEGER{}, STREAM_SEEK_SET, nullptr))) {
            error = "Could not read the encoded PNG stream.";
            return std::nullopt;
        }
        std::string bytes(static_cast<std::size_t>(stat.cbSize.QuadPart), '\0');
        ULONG read{};
        if (FAILED(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read)) || read != bytes.size()) {
            error = "Could not read the complete encoded PNG image.";
            return std::nullopt;
        }
        return bytes;
    } catch (const std::exception& failure) {
        error = failure.what();
        return std::nullopt;
    }
}

#else

struct NetworkChartPng::Impl {};

NetworkChartPng::NetworkChartPng(int, int) { throw std::runtime_error("Chart PNG export is unavailable on this platform."); }
NetworkChartPng::~NetworkChartPng() = default;
void NetworkChartPng::AddRectFilled(ImVec2, ImVec2, ImU32) {}
void NetworkChartPng::AddLine(ImVec2, ImVec2, ImU32, float) {}
void NetworkChartPng::AddPolyline(const ImVec2*, int, ImU32, ImDrawFlags, float) {}
void NetworkChartPng::AddCircle(ImVec2, float, ImU32, int) {}
void NetworkChartPng::AddCircleFilled(ImVec2, float, ImU32, int) {}
void NetworkChartPng::PushClipRect(ImVec2, ImVec2, bool) {}
void NetworkChartPng::PopClipRect() {}
void NetworkChartPng::AddText(ImVec2, ImU32, const char*) {}
float NetworkChartPng::textWidth(const char*) const { return 0.0F; }
float NetworkChartPng::textHeight() const { return 0.0F; }
void NetworkChartPng::setTextSize(float) {}
std::optional<std::string> NetworkChartPng::encode(std::string& error) {
    error = "Chart PNG export is unavailable on this platform.";
    return std::nullopt;
}

#endif

bool NetworkChartPng::save(const std::filesystem::path& path, std::string& error) {
    const auto bytes = encode(error);
    return bytes && core::writeFileAtomically(path, *bytes, error);
}

}
