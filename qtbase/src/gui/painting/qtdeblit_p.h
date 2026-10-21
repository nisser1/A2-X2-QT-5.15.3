/*
 * Qt TDE (vg_lite 2D engine) blit hook for sophon X2/A2 SoCs.
 *
 * Intercepts QRasterPaintEngine::drawImage scale/blit fast-paths and
 * executes them on the TDE hardware (GCNanoLiteV via libvg_lite),
 * writing directly into the linuxfb framebuffer.
 *
 * Enable with QT_QPA_TDE=1, verbose logging with QT_QPA_TDE_DEBUG=1.
 *
 * SPDX-License-Identifier: LGPL-3.0-only OR GPL-2.0-or-later
 */

#ifndef QTDEBLIT_P_H
#define QTDEBLIT_P_H

//
//  W A R N I N G
//  -------------
//
// This file is not part of the Qt API.  It exists for the sophgo TDE
// hardware blit patch and may change or disappear without notice.
//

#include <QtGui/private/qtguiglobal_p.h>
#include <QtGui/qimage.h>
#include <QtGui/qregion.h>

#ifdef QT_TDE_BLIT

#include <pthread.h>

#include <vg_lite.h>

QT_BEGIN_NAMESPACE

class QString;

// 必须默认可见：linuxfb 平台插件（独立 .so）要调 setFramebuffer。
class Q_GUI_EXPORT QTdeBlit
{
public:
    // Registered by the linuxfb plugin (P3) so the blit hook knows the
    // physical layout of the framebuffer it may write to directly.
    struct FbInfo {
        ulong physBase = 0;   // fb physical (bus) address, 0 = unknown
        int width = 0;        // fb pixel width
        int height = 0;       // fb pixel height
        int stride = 0;       // fb line length in bytes
        int xOffset = 0;      // screen x offset inside fb
        int yOffset = 0;      // screen y offset inside fb
        bool isRgb888 = false;
    };

    static bool isAvailable();
    static void setFramebuffer(const FbInfo &info);
    static bool hasFramebuffer();

    // dst is expressed in screen coordinates (same space as the
    // rasterBuffer the paint engine would have written to).
    // srcBits/srcStride describe the source image memory; the caller
    // guarantees the memory stays valid for the duration of the call.
    // Returns true if the hardware blit was issued; on false the
    // caller must fall back to the raster path.
    static bool blit(const uchar *srcBits, int srcStride, int srcW, int srcH,
                     const QRect &srcRect, QImage::Format srcFormat,
                     const QRect &dstRect, QString *whyNot = nullptr);

    // fb 物理地址查询（P3 由 linuxfb 注册；也可从 /proc/iomem 探测）
    static ulong probeFramebufferPhys();

    // P1 直写 fb 后登记区域：QLinuxFbScreen::doRedraw 用它把已由 TDE
    // 写到 scanout 的区域从本轮软件合成里剔除，避免 raster 覆盖硬件结果。
    // 到屏幕的窗口合成（QFbScreen::doRedraw）发生前调用。
    static void addDirty(const QRect &screenRect);
    static QRegion takeDirty();

private:
    QTdeBlit();
    ~QTdeBlit();
    static QTdeBlit *instance();
    bool ensureInit();
    bool doBlit(vg_lite_buffer_t *src, vg_lite_buffer_t *dst,
                const vg_lite_rectangle_t &srcRect,
                float scaleX, float scaleY,
                float dstCx, float dstCy, float srcCx, float srcCy,
                QString *whyNot);

    pthread_mutex_t m_mutex = PTHREAD_MUTEX_INITIALIZER;
    bool m_inited = false;
    bool m_initFailed = false;
    FbInfo m_fb;
    QRegion m_hwDirty;   // TDE 已直写 fb、尚未被 doRedraw 消费的屏幕区域
};

QT_END_NAMESPACE

#endif // QT_TDE_BLIT
#endif // QTDEBLIT_P_H
