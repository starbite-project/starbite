/* Stand-in for Wine's wine/wined3d.h: the tests only ask a loaded wined3d.dll (never present on Windows) which
 * renderer it uses. */
#ifndef SB_WINE_WINED3D_H
#define SB_WINE_WINED3D_H
enum wined3d_renderer
{
    WINED3D_RENDERER_AUTO,
    WINED3D_RENDERER_VULKAN,
    WINED3D_RENDERER_OPENGL,
    WINED3D_RENDERER_NO3D,
};
#endif
