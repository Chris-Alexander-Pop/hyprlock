#pragma once

#include "../defines.hpp"
#include "wayland.hpp"
#include "../renderer/Framebuffer.hpp"
#include <string>
#include <vector>

class CShmPresent {
  public:
    ~CShmPresent();

    bool ensure(int w, int h);
    bool publish(SP<CCWlSurface> surface, const CFramebuffer& fb);

  private:
    struct Slot {
        int             fd    = -1;
        void*           map   = nullptr;
        size_t          bytes = 0;
        std::string     path;
        SP<CCWlShmPool> pool;
        SP<CCWlBuffer>  buffer;
        bool            busy = false;
    };

    void destroySlot(Slot& slot);

    Slot                 m_slots[2];
    int                  m_idx    = 0;
    int                  m_w      = 0;
    int                  m_h      = 0;
    int                  m_stride = 0;
    std::vector<uint8_t> m_readback;
};
