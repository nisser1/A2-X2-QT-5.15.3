/*
 * Qt TDE (vg_lite 2D engine) blit hook for sophon X2/A2 SoCs.
 *
 * SPDX-License-Identifier: LGPL-3.0-only OR GPL-2.0-or-later
 */

#include "qtdeblit_p.h"

#ifdef QT_TDE_BLIT

#include <qglobal.h>
#include <qstring.h>
#include <qlogging.h>
#include <qfile.h>
#include <qcoreapplication.h>

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <unistd.h>

// vg_lite.h is only available when QT_TDE_BLIT is defined (gui.pro gate)
#include <vg_lite.h>

QT_BEGIN_NAMESPACE

namespace {

inline bool tdeVerbose()
{
    static const int v = qEnvironmentVariableIntValue("QT_QPA_TDE_DEBUG");
    return v == 1;
}

inline bool tdeEnabled()
{
    static const int e = qEnvironmentVariableIntValue("QT_QPA_TDE");
    return e == 1;
}

// Flag values understood by vg_lite_map() on this platform (checked
// against libvg_lite.so disassembly): 0 = kernel-allocated contiguous
// memory, 1 = user memory, 2 = dmabuf fd.
constexpr int kMapUserMemory = 1;

} // namespace

QTdeBlit *QTdeBlit::instance()
{
    static QTdeBlit inst;
    return &inst;
}

QTdeBlit::QTdeBlit() = default;

QTdeBlit::~QTdeBlit()
{
    if (m_inited) {
        vg_lite_close();
        m_inited = false;
    }
}

bool QTdeBlit::ensureInit()
{
    if (m_inited)
        return true;
    if (m_initFailed)
        return false;

    vg_lite_error_t err = vg_lite_init(1920, 1080);
    if (err != VG_LITE_SUCCESS) {
        m_initFailed = true;
        if (tdeVerbose())
            qWarning("QTdeBlit: vg_lite_init failed err=%d", int(err));
        return false;
    }
    m_inited = true;

    if (tdeVerbose()) {
        vg_lite_info_t info;
        if (vg_lite_get_info(&info) == VG_LITE_SUCCESS)
            qWarning("QTdeBlit: vg_lite api 0x%x header 0x%x release 0x%x",
                     unsigned(info.api_version), unsigned(info.header_version),
                     unsigned(info.release_version));
        qWarning("QTdeBlit: 24BIT feature=%d",
                 int(vg_lite_query_feature(gcFEATURE_BIT_VG_24BIT)));
    }
    return true;
}

bool QTdeBlit::isAvailable()
{
    if (!tdeEnabled())
        return false;
    return instance()->ensureInit();
}

void QTdeBlit::setFramebuffer(const FbInfo &info)
{
    instance()->m_fb = info;
    if (tdeVerbose())
        qWarning("QTdeBlit: fb phys=0x%lx %dx%d stride=%d off=%d,%d rgb888=%d",
                 info.physBase, info.width, info.height, info.stride,
                 info.xOffset, info.yOffset, int(info.isRgb888));
}

bool QTdeBlit::hasFramebuffer()
{
    return instance()->m_fb.physBase != 0;
}

ulong QTdeBlit::probeFramebufferPhys()
{
    // /proc/iomem 在本平台以 "xxxxxxxx-xxxxxxxx : FRAME_BUFFER" 暴露 fb 保留区。
    QFile f(QStringLiteral("/proc/iomem"));
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    const QByteArray content = f.readAll();
    const int idx = content.indexOf("FRAME_BUFFER");
    if (idx <= 0)
        return 0;
    const int lineStart = content.lastIndexOf('\n', idx) + 1;
    const int dash = content.indexOf('-', lineStart);
    if (dash < 0 || dash >= idx)
        return 0;
    bool ok = false;
    const ulong base = content.mid(lineStart, dash - lineStart).trimmed().toULong(&ok, 16);
    return ok ? base : 0;
}

void QTdeBlit::addDirty(const QRect &screenRect)
{
    if (screenRect.isEmpty())
        return;
    QTdeBlit *self = instance();
    pthread_mutex_lock(&self->m_mutex);
    self->m_hwDirty += screenRect;
    pthread_mutex_unlock(&self->m_mutex);
}

QRegion QTdeBlit::takeDirty()
{
    QTdeBlit *self = instance();
    pthread_mutex_lock(&self->m_mutex);
    const QRegion dirty = self->m_hwDirty;
    self->m_hwDirty = QRegion();
    pthread_mutex_unlock(&self->m_mutex);
    return dirty;
}

bool QTdeBlit::blit(const uchar *srcBits, int srcStride, int srcW, int srcH,
                    const QRect &srcRect, QImage::Format srcFormat,
                    const QRect &dstRect, QString *whyNot)
{
    QTdeBlit *self = instance();

    if (whyNot) whyNot->clear();

    if (srcRect.isEmpty() || dstRect.isEmpty()) {
        if (whyNot) *whyNot = QStringLiteral("empty rect");
        return false;
    }

    if (srcFormat != QImage::Format_RGB888 || !self->m_fb.isRgb888) {
        if (whyNot) *whyNot = QStringLiteral("format");
        return false;
    }

    // Physical addresses must be 16-byte aligned for TDE stride/addr
    // constraints. The fb screen base satisfies this on this platform;
    // the src QImage memory is mapped by vg_lite_map() below which
    // requires 64-byte alignment of the user pointer (vendor sample
    // aligns to 64).
    if (self->m_fb.physBase == 0) {
        if (whyNot) *whyNot = QStringLiteral("no fb");
        return false;
    }

    const int bpp = 3;
    Q_UNUSED(bpp);
    // Qt RGB888 = red, green, blue byte order.
    const vg_lite_buffer_format_t vgFmt = VG_LITE_RGB888;

    // vg_lite_map(USER_MEMORY) 需页对齐起点；QImage bits() 不保证，
    // 行首偏移由调用方换算成 (srcBits - 余数) 不做，这里直接要求对齐。
    const int srcAlign = quintptr(srcBits) % 64;
    if (srcAlign != 0) {
        if (whyNot) *whyNot = QStringLiteral("src not 64B aligned");
        return false;
    }
    if (srcStride % 16 != 0) {
        if (whyNot) *whyNot = QStringLiteral("src stride not 16B aligned");
        return false;
    }

    if (!self->ensureInit()) {
        if (whyNot) *whyNot = QStringLiteral("tde init failed");
        return false;
    }

    vg_lite_buffer_t src = {};
    src.width = srcW;
    src.height = srcH;
    src.stride = srcStride;
    src.format = vgFmt;
    src.tiled = VG_LITE_LINEAR;
    src.image_mode = VG_LITE_NORMAL_IMAGE_MODE;
    src.transparency_mode = VG_LITE_IMAGE_OPAQUE;
    src.memory = const_cast<vg_lite_uint8_t *>(srcBits);
    // Pre-mapped CPU memory: vg_lite_map(VG_LITE_MAP_USER_MEMORY) fills
    // in the hardware (bus) address via the kernel driver.
    if (vg_lite_map(&src, vg_lite_map_flag_t(kMapUserMemory), -1) != VG_LITE_SUCCESS) {
        if (whyNot) *whyNot = QStringLiteral("vg_lite_map src failed");
        return false;
    }

    // Destination: wrap the framebuffer scanout memory directly.
    // dstRect 已是屏幕坐标（backing-store 空间 = fb 屏幕区）。
    const QRect screenDst = dstRect;
    if (screenDst.x() < 0 || screenDst.y() < 0
        || screenDst.right() >= self->m_fb.width
        || screenDst.bottom() >= self->m_fb.height) {
        if (whyNot) *whyNot = QStringLiteral("dst out of fb bounds");
        vg_lite_unmap(&src);
        return false;
    }

    vg_lite_buffer_t dst = {};
    dst.width = self->m_fb.width;
    dst.height = self->m_fb.height;
    dst.stride = self->m_fb.stride;
    dst.format = vgFmt;
    dst.tiled = VG_LITE_LINEAR;
    dst.image_mode = VG_LITE_NORMAL_IMAGE_MODE;
    dst.transparency_mode = VG_LITE_IMAGE_OPAQUE;
    dst.handle = nullptr;
    dst.memory = nullptr;      // pure device memory, no CPU mapping needed
    dst.address = vg_lite_uint32_t(self->m_fb.physBase);

    const float scaleX = float(dstRect.width()) / float(srcRect.width());
    const float scaleY = float(dstRect.height()) / float(srcRect.height());

    // blit_rect applies the matrix to the source rectangle: build a
    // matrix that maps srcRect onto dstRect (scale + translate), the
    // same convention as bmcv_tde_convert.
    const float dstCx = dstRect.x() + float(dstRect.width()) / 2.0f;
    const float dstCy = dstRect.y() + float(dstRect.height()) / 2.0f;
    const float srcCx = srcRect.x() + float(srcRect.width()) / 2.0f;
    const float srcCy = srcRect.y() + float(srcRect.height()) / 2.0f;

    bool ok = self->doBlit(&src, &dst,
                           vg_lite_rectangle_t{ srcRect.x(), srcRect.y(),
                                                srcRect.width(), srcRect.height() },
                           scaleX, scaleY, dstCx, dstCy, srcCx, srcCy, whyNot);

    vg_lite_unmap(&src);
    if (ok)
        addDirty(screenDst);
    return ok;
}

bool QTdeBlit::doBlit(vg_lite_buffer_t *src, vg_lite_buffer_t *dst,
                      const vg_lite_rectangle_t &srcRect,
                      float scaleX, float scaleY,
                      float dstCx, float dstCy, float srcCx, float srcCy,
                      QString *whyNot)
{
    pthread_mutex_lock(&m_mutex);

    vg_lite_matrix_t matrix;
    vg_lite_identity(&matrix);
    vg_lite_translate(dstCx, dstCy, &matrix);
    vg_lite_scale(scaleX, scaleY, &matrix);
    vg_lite_translate(-srcCx, -srcCy, &matrix);

    vg_lite_rectangle_t rect = srcRect;
    vg_lite_error_t err = vg_lite_blit_rect(dst, src, &rect, &matrix,
                                            VG_LITE_BLEND_NONE, 0, VG_LITE_FILTER_BI_LINEAR);
    if (err == VG_LITE_SUCCESS)
        err = vg_lite_finish();

    pthread_mutex_unlock(&m_mutex);

    if (err != VG_LITE_SUCCESS) {
        if (whyNot) *whyNot = QStringLiteral("vg_lite err %1").arg(int(err));
        return false;
    }
    return true;
}

QT_END_NAMESPACE

#endif // QT_TDE_BLIT
