/*
 * gdiplus の画像の描画 (GdipDrawImagePointsRect) の速さと結果を測る。
 *
 * Win-kurarin (.NET の PictureBox、StretchImage) と同じ、500x720 の 32bppARGB を
 * HDC (32bpp の DIB) に 405x577 で描く場合を主に、補間・変換・折り返し・描き先を変えた場合を並べる。
 * 各場合の描き先の画素の FNV-1a と、1 回の時間 (QPC、最小・中央) を出す。
 *
 *   gdipbench.exe [回数] [出力のファイル] [dump のフォルダー]
 *
 * 出力は既定で C:\gdipbench.txt。dump のフォルダーを付けると、各場合の描き先の画素を <名前>.raw に書く。
 * 環境変数 GDIPBENCH_DLL で読む gdiplus の DLL を変えられる (既定 gdiplus.dll。直す前と後を同じ Wine で比べるため)。
 * 作り方: i686-w64-mingw32-gcc -O2 -o gdipbench.exe tests/gdipbench.c -lgdi32
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef float REAL;
typedef int GpStatus;
typedef void GpBitmap, GpImage, GpGraphics, GpImageAttributes, GpMatrix;
typedef struct { REAL X, Y; } GpPointF;
typedef struct { UINT32 GdiplusVersion; void *DebugEventCallback; BOOL SuppressBackgroundThread, SuppressExternalCodecs; } GdiplusStartupInput;

#define PixelFormat24bppRGB   0x00021808
#define PixelFormat32bppRGB   0x00022009
#define PixelFormat32bppARGB  0x0026200a
#define PixelFormat32bppPARGB 0x000e200b
#define UnitPixel 2

static GpStatus (WINAPI *pGdiplusStartup)(ULONG_PTR *, const GdiplusStartupInput *, void *);
#define GdiplusStartup pGdiplusStartup
static GpStatus (WINAPI *pGdipCreateBitmapFromScan0)(INT, INT, INT, INT, BYTE *, GpBitmap **);
#define GdipCreateBitmapFromScan0 pGdipCreateBitmapFromScan0
static GpStatus (WINAPI *pGdipCreateFromHDC)(HDC, GpGraphics **);
#define GdipCreateFromHDC pGdipCreateFromHDC
static GpStatus (WINAPI *pGdipGetImageGraphicsContext)(GpImage *, GpGraphics **);
#define GdipGetImageGraphicsContext pGdipGetImageGraphicsContext
static GpStatus (WINAPI *pGdipSetInterpolationMode)(GpGraphics *, int);
#define GdipSetInterpolationMode pGdipSetInterpolationMode
static GpStatus (WINAPI *pGdipSetPixelOffsetMode)(GpGraphics *, int);
#define GdipSetPixelOffsetMode pGdipSetPixelOffsetMode
static GpStatus (WINAPI *pGdipSetCompositingMode)(GpGraphics *, int);
#define GdipSetCompositingMode pGdipSetCompositingMode
static GpStatus (WINAPI *pGdipSetWorldTransform)(GpGraphics *, GpMatrix *);
#define GdipSetWorldTransform pGdipSetWorldTransform
static GpStatus (WINAPI *pGdipCreateMatrix2)(REAL, REAL, REAL, REAL, REAL, REAL, GpMatrix **);
#define GdipCreateMatrix2 pGdipCreateMatrix2
static GpStatus (WINAPI *pGdipDeleteMatrix)(GpMatrix *);
#define GdipDeleteMatrix pGdipDeleteMatrix
static GpStatus (WINAPI *pGdipDrawImagePointsRect)(GpGraphics *, GpImage *, const GpPointF *, INT, REAL, REAL, REAL, REAL, INT,
                                        const GpImageAttributes *, void *, void *);
#define GdipDrawImagePointsRect pGdipDrawImagePointsRect
static GpStatus (WINAPI *pGdipCreateImageAttributes)(GpImageAttributes **);
#define GdipCreateImageAttributes pGdipCreateImageAttributes
static GpStatus (WINAPI *pGdipSetImageAttributesWrapMode)(GpImageAttributes *, int, DWORD, BOOL);
#define GdipSetImageAttributesWrapMode pGdipSetImageAttributesWrapMode
static GpStatus (WINAPI *pGdipDisposeImageAttributes)(GpImageAttributes *);
#define GdipDisposeImageAttributes pGdipDisposeImageAttributes
static GpStatus (WINAPI *pGdipDeleteGraphics)(GpGraphics *);
#define GdipDeleteGraphics pGdipDeleteGraphics
static GpStatus (WINAPI *pGdipDisposeImage)(GpImage *);
#define GdipDisposeImage pGdipDisposeImage

#define SRC_W 500
#define SRC_H 720
#define DST_W 1100
#define DST_H 1100

struct test
{
    const char *name;
    int src_format;     /* 描く画像の形式 */
    int target;         /* 0: HDC (32bpp の DIB)、1: 32bppARGB の Bitmap、2: 32bppPARGB の Bitmap */
    int interpolation;  /* GdipSetInterpolationMode の値 (-1 で既定のまま) */
    int offset;         /* GdipSetPixelOffsetMode の値 (-1 で既定のまま) */
    int wrap;           /* -1: 属性なし、0..4: WrapMode (outside は 0x80ff0000) */
    REAL m[6];          /* 世界の変換 (全部 0 なら設定しない) */
    GpPointF pts[3];    /* 描き先の 3 点 */
    REAL src[4];        /* 元の矩形 */
};

static const struct test tests[] =
{
    /* Win-kurarin と同じ */
    {"kurarin",      PixelFormat32bppARGB,  0, -1, -1, -1, {0}, {{0,0},{405,0},{0,577}}, {0,0,500,720}},
    {"kurarin-ex",   PixelFormat32bppARGB,  0, 3,  -1, -1, {0}, {{0,0},{405,0},{0,577}}, {0,0,500,720}},
    {"other-sizes1", PixelFormat32bppARGB,  0, -1, -1, -1, {0}, {{0,0},{567,0},{0,615}}, {0,0,500,720}},
    {"other-sizes2", PixelFormat32bppARGB,  0, -1, -1, -1, {0}, {{0,0},{775,0},{0,643}}, {0,0,500,720}},
    {"upscale2",     PixelFormat32bppARGB,  0, -1, -1, -1, {0}, {{0,0},{1000,0},{0,1080}}, {0,0,500,720}},
    {"offset-frac",  PixelFormat32bppARGB,  0, -1, -1, -1, {0}, {{13.3f,7.6f},{419.8f,7.6f},{13.3f,590.2f}}, {3.5f,10.25f,481.0f,700.5f}},
    {"src-sub",      PixelFormat32bppARGB,  0, -1, -1, -1, {0}, {{50,40},{350,40},{50,500}}, {100,200,250,300}},
    {"flip-x",       PixelFormat32bppARGB,  0, -1, -1, -1, {0}, {{405,0},{0,0},{405,577}}, {0,0,500,720}},
    {"flip-y-neg",   PixelFormat32bppARGB,  0, -1, -1, -1, {0}, {{0,0},{405,0},{0,577}}, {0,720,500,-720}},
    {"neg-dst",      PixelFormat32bppARGB,  0, -1, -1, -1, {0}, {{-40,-30},{365,-30},{-40,547}}, {0,0,500,720}},
    {"clip-right",   PixelFormat32bppARGB,  0, -1, -1, -1, {0}, {{800,700},{1205,700},{800,1277}}, {0,0,500,720}},
    {"outside-src",  PixelFormat32bppARGB,  0, -1, -1, -1, {0}, {{0,0},{405,0},{0,577}}, {-20,-30,540,780}},
    {"nearest",      PixelFormat32bppARGB,  0, 5,  -1, -1, {0}, {{0,0},{405,0},{0,577}}, {0,0,500,720}},
    {"nearest-half", PixelFormat32bppARGB,  0, 5,  4,  -1, {0}, {{0,0},{405,0},{0,577}}, {0,0,500,720}},
    {"bicubic",      PixelFormat32bppARGB,  0, 4,  -1, -1, {0}, {{0,0},{405,0},{0,577}}, {0,0,500,720}},
    {"hq-bicubic",   PixelFormat32bppARGB,  0, 7,  -1, -1, {0}, {{0,0},{405,0},{0,577}}, {0,0,500,720}},
    {"hq-bilinear",  PixelFormat32bppARGB,  0, 6,  -1, -1, {0}, {{0,0},{405,0},{0,577}}, {0,0,500,720}},
    {"half-offset",  PixelFormat32bppARGB,  0, 3,  4,  -1, {0}, {{0,0},{405,0},{0,577}}, {0,0,500,720}},
    {"rotate",       PixelFormat32bppARGB,  0, -1, -1, -1, {0.866f,0.5f,-0.5f,0.866f,400,50}, {{0,0},{405,0},{0,577}}, {0,0,500,720}},
    {"skew-pts",     PixelFormat32bppARGB,  0, -1, -1, -1, {0}, {{10,10},{415,60},{40,587}}, {0,0,500,720}},
    {"scale-xform",  PixelFormat32bppARGB,  0, -1, -1, -1, {0.75f,0,0,0.6f,12.5f,3.25f}, {{0,0},{500,0},{0,720}}, {0,0,500,720}},
    {"wrap-clamp",   PixelFormat32bppARGB,  0, -1, -1, 4,  {0}, {{0,0},{405,0},{0,577}}, {-20,-30,540,780}},
    {"wrap-tile",    PixelFormat32bppARGB,  0, -1, -1, 0,  {0}, {{0,0},{405,0},{0,577}}, {-20,-30,540,780}},
    {"wrap-flipxy",  PixelFormat32bppARGB,  0, -1, -1, 3,  {0}, {{0,0},{405,0},{0,577}}, {-20,-30,540,780}},
    {"attr-clamp-in",PixelFormat32bppARGB,  0, -1, -1, 4,  {0}, {{0,0},{405,0},{0,577}}, {0,0,500,720}},
    {"src24",        PixelFormat24bppRGB,   0, -1, -1, -1, {0}, {{0,0},{405,0},{0,577}}, {0,0,500,720}},
    {"src32rgb",     PixelFormat32bppRGB,   0, -1, -1, -1, {0}, {{0,0},{405,0},{0,577}}, {0,0,500,720}},
    {"srcpargb",     PixelFormat32bppPARGB, 0, -1, -1, -1, {0}, {{0,0},{405,0},{0,577}}, {0,0,500,720}},
    {"to-argb",      PixelFormat32bppARGB,  1, -1, -1, -1, {0}, {{0,0},{405,0},{0,577}}, {0,0,500,720}},
    {"to-pargb",     PixelFormat32bppARGB,  2, -1, -1, -1, {0}, {{0,0},{405,0},{0,577}}, {0,0,500,720}},
    {"copy-1to1",    PixelFormat32bppARGB,  0, -1, -1, -1, {0}, {{0,0},{500,0},{0,720}}, {0,0,500,720}},
    {"copy-bmp",     PixelFormat32bppARGB,  1, -1, -1, -1, {0}, {{0,0},{500,0},{0,720}}, {0,0,500,720}},
    {"tiny",         PixelFormat32bppARGB,  0, -1, -1, -1, {0}, {{0,0},{3,0},{0,2}}, {0,0,500,720}},
    {"nearest-up",   PixelFormat32bppARGB,  0, 5,  -1, -1, {0}, {{0,0},{1000,0},{0,1080}}, {0,0,500,720}},
    {"nearest-out",  PixelFormat32bppARGB,  0, 5,  -1, 4,  {0}, {{0,0},{405,0},{0,577}}, {-20,-30,540,780}},
    {"nearest-tile", PixelFormat32bppARGB,  0, 5,  -1, 3,  {0}, {{0,0},{405,0},{0,577}}, {-20,-30,540,780}},
    {"nearest-frac", PixelFormat32bppARGB,  0, 5,  2,  -1, {0}, {{13.3f,7.6f},{419.8f,7.6f},{13.3f,590.2f}}, {3.5f,10.25f,481.0f,700.5f}},
    {"tile-x",       PixelFormat32bppARGB,  0, -1, -1, 1,  {0}, {{0,0},{405,0},{0,577}}, {-700,-30,1900,780}},
    {"tile-y-up",    PixelFormat32bppARGB,  0, -1, -1, 2,  {0}, {{0,0},{1000,0},{0,1080}}, {-20,-900,540,2500}},
    {"tile-flip-in", PixelFormat32bppARGB,  0, -1, -1, 3,  {0}, {{0,0},{405,0},{0,577}}, {0,0,500,720}},
    {"nearest-rot",  PixelFormat32bppARGB,  0, 5,  -1, -1, {0.866f,0.5f,-0.5f,0.866f,400,50}, {{0,0},{405,0},{0,577}}, {0,0,500,720}},
    {"one-col",      PixelFormat32bppARGB,  0, -1, -1, -1, {0}, {{5,5},{6,5},{5,405}}, {0,0,500,720}},
};

static int load_gdiplus(FILE *out)
{
    const char *name = getenv("GDIPBENCH_DLL");
    HMODULE mod = LoadLibraryA(name && *name ? name : "gdiplus.dll");
    if (!mod) { fprintf(out, "LoadLibrary %s failed %lu\n", name ? name : "gdiplus.dll", GetLastError()); return 0; }
    if (!(pGdiplusStartup = (void *)GetProcAddress(mod, "GdiplusStartup"))) { fprintf(out, "no GdiplusStartup\n"); return 0; }
    if (!(pGdipCreateBitmapFromScan0 = (void *)GetProcAddress(mod, "GdipCreateBitmapFromScan0"))) { fprintf(out, "no GdipCreateBitmapFromScan0\n"); return 0; }
    if (!(pGdipCreateFromHDC = (void *)GetProcAddress(mod, "GdipCreateFromHDC"))) { fprintf(out, "no GdipCreateFromHDC\n"); return 0; }
    if (!(pGdipGetImageGraphicsContext = (void *)GetProcAddress(mod, "GdipGetImageGraphicsContext"))) { fprintf(out, "no GdipGetImageGraphicsContext\n"); return 0; }
    if (!(pGdipSetInterpolationMode = (void *)GetProcAddress(mod, "GdipSetInterpolationMode"))) { fprintf(out, "no GdipSetInterpolationMode\n"); return 0; }
    if (!(pGdipSetPixelOffsetMode = (void *)GetProcAddress(mod, "GdipSetPixelOffsetMode"))) { fprintf(out, "no GdipSetPixelOffsetMode\n"); return 0; }
    if (!(pGdipSetCompositingMode = (void *)GetProcAddress(mod, "GdipSetCompositingMode"))) { fprintf(out, "no GdipSetCompositingMode\n"); return 0; }
    if (!(pGdipSetWorldTransform = (void *)GetProcAddress(mod, "GdipSetWorldTransform"))) { fprintf(out, "no GdipSetWorldTransform\n"); return 0; }
    if (!(pGdipCreateMatrix2 = (void *)GetProcAddress(mod, "GdipCreateMatrix2"))) { fprintf(out, "no GdipCreateMatrix2\n"); return 0; }
    if (!(pGdipDeleteMatrix = (void *)GetProcAddress(mod, "GdipDeleteMatrix"))) { fprintf(out, "no GdipDeleteMatrix\n"); return 0; }
    if (!(pGdipDrawImagePointsRect = (void *)GetProcAddress(mod, "GdipDrawImagePointsRect"))) { fprintf(out, "no GdipDrawImagePointsRect\n"); return 0; }
    if (!(pGdipCreateImageAttributes = (void *)GetProcAddress(mod, "GdipCreateImageAttributes"))) { fprintf(out, "no GdipCreateImageAttributes\n"); return 0; }
    if (!(pGdipSetImageAttributesWrapMode = (void *)GetProcAddress(mod, "GdipSetImageAttributesWrapMode"))) { fprintf(out, "no GdipSetImageAttributesWrapMode\n"); return 0; }
    if (!(pGdipDisposeImageAttributes = (void *)GetProcAddress(mod, "GdipDisposeImageAttributes"))) { fprintf(out, "no GdipDisposeImageAttributes\n"); return 0; }
    if (!(pGdipDeleteGraphics = (void *)GetProcAddress(mod, "GdipDeleteGraphics"))) { fprintf(out, "no GdipDeleteGraphics\n"); return 0; }
    if (!(pGdipDisposeImage = (void *)GetProcAddress(mod, "GdipDisposeImage"))) { fprintf(out, "no GdipDisposeImage\n"); return 0; }
    { char path[MAX_PATH]; GetModuleFileNameA(mod, path, sizeof(path)); fprintf(out, "# dll %s\n", path); }
    return 1;
}

static DWORD rng = 12345;
static DWORD next(void) { rng = rng * 1103515245 + 12345; return rng >> 8; }

/* 元の画像: 色は場所と雑音。不透明の所・透明の所 (色は残す)・半透明の縁を混ぜる。 */
static void make_source(DWORD *pix)
{
    int x, y;
    rng = 12345;
    for (y = 0; y < SRC_H; y++)
        for (x = 0; x < SRC_W; x++)
        {
            DWORD n = next();
            DWORD r = (x * 255 / SRC_W) ^ (n & 0x1f), g = (y * 255 / SRC_H) ^ ((n >> 5) & 0x1f), b = (n >> 10) & 0xff;
            int dx = x - SRC_W / 2, dy = y - SRC_H / 2;
            int d = dx * dx / 4 + dy * dy / 9;
            DWORD a;
            if (d < 9000) a = 255;
            else if (d < 14000) a = 255 - (d - 9000) * 255 / 5000;
            else a = (x / 37 + y / 41) % 3 == 0 ? 255 : 0;
            if ((x / 50 + y / 60) % 7 == 3) a = n & 0xff;
            pix[y * SRC_W + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
}

static void fill_target(DWORD *pix)
{
    int i;
    for (i = 0; i < DST_W * DST_H; i++)
        pix[i] = 0xff000000 | ((i * 2654435761u) >> 8 & 0x3f3f3f) | 0x404040;
}

static DWORD fnv(const DWORD *pix, int n)
{
    const BYTE *p = (const BYTE *)pix;
    DWORD h = 2166136261u;
    int i;
    for (i = 0; i < n * 4; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    int runs = argc > 1 ? atoi(argv[1]) : 5;
    const char *outname = argc > 2 ? argv[2] : "C:\\gdipbench.txt";
    const char *dumpdir = argc > 3 ? argv[3] : NULL;
    GdiplusStartupInput input = {1, NULL, FALSE, FALSE};
    ULONG_PTR token;
    DWORD *src, *src24, *dibbits, *bmpbits;
    BITMAPINFO bi;
    HDC hdc;
    HBITMAP dib;
    LARGE_INTEGER freq;
    FILE *out;
    unsigned i;
    int x, y;

    if (runs < 1) runs = 1;
    out = fopen(outname, "w");
    if (!out) return 1;
    QueryPerformanceFrequency(&freq);
    if (!load_gdiplus(out)) return 1;
    if (GdiplusStartup(&token, &input, NULL)) { fprintf(out, "GdiplusStartup failed\n"); return 1; }

    src = malloc(SRC_W * SRC_H * 4);
    src24 = malloc(SRC_W * SRC_H * 4);
    bmpbits = malloc(DST_W * DST_H * 4);
    make_source(src);
    /* 24bpp の行は 4 の倍数の幅に揃える (500*3 = 1500) */
    for (y = 0; y < SRC_H; y++)
        for (x = 0; x < SRC_W; x++)
        {
            BYTE *p = (BYTE *)src24 + y * SRC_W * 3 + x * 3;
            DWORD c = src[y * SRC_W + x];
            p[0] = c; p[1] = c >> 8; p[2] = c >> 16;
        }

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = DST_W;
    bi.bmiHeader.biHeight = -DST_H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    hdc = CreateCompatibleDC(0);
    dib = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, (void **)&dibbits, NULL, 0);
    SelectObject(hdc, dib);

    fprintf(out, "# name\thash\tmin_ms\tmedian_ms\truns\tstatus\n");
    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
    {
        const struct test *t = &tests[i];
        GpBitmap *image = NULL, *target = NULL;
        GpImageAttributes *attr = NULL;
        double *ms = malloc(runs * sizeof(double));
        DWORD hash = 0, *result;
        GpStatus status = 0, s;
        int r;
        BYTE *srcbuf = (BYTE *)(t->src_format == PixelFormat24bppRGB ? src24 : src);
        int stride = t->src_format == PixelFormat24bppRGB ? SRC_W * 3 : SRC_W * 4;

        s = GdipCreateBitmapFromScan0(SRC_W, SRC_H, stride, t->src_format, srcbuf, &image);
        if (s) { fprintf(out, "%s\t-\t-\t-\t0\tbitmap %d\n", t->name, s); continue; }
        if (t->wrap >= 0)
        {
            GdipCreateImageAttributes(&attr);
            GdipSetImageAttributesWrapMode(attr, t->wrap, 0x80ff0000, FALSE);
        }

        for (r = 0; r < runs; r++)
        {
            GpGraphics *g = NULL;
            LARGE_INTEGER a, b;

            fill_target(t->target ? bmpbits : dibbits);
            GdiFlush();
            if (t->target)
            {
                GdipCreateBitmapFromScan0(DST_W, DST_H, DST_W * 4,
                    t->target == 1 ? PixelFormat32bppARGB : PixelFormat32bppPARGB, (BYTE *)bmpbits, &target);
                GdipGetImageGraphicsContext(target, &g);
            }
            else
                GdipCreateFromHDC(hdc, &g);
            if (t->interpolation >= 0) GdipSetInterpolationMode(g, t->interpolation);
            if (t->offset >= 0) GdipSetPixelOffsetMode(g, t->offset);
            if (t->m[0] || t->m[1] || t->m[2] || t->m[3])
            {
                GpMatrix *m;
                GdipCreateMatrix2(t->m[0], t->m[1], t->m[2], t->m[3], t->m[4], t->m[5], &m);
                GdipSetWorldTransform(g, m);
                GdipDeleteMatrix(m);
            }

            QueryPerformanceCounter(&a);
            status = GdipDrawImagePointsRect(g, image, t->pts, 3, t->src[0], t->src[1], t->src[2], t->src[3],
                                             UnitPixel, attr, NULL, NULL);
            QueryPerformanceCounter(&b);
            ms[r] = (b.QuadPart - a.QuadPart) * 1000.0 / freq.QuadPart;

            GdipDeleteGraphics(g);
            if (target) { GdipDisposeImage(target); target = NULL; }
            GdiFlush();
        }

        result = t->target ? bmpbits : dibbits;
        hash = fnv(result, DST_W * DST_H);
        if (dumpdir)
        {
            char path[MAX_PATH];
            FILE *f;
            snprintf(path, sizeof(path), "%s\\%s.raw", dumpdir, t->name);
            if ((f = fopen(path, "wb"))) { fwrite(result, 4, DST_W * DST_H, f); fclose(f); }
        }
        qsort(ms, runs, sizeof(double), cmp_double);
        fprintf(out, "%s\t%08lx\t%.3f\t%.3f\t%d\t%d\n", t->name, (unsigned long)hash, ms[0], ms[runs / 2], runs, status);
        fflush(out);
        free(ms);
        if (attr) GdipDisposeImageAttributes(attr);
        GdipDisposeImage(image);
    }
    fprintf(out, "# done\n");
    fclose(out);
    return 0;
}
