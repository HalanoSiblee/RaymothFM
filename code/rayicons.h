#ifndef WIN7RAYEXPLORER_RAYICONS_H
#define WIN7RAYEXPLORER_RAYICONS_H

// -----------------------------------------------------------------------------
// Small, header-only rIcons renderer.
//
// The bitmap data below is taken from the rIcons pack used by raylib/raygui:
// 16x16 monochrome icons stored as eight uint32_t words per icon.
// This project only needs a small Explorer-oriented subset, so we keep the
// header tiny instead of carrying the complete 256-icon pack.
// -----------------------------------------------------------------------------

#include <raylib.h>
#include <cstdint>

namespace rayicons {

enum Icon : int {
    None = 0,
    FolderFileOpen = 1,
    FolderOpen = 3,
    FileOpen = 5,
    FileNew = 8,
    FileDelete = 9,
    FileText = 10,
    FileAudio = 11,
    FileImage = 12,
    FileVideo = 14,
    FileCopy = 16,
    FileCut = 17,
    FilePaste = 18,
    ArrowLeft = 126,   // rIcons: RICON_ARROW_LEFT
    ArrowRight = 127,  // rIcons: RICON_ARROW_RIGHT
    ArrowDown = 128,   // rIcons: RICON_ARROW_BOTTOM
    ArrowUp = 129,     // rIcons: RICON_ARROW_TOP
    House = 185
};

// Data for the selected rIcons entries. Each icon is 16x16 and occupies
// 8 uint32_t values (two 16-bit rows packed per uint32_t).
static constexpr uint32_t kFolderFileOpen[8] = {
    0x3ff80000u, 0x2f082008u, 0x2042207eu, 0x40027fcu, 0x40024002u, 0x40024002u, 0x40024002u, 0x00007ffeu
};
static constexpr uint32_t kFolderOpen[8] = {
    0x00000000u, 0x0042007eu, 0x40027fcu, 0x40024002u, 0x41024002u, 0x44424282u, 0x793e4102u, 0x00000100u
};
static constexpr uint32_t kFileOpen[8] = {
    0x3ff00000u, 0x201c2010u, 0x20042004u, 0x21042004u, 0x24442284u, 0x21042104u, 0x20042104u, 0x00003ffcu
};
static constexpr uint32_t kFileNew[8] = {
    0x3ff00000u, 0x201c2010u, 0x20042004u, 0x20042004u, 0x22042204u, 0x22042f84u, 0x20042204u, 0x00003ffcu
};
static constexpr uint32_t kFileText[8] = {
    0x3ff00000u, 0x201c2010u, 0x20042004u, 0x20042ff4u, 0x20042ff4u, 0x20042ff4u, 0x20042004u, 0x00003ffcu
};
static constexpr uint32_t kFileAudio[8] = {
    0x3ff00000u, 0x201c2010u, 0x27042004u, 0x244424c4u, 0x26442444u, 0x20642664u, 0x20042004u, 0x00003ffcu
};
static constexpr uint32_t kFileImage[8] = {
    0x3ff00000u, 0x201c2010u, 0x26042604u, 0x20042004u, 0x35442884u, 0x2414222cu, 0x20042004u, 0x00003ffcu
};
static constexpr uint32_t kFileVideo[8] = {
    0x3ff00000u, 0x3ffc2ff0u, 0x3f3c2ff4u, 0x3dbc2eb4u, 0x3dbc2bb4u, 0x3f3c2eb4u, 0x3ffc2ff4u, 0x00002ff4u
};
static constexpr uint32_t kFileCopy[8] = {
    0x0ff00000u, 0x381c0810u, 0x28042804u, 0x28042804u, 0x28042804u, 0x28042804u, 0x20102ffcu, 0x00003ff0u
};
static constexpr uint32_t kFilePaste[8] = {
    0x01c00000u, 0x13e41becu, 0x3f841004u, 0x204420c4u, 0x20442044u, 0x20442044u, 0x207c2044u, 0x00003fc0u
};
static constexpr uint32_t kArrowLeft[8] = {
    0x00000000u, 0x02000000u, 0x00800100u, 0x00200040u, 0x00200010u, 0x00800040u, 0x02000100u, 0x00000000u
};
static constexpr uint32_t kArrowRight[8] = {
    0x00000000u, 0x00400000u, 0x01000080u, 0x04000200u, 0x04000800u, 0x01000200u, 0x00400080u, 0x00000000u
};
static constexpr uint32_t kArrowDown[8] = {
    0x00000000u, 0x00000000u, 0x00000000u, 0x08081004u, 0x02200410u, 0x00800140u, 0x00000000u, 0x00000000u
};
static constexpr uint32_t kArrowUp[8] = {
    0x00000000u, 0x00000000u, 0x01400080u, 0x04100220u, 0x10040808u, 0x00000000u, 0x00000000u, 0x00000000u
};
static constexpr uint32_t kHouse[8] = {
    0x00000000u, 0x00400000u, 0x0c601010u, 0x07c00fe0u, 0x07c007c0u, 0x0c600fe0u, 0x20081010u, 0x00000000u
};

inline const uint32_t* data(Icon icon) {
    switch (icon) {
        case FolderFileOpen: return kFolderFileOpen;
        case FolderOpen:     return kFolderOpen;
        case FileOpen:       return kFileOpen;
        case FileNew:        return kFileNew;
        case FileText:       return kFileText;
        case FileAudio:      return kFileAudio;
        case FileImage:      return kFileImage;
        case FileVideo:      return kFileVideo;
        case FileCopy:       return kFileCopy;
        case FilePaste:      return kFilePaste;
        case ArrowLeft:      return kArrowLeft;
        case ArrowRight:     return kArrowRight;
        case ArrowDown:      return kArrowDown;
        case ArrowUp:        return kArrowUp;
        case House:          return kHouse;
        default:             return nullptr;
    }
}

inline bool pixel(const uint32_t* d, int x, int y) {
    if (!d || x < 0 || y < 0 || x >= 16 || y >= 16) return false;
    const int word = y / 2;
    const int bit = x + ((y & 1) ? 16 : 0);
    return (d[word] & (1u << bit)) != 0;
}

inline void Draw(Icon icon, int x, int y, int pixelSize, Color color) {
    if (pixelSize < 1) pixelSize = 1;
    const uint32_t* d = data(icon);
    if (!d) return;
    for (int py = 0; py < 16; ++py) {
        for (int px = 0; px < 16; ++px) {
            if (pixel(d, px, py)) {
                DrawRectangle(x + px * pixelSize, y + py * pixelSize,
                              pixelSize, pixelSize, color);
            }
        }
    }
}

inline int PixelSizeFor(int targetSize) {
    if (targetSize <= 16) return 1;
    return targetSize / 16;
}

} // namespace rayicons

#endif
