// Exercises the real Win32 KEYFBUFF.ASM blitter against a scalar pixel reference.
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

extern "C" {
unsigned char* BigShapeBufferStart = nullptr;
unsigned char* TheaterShapeBufferStart = nullptr;
int UseBigShapeBuffer = 0;
int IsTheaterShape = 0;
int MMXAvailable = 0;
void MMX_Done() {}
void __cdecl Buffer_Frame_To_Page(int, int, int, int, const void*, void*, int, ...);
}

struct Viewport {
    unsigned char* pixels;
    int width, height, x_add, x, y, pitch;
    void* buffer;
};
struct ShapeHeader { int flags; std::uint32_t data_offset; int theater; };
static_assert(sizeof(Viewport) == 32, "This test must be compiled for Win32.");
static_assert(sizeof(ShapeHeader) == 12, "Assembly shape header layout changed.");

const int TRANS = 0x40, GHOST = 0x1000, FADING = 0x100, PREDATOR = 0x200;

// Keep SEH in a function without C++ destructors so a bad assembly jump is
// reported as a regression instead of leaving a Windows crash dialog open.
static unsigned long draw(int x, int y, int width, int height, const void* shape,
                          Viewport* viewport, int flags, const void* ghost,
                          const void* fade, int fade_count, int predator_index) {
    __try {
        if (flags & GHOST) {
            if (flags & FADING) {
                Buffer_Frame_To_Page(x, y, width, height, shape, viewport, flags,
                                     ghost, fade, fade_count, predator_index);
            } else {
                Buffer_Frame_To_Page(x, y, width, height, shape, viewport, flags,
                                     ghost, predator_index);
            }
        } else if (flags & FADING) {
            Buffer_Frame_To_Page(x, y, width, height, shape, viewport, flags,
                                 fade, fade_count, predator_index);
        } else {
            Buffer_Frame_To_Page(x, y, width, height, shape, viewport, flags,
                                 predator_index);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
    return 0;
}

static int run_case(int width, int height, int x, int y, int flags, int cache_mode,
                    int fade_count, int predator_index) {
    const int screen_width = 360, screen_height = 8, stride = screen_width + 11;
    const int guard = 1024;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(std::max(width, 1) * std::max(height, 1)));
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = i % 7 == 0 ? 0 : static_cast<unsigned char>((i * 31 + 5) % 255 + 1);
    }
    const auto original_pixels = pixels;
    std::vector<unsigned char> ghost(256 + 256 * 256);
    std::vector<unsigned char> fade(256);
    for (int color = 0; color < 256; ++color) {
        ghost[color] = color % 3 == 0 ? static_cast<unsigned char>((color / 3) % 255) : 255;
        fade[color] = static_cast<unsigned char>((color * 3 + 17) & 255);
        for (int dest = 0; dest < 256; ++dest) {
            ghost[256 + color * 256 + dest] = static_cast<unsigned char>((color * 11 + dest * 5 + 9) & 255);
        }
    }
    std::vector<unsigned char> actual(guard * 2 + stride * screen_height);
    for (std::size_t i = 0; i < actual.size(); ++i) {
        actual[i] = static_cast<unsigned char>((i * 13 + 19) & 255);
    }
    auto expected = actual;
    Viewport viewport = {actual.data() + guard, screen_width, screen_height, 4, 0, 0, 7, nullptr};
    const int left = std::max(x, 0), right = std::min(x + width, screen_width);
    const int top = std::max(y, 0), bottom = std::min(y + height, screen_height);
    const int offsets[] = {1, 3, 2, 5, 2, 3, 4, 1};
    const int negative_offsets[] = {-1, -3, -2, -5, -2, -4, -3, -1};
    int predator_cursor = std::abs(predator_index) & 7;
    for (int row = top; row < bottom; ++row) {
        for (int col = left; col < right; ++col) {
            unsigned char color = pixels[static_cast<std::size_t>((row - y) * width + col - x)];
            if ((flags & TRANS) && color == 0) continue;
            const int position = guard + row * stride + col;
            if (flags & PREDATOR) {
                const int offset = predator_index < 0 ? stride + negative_offsets[predator_cursor] : offsets[predator_cursor];
                color = expected[position + offset];
                predator_cursor = (predator_cursor + 1) & 7;
            }
            if ((flags & GHOST) && ghost[color] != 255) {
                color = ghost[256 + ghost[color] * 256 + expected[position]];
            }
            if (flags & FADING) {
                for (int iteration = 0; iteration < fade_count; ++iteration) color = fade[color];
            }
            expected[position] = color;
        }
    }
    std::vector<unsigned char> headers(sizeof(ShapeHeader) + static_cast<std::size_t>(std::max(height, 1)), 0);
    auto* header = reinterpret_cast<ShapeHeader*>(headers.data());
    header->flags = -1;
    header->data_offset = 0;
    header->theater = cache_mode == 3 ? 1 : 0;
    BigShapeBufferStart = pixels.data();
    TheaterShapeBufferStart = pixels.data();
    UseBigShapeBuffer = cache_mode == 0 ? 0 : 1;
    const void* shape = cache_mode == 0 ? static_cast<const void*>(pixels.data()) : headers.data();
    if (cache_mode == 2) {
        auto scratch = actual;
        Viewport scratch_viewport = viewport;
        scratch_viewport.pixels = scratch.data() + guard;
        const unsigned long error = draw(x, y, width, height, shape, &scratch_viewport, flags, ghost.data(), fade.data(), fade_count, predator_index);
        if (error != 0) {
            std::printf("FAIL header setup: exception 0x%08lX\n", error);
            return 1;
        }
    }
    const unsigned long error = draw(x, y, width, height, shape, &viewport, flags, ghost.data(), fade.data(), fade_count, predator_index);
    if (error != 0) {
        std::printf("FAIL width=%d height=%d x=%d y=%d flags=0x%X cache=%d: exception 0x%08lX\n",
                    width, height, x, y, flags, cache_mode, error);
        return 1;
    }
    if (actual != expected || pixels != original_pixels) {
        const auto mismatch = std::mismatch(actual.begin(), actual.end(), expected.begin());
        const int position = static_cast<int>(mismatch.first - actual.begin());
        std::printf("FAIL width=%d height=%d x=%d y=%d flags=0x%X cache=%d: pixel offset %d actual=%u expected=%u\n",
                    width, height, x, y, flags, cache_mode, position,
                    mismatch.first == actual.end() ? 0u : *mismatch.first,
                    mismatch.second == expected.end() ? 0u : *mismatch.second);
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    // The startup failure involves terrain shadow rendering with transparency.
    if (argc > 1) return run_case(std::atoi(argv[1]), 3, -3, 2, GHOST | TRANS, 1, 1, 0);
    int scenarios = 0;
    const int extra_widths[] = {127, 128, 129, 255, 256, 257};
    std::vector<int> widths;
    for (int width = 1; width <= 65; ++width) widths.push_back(width);
    widths.insert(widths.end(), extra_widths, extra_widths + 6);
    for (int mask = 0; mask < 16; ++mask) {
        const int flags = ((mask & 1) ? TRANS : 0) | ((mask & 2) ? GHOST : 0) |
                          ((mask & 4) ? FADING : 0) | ((mask & 8) ? PREDATOR : 0);
        for (int width : widths) {
            for (int geometry = 0; geometry < 5; ++geometry) {
                const int x = geometry == 1 ? -3 : geometry == 2 ? 358 : 8;
                const int y = geometry == 3 ? -1 : geometry == 4 ? 7 : 2;
                // Raw pixels and cached shapes clipped at each edge both enter
                // the unrolled routines whose jump calculation is under test.
                for (int cache = 0; cache < 2; ++cache) {
                    if (cache == 1 && (geometry == 0 || (geometry == 2 && width <= 2))) continue;
                    for (int fade_count = 1; fade_count <= 2; ++fade_count) {
                        if (!(flags & FADING) && fade_count == 2) continue;
                        if (run_case(width, 3, x, y, flags, cache, fade_count, 0)) return 1;
                        ++scenarios;
                    }
                }
            }
        }
    }
    // Also cover the ordinary first-draw/cached-draw path used for trees.
    for (int flags : {0, TRANS, GHOST, GHOST | TRANS}) {
        for (int width : widths) {
            for (int cache = 1; cache <= 3; ++cache) {
                if (run_case(width, 3, 8, 2, flags, cache, 1, 0)) return 1;
                ++scenarios;
            }
        }
    }
    // Zero-sized raw draws and completely off-screen sprites must do nothing.
    for (int flags : {0, TRANS, GHOST, GHOST | TRANS}) {
        for (int cache = 0; cache <= 1; ++cache) {
            // Cached shapes in the engine have positive dimensions; their
            // header scanner assumes at least one pixel and one row.
            if (cache == 0) {
                if (run_case(0, 3, 8, 2, flags, cache, 1, 0) ||
                    run_case(5, 0, 8, 2, flags, cache, 1, 0)) return 1;
                scenarios += 2;
            }
            if (run_case(5, 3, -10, 2, flags, cache, 1, 0) ||
                run_case(5, 3, 365, 2, flags, cache, 1, 0)) return 1;
            scenarios += 2;
        }
    }
    std::printf("PASS: %d actual assembly renderer scenarios (pixels, clipping, guards, source preservation).\n", scenarios);
    return 0;
}
