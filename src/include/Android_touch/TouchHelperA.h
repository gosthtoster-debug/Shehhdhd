#pragma once

#include <linux/input.h>
#include <vector>
#include <functional>
#include "VectorStruct.h"

namespace Touch {
    struct touchObj {
        Vector2 pos{};
        int id = 0;
        bool isDown = false;
    };

    struct Device {
        int fd;
        float S2TX;
        float S2TY;
        input_absinfo absX, absY;
        touchObj Finger[10];

        Device() { memset((void *) this, 0, sizeof(*this)); }
    };

    bool Init(const Vector2 &s, bool p_readOnly);

    void Close();

    void Down(float x, float y);

    void Move(float x, float y);

    void Up();

    void Move(touchObj *touch, float x, float y);

    void Upload();

    void SetCallBack(const std::function<void(std::vector<Device> *)> &cb);

    Vector2 Touch2Screen(const Vector2 &coord);

    Vector2 GetScale();

    void setOrientation(int orientation);

    void setOtherTouch(bool p_otherTouch);
}

// ── shim: API that cheat.cpp expects ──
#include "ANativeWindowCreator.h"
inline bool Touch_Init(int w, int h, int /*orient*/, bool readOnly) {
    Touch::setOrientation((int)anwc::ANativeWindowCreator::GetDisplayInfo().orientation);
    return Touch::Init(Vector2((float)w, (float)h), readOnly);
}
inline void Touch_UpdateImGui() {    // reader thread already writes io.MousePos/MouseDown; here: keep rotation fresh
    static int n = 0;
    if (++n % 30 == 0) Touch::setOrientation((int)anwc::ANativeWindowCreator::GetDisplayInfo().orientation);
}
inline void Touch_Close() { Touch::Close(); }
