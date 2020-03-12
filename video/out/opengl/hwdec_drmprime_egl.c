/*
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
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>

#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>

#include <EGL/egl.h>

#include "common.h"
#include "video/hwdec.h"
#include "common/msg.h"
#include "options/m_config.h"
#include "libmpv/render_gl.h"
#include "video/out/drm_common.h"
#include "video/out/drm_prime.h"
#include "video/out/gpu/hwdec.h"
#include "video/mp_image.h"
#include "video/out/opengl/ra_gl.h"



typedef void* GLeglImageOES;
typedef void *EGLImageKHR;

// Any EGL_EXT_image_dma_buf_import definitions used in this source file.
#define EGL_LINUX_DMA_BUF_EXT             0x3270
#define EGL_LINUX_DRM_FOURCC_EXT          0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT         0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT     0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT      0x3274
#define EGL_DMA_BUF_PLANE1_FD_EXT         0x3275
#define EGL_DMA_BUF_PLANE1_OFFSET_EXT     0x3276
#define EGL_DMA_BUF_PLANE1_PITCH_EXT      0x3277
#define EGL_DMA_BUF_PLANE2_FD_EXT         0x3278
#define EGL_DMA_BUF_PLANE2_OFFSET_EXT     0x3279
#define EGL_DMA_BUF_PLANE2_PITCH_EXT      0x327A

// Any EGL_EXT_image_dma_buf_import definitions used in this source file.
#define EGL_DMA_BUF_PLANE3_FD_EXT         0x3440
#define EGL_DMA_BUF_PLANE3_OFFSET_EXT     0x3441
#define EGL_DMA_BUF_PLANE3_PITCH_EXT      0x3442

struct priv {
    struct mp_log *log;
    int num_planes;
    int display_w, display_h;
    GLuint gl_textures[4];
    EGLImageKHR images[4];

    EGLImageKHR (EGLAPIENTRY *CreateImageKHR)(EGLDisplay, EGLContext,
                                              EGLenum, EGLClientBuffer,
                                              const EGLint *);
    EGLBoolean (EGLAPIENTRY *DestroyImageKHR)(EGLDisplay, EGLImageKHR);
    void (EGLAPIENTRY *EGLImageTargetTexture2DOES)(GLenum, GLeglImageOES);
    AVDRMFrameDescriptor *desc;
    struct mp_image layout;
    struct ra_tex *tex[4];
    struct mp_hwdec_ctx hwctx;
};

static void scale_dst_rect(struct ra_hwdec *hw, int source_w, int source_h ,struct mp_rect *src, struct mp_rect *dst)
{
    struct priv *p = hw->priv;

    // drm can allow to have a layer that has a different size from framebuffer
    // we scale here the destination size to video mode
    double hratio = p->display_w / (double)source_w;
    double vratio = p->display_h / (double)source_h;
    double ratio = hratio <= vratio ? hratio : vratio;

    dst->x0 = src->x0 * ratio;
    dst->x1 = src->x1 * ratio;
    dst->y0 = src->y0 * ratio;
    dst->y1 = src->y1 * ratio;

    int offset_x = (p->display_w - ratio * source_w) / 2;
    int offset_y = (p->display_h - ratio * source_h) / 2;

    dst->x0 += offset_x;
    dst->x1 += offset_x;
    dst->y0 += offset_y;
    dst->y1 += offset_y;
}

static void uninit(struct ra_hwdec *hw)
{
//    struct priv *p = hw->priv;
}

static int init(struct ra_hwdec *hw)
{
    struct priv *p = hw->priv;
    p->log = hw->log;
    mp_err(p->log, "in init\n");
    
    if (!ra_is_gl(hw->ra)) {
        // This is not an OpenGL RA.
        mp_err(p->log, "not a open gl ra\n");
        return 1;
    }

    if (!eglGetCurrentContext()) {
        mp_err(p->log, "cant get current context\n");
        return 1;
    }

    const char *exts = eglQueryString(eglGetCurrentDisplay(), EGL_EXTENSIONS);
    if (!exts) {
        mp_err(p->log, "cannot get extensions\n");
        return 1;
    }
    mp_err(p->log, "Extensions: \n%s\n", exts);

    GL *gl = ra_gl_get(hw->ra);
    if (!gl_check_extension(exts, "EGL_EXT_image_dma_buf_import") ||
        !gl_check_extension(exts, "EGL_KHR_image_base") ||
        !gl_check_extension(gl->extensions, "GL_OES_EGL_image") ||
        !(gl->mpgl_caps & MPGL_CAP_TEX_RG)) {
        mp_err(p->log, "extension check not succesfull");
        return 1;
     }

     p->hwctx = (struct mp_hwdec_ctx) {
        .driver_name = hw->driver->name,
    };
    if (!av_hwdevice_ctx_create(&p->hwctx.av_device_ref, AV_HWDEVICE_TYPE_DRM,
                                "/dev/dri/card0", NULL, 0)) {
        hwdec_devices_add(hw->devs, &p->hwctx);
    } else {
        mp_err(p->log, "not able to create dev at render node");
    }
    return 0;
}

static int drmprime_gl_mapper_init(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->owner->priv;
    mp_err(p->log, "staring to init mapper\n");
    mapper->dst_params = mapper->src_params;
//    mapper->dst_params.imgfmt = mapper->src_params.imgfmt;
    mapper->dst_params.imgfmt = IMGFMT_NV12;
    mp_err(p->log, "imgfmt: %d\n", mapper->src_params.imgfmt);
    mp_err(p->log, "imgfmt: %d\n", IMGFMT_DRMPRIME);
    mapper->dst_params.hw_subfmt = 0;
    struct ra_imgfmt_desc desc = {0};
    mp_err(p->log, "here 1\n");
    if (!ra_get_imgfmt_desc(mapper->ra, mapper->dst_params.imgfmt, &desc)) {
        mp_err(p->log, "could not get imgfmt\n");
        return -1;
    }

    p->num_planes = desc.num_planes;
    mp_image_set_params(&p->layout, &mapper->dst_params);

    // EGL_KHR_image_base
    p->CreateImageKHR = (void *)eglGetProcAddress("eglCreateImageKHR");
    p->DestroyImageKHR = (void *)eglGetProcAddress("eglDestroyImageKHR");
    // GL_OES_EGL_image
    mp_err(p->log, "here 2\n");
    p->EGLImageTargetTexture2DOES =
        (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    if (!p->CreateImageKHR || !p->DestroyImageKHR ||
        !p->EGLImageTargetTexture2DOES)
        return 1;

    GL *gl = ra_gl_get(mapper->ra);
    gl->GenTextures(4, p->gl_textures);
    for (int n = 0; n < desc.num_planes; n++) {
        gl->BindTexture(GL_TEXTURE_2D, p->gl_textures[n]);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl->BindTexture(GL_TEXTURE_2D, 0);

        struct ra_tex_params params = {
            .dimensions = 2,
            .w = mp_image_plane_w(&p->layout, n),
            .h = mp_image_plane_h(&p->layout, n),
            .d = 1,
            .format = desc.planes[n],
            .render_src = true,
            .src_linear = true,
        };

        if (params.format->ctype != RA_CTYPE_UNORM)
            return 1;

        p->tex[n] = ra_create_wrapped_tex(mapper->ra, &params,
                                                 p->gl_textures[n]);
        if (!p->tex[n])
            return 1;
    }
    mp_err(p->log, "here 3\n");
    return 0;
}

static void drmprime_gl_mapper_uninit(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;

    if (p) {
        GL *gl = ra_gl_get(mapper->ra);
        gl->DeleteTextures(4, p->gl_textures);
        for (int n = 0; n < 4; n++) {
            p->gl_textures[n] = 0;
            ra_tex_free(mapper->ra, &p->tex[n]);
        }
        talloc_free(p);
    }
}

#define ADD_ATTRIB(name, value)                         \
    do {                                                \
    assert(num_attribs + 3 < MP_ARRAY_SIZE(attribs));   \
    attribs[num_attribs++] = (name);                    \
    attribs[num_attribs++] = (value);                   \
    attribs[num_attribs] = EGL_NONE;                    \
    } while(0)

#define ADD_PLANE_ATTRIBS(plane) do { \
            ADD_ATTRIB(EGL_DMA_BUF_PLANE ## plane ## _FD_EXT, \
                        p_mapper->desc->objects[p_mapper->desc->layers[n].planes[plane].object_index].fd); \
            ADD_ATTRIB(EGL_DMA_BUF_PLANE ## plane ## _OFFSET_EXT, \
                        p_mapper->desc->layers[n].planes[plane].offset); \
            ADD_ATTRIB(EGL_DMA_BUF_PLANE ## plane ## _PITCH_EXT, \
                        p_mapper->desc->layers[n].planes[plane].pitch); \
        } while (0)

static int drmprime_gl_mapper_map(struct ra_hwdec_mapper *mapper)
{
    struct priv *p_mapper = mapper->priv;
    p_mapper->desc = (AVDRMFrameDescriptor *)mapper->src->planes[0];

    GL *gl = ra_gl_get(mapper->ra);

    for (int n = 0; n < p_mapper->num_planes; n++) {
        int attribs[20] = {EGL_NONE};
        int num_attribs = 0;

        ADD_ATTRIB(EGL_LINUX_DRM_FOURCC_EXT, p_mapper->desc->layers[n].format);
        ADD_ATTRIB(EGL_WIDTH,  p_mapper->tex[n]->params.w);
        ADD_ATTRIB(EGL_HEIGHT, p_mapper->tex[n]->params.h);

        ADD_PLANE_ATTRIBS(0);
        if (p_mapper->desc->layers[n].nb_planes > 1)
            ADD_PLANE_ATTRIBS(1);
        if (p_mapper->desc->layers[n].nb_planes > 2)
            ADD_PLANE_ATTRIBS(2);
        if (p_mapper->desc->layers[n].nb_planes > 3)
            ADD_PLANE_ATTRIBS(3);

        p_mapper->images[n] = p_mapper->CreateImageKHR(eglGetCurrentDisplay(),
            EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
        if (!p_mapper->images[n])
            return 1;

        gl->BindTexture(GL_TEXTURE_2D, p_mapper->gl_textures[n]);
        p_mapper->EGLImageTargetTexture2DOES(GL_TEXTURE_2D, p_mapper->images[n]);

        mapper->tex[n] = p_mapper->tex[n];
    }
    gl->BindTexture(GL_TEXTURE_2D, 0);
    return 0;
}

static void drmprime_gl_mapper_unmap(struct ra_hwdec_mapper *mapper)
{
    struct priv *p_mapper = mapper->priv;

    if (p_mapper) {
        for (int n = 0; n < 4; n++) {
            if (p_mapper->images[n])
                p_mapper->DestroyImageKHR(eglGetCurrentDisplay(), p_mapper->images[n]);
            p_mapper->images[n] = 0;
        }
    }
}

const struct ra_hwdec_driver ra_hwdec_drmprime_egl = {
    .name = "drmprime-egl",
    .priv_size = sizeof(struct priv),
    .imgfmts = {IMGFMT_DRMPRIME, 0},
    .init = init,
//    .overlay_frame = overlay_frame,
    .uninit = uninit,
    .mapper = &(const struct ra_hwdec_mapper_driver){
        .priv_size = sizeof(struct priv),
        .init = drmprime_gl_mapper_init,
        .uninit = drmprime_gl_mapper_uninit,
        .map = drmprime_gl_mapper_map,
        .unmap = drmprime_gl_mapper_unmap,
    },
    
};
