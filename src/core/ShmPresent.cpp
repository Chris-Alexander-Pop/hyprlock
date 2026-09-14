#include "ShmPresent.hpp"
#include "hyprlock.hpp"
#include "../helpers/Log.hpp"
#include "../helpers/MiscFunctions.hpp"

#include <GLES3/gl32.h>
#include <sys/mman.h>
#include <unistd.h>

CShmPresent::~CShmPresent() {
    destroySlot(m_slots[0]);
    destroySlot(m_slots[1]);
}

void CShmPresent::destroySlot(Slot& slot) {
    slot.buffer.reset();
    slot.pool.reset();
    if (slot.map && slot.map != MAP_FAILED)
        munmap(slot.map, slot.bytes);
    slot.map = nullptr;
    if (slot.fd >= 0)
        close(slot.fd);
    slot.fd = -1;
    if (!slot.path.empty())
        unlink(slot.path.c_str());
    slot.path.clear();
    slot.bytes = 0;
    slot.busy  = false;
}

bool CShmPresent::ensure(int w, int h) {
    if (w <= 0 || h <= 0)
        return false;

    if (m_w == w && m_h == h && m_slots[0].buffer && m_slots[1].buffer)
        return true;

    destroySlot(m_slots[0]);
    destroySlot(m_slots[1]);

    auto shm = g_pHyprlock->getShm();
    if (!shm) {
        Log::logger->log(Log::ERR, "shm present: no wl_shm");
        return false;
    }

    m_w      = w;
    m_h      = h;
    m_stride = w * 4;
    m_readback.resize(static_cast<size_t>(m_stride) * static_cast<size_t>(h));

    for (int i = 0; i < 2; ++i) {
        auto& slot   = m_slots[i];
        slot.bytes   = static_cast<size_t>(m_stride) * static_cast<size_t>(h);
        slot.fd      = createPoolFile(slot.bytes, slot.path);
        if (slot.fd < 0)
            return false;

        slot.map = mmap(nullptr, slot.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, slot.fd, 0);
        if (slot.map == MAP_FAILED) {
            Log::logger->log(Log::ERR, "shm present: mmap failed");
            slot.map = nullptr;
            return false;
        }

        slot.pool   = makeShared<CCWlShmPool>(shm->sendCreatePool(slot.fd, slot.bytes));
        slot.buffer = makeShared<CCWlBuffer>(slot.pool->sendCreateBuffer(0, w, h, m_stride, WL_SHM_FORMAT_XRGB8888));
        slot.busy   = false;
        slot.buffer->setRelease([this, i](CCWlBuffer*) { m_slots[i].busy = false; });
    }

    Log::logger->log(Log::INFO, "shm present: {}x{} double-buffered", w, h);
    return true;
}

bool CShmPresent::publish(SP<CCWlSurface> surface, const CFramebuffer& fb) {
    if (!surface || !ensure(fb.m_vSize.x, fb.m_vSize.y))
        return false;

    Slot* slot = nullptr;
    for (int n = 0; n < 2; ++n) {
        Slot& cand = m_slots[m_idx];
        m_idx      = (m_idx + 1) % 2;
        if (!cand.busy) {
            slot = &cand;
            break;
        }
    }
    if (!slot)
        slot = &m_slots[m_idx];

    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb.m_iFb);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, m_w, m_h, GL_RGBA, GL_UNSIGNED_BYTE, m_readback.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    auto* srcBase = m_readback.data();
    auto* dstBase = static_cast<uint8_t*>(slot->map);
    for (int y = 0; y < m_h; ++y) {
        const uint8_t* src = srcBase + static_cast<size_t>(m_h - 1 - y) * static_cast<size_t>(m_w) * 4;
        uint8_t*       dst = dstBase + static_cast<size_t>(y) * static_cast<size_t>(m_stride);
        for (int x = 0; x < m_w; ++x) {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = 0xFF;
            src += 4;
            dst += 4;
        }
    }

    slot->busy = true;
    surface->sendAttach(slot->buffer.get(), 0, 0);
    surface->sendDamageBuffer(0, 0, m_w, m_h);
    surface->sendCommit();
    return true;
}
