/*
 * Copyright (c) 2013 Stefano Pigozzi <stefano.pigozzi@gmail.com>
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>

#include <IOSurface/IOSurface.h>
#include <CoreVideo/CoreVideo.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/CGLIOSurface.h>

#include "video/mp_image_pool.h"
#include "hwdec.h"
#include "common/global.h"
#include "options/options.h"

struct vt_gl_plane_format {
    GLenum gl_format;
    GLenum gl_type;
    GLenum gl_internal_format;
    char swizzle[5];
};

struct vt_format {
    uint32_t cvpixfmt;
    int imgfmt;
    int planes;
    struct vt_gl_plane_format gl[MP_MAX_PLANES];
};

struct priv {
    struct mp_hwdec_ctx hwctx;
    struct mp_vt_ctx vtctx;

    CVPixelBufferRef pbuf;
    GLuint gl_planes[MP_MAX_PLANES];
};

static struct vt_format vt_formats[] = {
    {
        .cvpixfmt = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
        .imgfmt = IMGFMT_NV12,
        .planes = 2,
        .gl = {
            { GL_RED, GL_UNSIGNED_BYTE, GL_RED },
            { GL_RG,  GL_UNSIGNED_BYTE, GL_RG } ,
        }
    },
    {
        .cvpixfmt = kCVPixelFormatType_422YpCbCr8,
        .imgfmt = IMGFMT_UYVY,
        .planes = 1,
        .gl = {
            { GL_RGB_422_APPLE, GL_UNSIGNED_SHORT_8_8_APPLE, GL_RGB, "gbra" }
        }
    },
    {
        .cvpixfmt = kCVPixelFormatType_420YpCbCr8Planar,
        .imgfmt = IMGFMT_420P,
        .planes = 3,
        .gl = {
            { GL_RED, GL_UNSIGNED_BYTE, GL_RED },
            { GL_RED, GL_UNSIGNED_BYTE, GL_RED },
            { GL_RED, GL_UNSIGNED_BYTE, GL_RED },
        }
    },
    {
        .cvpixfmt = kCVPixelFormatType_32BGRA,
        .imgfmt = IMGFMT_RGB0,
        .planes = 1,
        .gl = {
            { GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, GL_RGBA }
        }
    },
};

static struct vt_format *vt_get_gl_format(uint32_t cvpixfmt)
{
    for (int i = 0; i < MP_ARRAY_SIZE(vt_formats); i++) {
        if (vt_formats[i].cvpixfmt == cvpixfmt)
            return &vt_formats[i];
    }
    return NULL;
}

static struct vt_format *vt_get_gl_format_from_imgfmt(uint32_t imgfmt)
{
    for (int i = 0; i < MP_ARRAY_SIZE(vt_formats); i++) {
        if (vt_formats[i].imgfmt == imgfmt)
            return &vt_formats[i];
    }
    return NULL;
}

static struct mp_image *download_image(struct mp_hwdec_ctx *ctx,
                                       struct mp_image *hw_image,
                                       struct mp_image_pool *swpool)
{
    if (hw_image->imgfmt != IMGFMT_VIDEOTOOLBOX)
        return NULL;

    struct mp_image *image = NULL;
    CVPixelBufferRef pbuf = (CVPixelBufferRef)hw_image->planes[3];
    CVPixelBufferLockBaseAddress(pbuf, kCVPixelBufferLock_ReadOnly);
    size_t width  = CVPixelBufferGetWidth(pbuf);
    size_t height = CVPixelBufferGetHeight(pbuf);
    uint32_t cvpixfmt = CVPixelBufferGetPixelFormatType(pbuf);
    struct vt_format *f = vt_get_gl_format(cvpixfmt);
    if (!f)
        goto unlock;

    struct mp_image img = {0};
    mp_image_setfmt(&img, f->imgfmt);
    mp_image_set_size(&img, width, height);

    for (int i = 0; i < f->planes; i++) {
        void *base    = CVPixelBufferGetBaseAddressOfPlane(pbuf, i);
        size_t stride = CVPixelBufferGetBytesPerRowOfPlane(pbuf, i);
        img.planes[i] = base;
        img.stride[i] = stride;
    }

    mp_image_copy_attributes(&img, hw_image);

    image = mp_image_pool_new_copy(swpool, &img);

unlock:
    CVPixelBufferUnlockBaseAddress(pbuf, kCVPixelBufferLock_ReadOnly);

    return image;
}

static bool check_hwdec(struct gl_hwdec *hw)
{
    if (hw->gl->version < 300) {
        MP_ERR(hw, "need >= OpenGL 3.0 for core rectangle texture support\n");
        return false;
    }

    if (!CGLGetCurrentContext()) {
        MP_ERR(hw, "need cocoa opengl backend to be active");
        return false;
    }

    return true;
}

static uint32_t get_vt_fmt(struct mp_vt_ctx *vtctx)
{
    struct gl_hwdec *hw = vtctx->priv;
    struct vt_format *f =
        vt_get_gl_format_from_imgfmt(hw->global->opts->videotoolbox_format);
    return f ? f->cvpixfmt : (uint32_t)-1;
}

static int create(struct gl_hwdec *hw)
{
    if (!check_hwdec(hw))
        return -1;

    struct priv *p = talloc_zero(hw, struct priv);
    hw->priv = p;

    hw->gl->GenTextures(MP_MAX_PLANES, p->gl_planes);

    p->vtctx = (struct mp_vt_ctx){
        .priv = hw,
        .get_vt_fmt = get_vt_fmt,
    };
    p->hwctx = (struct mp_hwdec_ctx){
        .type = HWDEC_VIDEOTOOLBOX,
        .download_image = download_image,
        .ctx = &p->vtctx,
    };
    hwdec_devices_add(hw->devs, &p->hwctx);

    return 0;
}

static int reinit(struct gl_hwdec *hw, struct mp_image_params *params)
{
    assert(params->imgfmt == hw->driver->imgfmt);

    struct vt_format *f = vt_get_gl_format_from_imgfmt(params->hw_subfmt);
    if (!f) {
        MP_ERR(hw, "Unsupported CVPixelBuffer format.\n");
        return -1;
    }

    params->imgfmt = f->imgfmt;
    params->hw_subfmt = 0;
    return 0;
}

static int map_frame(struct gl_hwdec *hw, struct mp_image *hw_image,
                     struct gl_hwdec_frame *out_frame)
{
    struct priv *p = hw->priv;
    GL *gl = hw->gl;

    CVPixelBufferRelease(p->pbuf);
    p->pbuf = (CVPixelBufferRef)hw_image->planes[3];
    CVPixelBufferRetain(p->pbuf);
    IOSurfaceRef surface = CVPixelBufferGetIOSurface(p->pbuf);
    if (!surface) {
        MP_ERR(hw, "CVPixelBuffer has no IOSurface\n");
        return -1;
    }

    uint32_t cvpixfmt = CVPixelBufferGetPixelFormatType(p->pbuf);
    struct vt_format *f = vt_get_gl_format(cvpixfmt);
    if (!f) {
        MP_ERR(hw, "CVPixelBuffer has unsupported format type\n");
        return -1;
    }

    const bool planar = CVPixelBufferIsPlanar(p->pbuf);
    const int planes  = CVPixelBufferGetPlaneCount(p->pbuf);
    assert(planar && planes == f->planes || f->planes == 1);

    GLenum gl_target = GL_TEXTURE_RECTANGLE;

    for (int i = 0; i < f->planes; i++) {
        gl->BindTexture(gl_target, p->gl_planes[i]);

        CGLError err = CGLTexImageIOSurface2D(
            CGLGetCurrentContext(), gl_target,
            f->gl[i].gl_internal_format,
            IOSurfaceGetWidthOfPlane(surface, i),
            IOSurfaceGetHeightOfPlane(surface, i),
            f->gl[i].gl_format, f->gl[i].gl_type, surface, i);

        if (err != kCGLNoError)
            MP_ERR(hw, "error creating IOSurface texture for plane %d: %s (%x)\n",
                   i, CGLErrorString(err), gl->GetError());

        gl->BindTexture(gl_target, 0);

        out_frame->planes[i] = (struct gl_hwdec_plane){
            .gl_texture = p->gl_planes[i],
            .gl_target = gl_target,
            .tex_w = IOSurfaceGetWidthOfPlane(surface, i),
            .tex_h = IOSurfaceGetHeightOfPlane(surface, i),
        };
        snprintf(out_frame->planes[i].swizzle, sizeof(out_frame->planes[i].swizzle),
                 "%s", f->gl[i].swizzle);
    }

    return 0;
}

static void destroy(struct gl_hwdec *hw)
{
    struct priv *p = hw->priv;
    GL *gl = hw->gl;

    CVPixelBufferRelease(p->pbuf);
    gl->DeleteTextures(MP_MAX_PLANES, p->gl_planes);

    hwdec_devices_remove(hw->devs, &p->hwctx);
}

const struct gl_hwdec_driver gl_hwdec_videotoolbox = {
    .name = "videotoolbox",
    .api = HWDEC_VIDEOTOOLBOX,
    .imgfmt = IMGFMT_VIDEOTOOLBOX,
    .create = create,
    .reinit = reinit,
    .map_frame = map_frame,
    .destroy = destroy,
};
