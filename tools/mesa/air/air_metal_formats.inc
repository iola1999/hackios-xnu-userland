/* Pixel formats and Metal-to-Gallium format lookup. */
struct rwa_pixel_format {
   unsigned mtl;
   const char *name;
   enum pipe_format pipe;
};

static const struct rwa_pixel_format rwa_pixel_formats[] = {
   {   1, "MTLPixelFormatA8Unorm",         PIPE_FORMAT_A8_UNORM },
   {  10, "MTLPixelFormatR8Unorm",         PIPE_FORMAT_R8_UNORM },
   {  11, "MTLPixelFormatR8Unorm_sRGB",    PIPE_FORMAT_R8_SRGB },
   {  12, "MTLPixelFormatR8Snorm",         PIPE_FORMAT_R8_SNORM },
   {  13, "MTLPixelFormatR8Uint",          PIPE_FORMAT_R8_UINT },
   {  14, "MTLPixelFormatR8Sint",          PIPE_FORMAT_R8_SINT },
   {  20, "MTLPixelFormatR16Unorm",        PIPE_FORMAT_R16_UNORM },
   {  22, "MTLPixelFormatR16Snorm",        PIPE_FORMAT_R16_SNORM },
   {  23, "MTLPixelFormatR16Uint",         PIPE_FORMAT_R16_UINT },
   {  24, "MTLPixelFormatR16Sint",         PIPE_FORMAT_R16_SINT },
   {  25, "MTLPixelFormatR16Float",        PIPE_FORMAT_R16_FLOAT },
   {  30, "MTLPixelFormatRG8Unorm",        PIPE_FORMAT_R8G8_UNORM },
   {  31, "MTLPixelFormatRG8Unorm_sRGB",   PIPE_FORMAT_R8G8_SRGB },
   {  32, "MTLPixelFormatRG8Snorm",        PIPE_FORMAT_R8G8_SNORM },
   {  33, "MTLPixelFormatRG8Uint",         PIPE_FORMAT_R8G8_UINT },
   {  34, "MTLPixelFormatRG8Sint",         PIPE_FORMAT_R8G8_SINT },

   /* The packed 16-bit four, by the reading argued above: the first channel of the
    * Metal name is at the least significant bits, which is also Gallium's rule, so the
    * two names are the same letters in the same order.
    */
   {  40, "MTLPixelFormatB5G6R5Unorm",     PIPE_FORMAT_R5G6B5_UNORM },
   {  41, "MTLPixelFormatA1BGR5Unorm",     PIPE_FORMAT_A1R5G5B5_UNORM },
   {  42, "MTLPixelFormatABGR4Unorm",      PIPE_FORMAT_A4R4G4B4_UNORM },
   {  43, "MTLPixelFormatBGR5A1Unorm",     PIPE_FORMAT_R5G5B5A1_UNORM },
   {  53, "MTLPixelFormatR32Uint",         PIPE_FORMAT_R32_UINT },
   {  54, "MTLPixelFormatR32Sint",         PIPE_FORMAT_R32_SINT },
   {  55, "MTLPixelFormatR32Float",        PIPE_FORMAT_R32_FLOAT },
   {  60, "MTLPixelFormatRG16Unorm",       PIPE_FORMAT_R16G16_UNORM },
   {  62, "MTLPixelFormatRG16Snorm",       PIPE_FORMAT_R16G16_SNORM },
   {  63, "MTLPixelFormatRG16Uint",        PIPE_FORMAT_R16G16_UINT },
   {  64, "MTLPixelFormatRG16Sint",        PIPE_FORMAT_R16G16_SINT },
   {  65, "MTLPixelFormatRG16Float",       PIPE_FORMAT_R16G16_FLOAT },
   {  70, "MTLPixelFormatRGBA8Unorm",      PIPE_FORMAT_R8G8B8A8_UNORM },
   {  71, "MTLPixelFormatRGBA8Unorm_sRGB", PIPE_FORMAT_R8G8B8A8_SRGB },
   {  72, "MTLPixelFormatRGBA8Snorm",      PIPE_FORMAT_R8G8B8A8_SNORM },
   {  73, "MTLPixelFormatRGBA8Uint",       PIPE_FORMAT_R8G8B8A8_UINT },
   {  74, "MTLPixelFormatRGBA8Sint",       PIPE_FORMAT_R8G8B8A8_SINT },
   {  80, "MTLPixelFormatBGRA8Unorm",      PIPE_FORMAT_B8G8R8A8_UNORM },
   {  81, "MTLPixelFormatBGRA8Unorm_sRGB", PIPE_FORMAT_B8G8R8A8_SRGB },

   /* The packed 32-bit five, by the same reading as the packed 16-bit four: the first
    * channel of the Metal name is at the least significant bits. iconservicesagent asks
    * for RGB10A2Unorm when it renders icons.
    */
   {  90, "MTLPixelFormatRGB10A2Unorm",    PIPE_FORMAT_R10G10B10A2_UNORM },
   {  91, "MTLPixelFormatRGB10A2Uint",     PIPE_FORMAT_R10G10B10A2_UINT },
   {  92, "MTLPixelFormatRG11B10Float",    PIPE_FORMAT_R11G11B10_FLOAT },
   {  93, "MTLPixelFormatRGB9E5Float",     PIPE_FORMAT_R9G9B9E5_FLOAT },
   {  94, "MTLPixelFormatBGR10A2Unorm",    PIPE_FORMAT_B10G10R10A2_UNORM },
   { 103, "MTLPixelFormatRG32Uint",        PIPE_FORMAT_R32G32_UINT },
   { 104, "MTLPixelFormatRG32Sint",        PIPE_FORMAT_R32G32_SINT },
   { 105, "MTLPixelFormatRG32Float",       PIPE_FORMAT_R32G32_FLOAT },
   { 110, "MTLPixelFormatRGBA16Unorm",     PIPE_FORMAT_R16G16B16A16_UNORM },
   { 112, "MTLPixelFormatRGBA16Snorm",     PIPE_FORMAT_R16G16B16A16_SNORM },
   { 113, "MTLPixelFormatRGBA16Uint",      PIPE_FORMAT_R16G16B16A16_UINT },
   { 114, "MTLPixelFormatRGBA16Sint",      PIPE_FORMAT_R16G16B16A16_SINT },
   { 115, "MTLPixelFormatRGBA16Float",     PIPE_FORMAT_R16G16B16A16_FLOAT },
   { 123, "MTLPixelFormatRGBA32Uint",      PIPE_FORMAT_R32G32B32A32_UINT },
   { 124, "MTLPixelFormatRGBA32Sint",      PIPE_FORMAT_R32G32B32A32_SINT },
   { 125, "MTLPixelFormatRGBA32Float",     PIPE_FORMAT_R32G32B32A32_FLOAT },

   /* The block-compressed formats MercuryPosterExtension's KTX textures use: ASTC (albedo-a.ktx
    * is 6x6 sRGB, normals-a.ktx 4x4) and BC1 (the jurin_*-D23 masks). A block has the layout
    * the ASTC or BCn specification fixes, which Metal and Gallium both use, so these are the
    * identity on memory as well.
    */
   { 130, "MTLPixelFormatBC1_RGBA",          PIPE_FORMAT_DXT1_RGBA },
   { 131, "MTLPixelFormatBC1_RGBA_sRGB",     PIPE_FORMAT_DXT1_SRGBA },
   { 132, "MTLPixelFormatBC2_RGBA",          PIPE_FORMAT_DXT3_RGBA },
   { 133, "MTLPixelFormatBC2_RGBA_sRGB",     PIPE_FORMAT_DXT3_SRGBA },
   { 134, "MTLPixelFormatBC3_RGBA",          PIPE_FORMAT_DXT5_RGBA },
   { 135, "MTLPixelFormatBC3_RGBA_sRGB",     PIPE_FORMAT_DXT5_SRGBA },
   { 140, "MTLPixelFormatBC4_RUnorm",        PIPE_FORMAT_RGTC1_UNORM },
   { 141, "MTLPixelFormatBC4_RSnorm",        PIPE_FORMAT_RGTC1_SNORM },
   { 142, "MTLPixelFormatBC5_RGUnorm",       PIPE_FORMAT_RGTC2_UNORM },
   { 143, "MTLPixelFormatBC5_RGSnorm",       PIPE_FORMAT_RGTC2_SNORM },
   { 150, "MTLPixelFormatBC6H_RGBFloat",     PIPE_FORMAT_BPTC_RGB_FLOAT },
   { 151, "MTLPixelFormatBC6H_RGBUfloat",    PIPE_FORMAT_BPTC_RGB_UFLOAT },
   { 152, "MTLPixelFormatBC7_RGBAUnorm",     PIPE_FORMAT_BPTC_RGBA_UNORM },
   { 153, "MTLPixelFormatBC7_RGBAUnorm_sRGB", PIPE_FORMAT_BPTC_SRGBA },
   /* ASTC LDR and sRGB: Metal numbers the sRGB sizes from 186 and the LDR ones from 204 in
    * the same order. */
#define ASTC(k, w, h) \
   { 186 + k, "MTLPixelFormatASTC_" #w "x" #h "_sRGB", PIPE_FORMAT_ASTC_##w##x##h##_SRGB }, \
   { 204 + k, "MTLPixelFormatASTC_" #w "x" #h "_LDR", PIPE_FORMAT_ASTC_##w##x##h }
   ASTC(0, 4, 4), ASTC(1, 5, 4), ASTC(2, 5, 5), ASTC(3, 6, 5), ASTC(4, 6, 6), ASTC(6, 8, 5),
   ASTC(7, 8, 6), ASTC(8, 8, 8), ASTC(9, 10, 5), ASTC(10, 10, 6), ASTC(11, 10, 8),
   ASTC(12, 10, 10), ASTC(13, 12, 10), ASTC(14, 12, 12),
#undef ASTC

   /* Depth and stencil. These are in the same table because a depth texture is a
    * texture (air_metal.h) and every caller asks for one the same way; what the format
    * changes is which binding it can have and how its texels are read, and both of
    * those are decided from the format itself below, never from a second list.
    *
    * The four are the ones iOS has and Gallium names the same bytes for. The depth
    * aspect of each is the identity on memory -- 16-bit normalised, 32-bit float -- and
    * Z32_FLOAT_S8X24_UINT is Gallium's spelling of "32-bit depth, 8-bit stencil, the
    * rest padding", which is Metal's Depth32Float_Stencil8 exactly. What this library
    * never does is pack or unpack those by hand: rwa_depth_read goes through
    * util_format's own unpackers, because the one thing worse than declining a depth
    * format is returning a depth that is nearly right.
    */
   { 250, "MTLPixelFormatDepth16Unorm",    PIPE_FORMAT_Z16_UNORM },
   { 252, "MTLPixelFormatDepth32Float",    PIPE_FORMAT_Z32_FLOAT },
   { 253, "MTLPixelFormatStencil8",        PIPE_FORMAT_S8_UINT },
   { 260, "MTLPixelFormatDepth32Float_Stencil8", PIPE_FORMAT_Z32_FLOAT_S8X24_UINT },
};

/* Whether a format carries each aspect, asked of Mesa's own description rather than of
 * the MTLPixelFormat number: one reader for "has this a depth?" means a format added to
 * the table above cannot be half-handled.
 */
static bool
format_has_depth(enum pipe_format f)
{
   return util_format_has_depth(util_format_description(f));
}

static bool
format_has_stencil(enum pipe_format f)
{
   return util_format_has_stencil(util_format_description(f));
}

static bool
format_is_depth_stencil(enum pipe_format f)
{
   return format_has_depth(f) || format_has_stencil(f);
}

/*
 * THE FOUR PACKED 16-BIT FORMATS ARE OFF BY DEFAULT, and that is not timidity: the
 * table above says which pipe_format each Metal name means under the reading taken,
 * and `air_metal_test --pixel-formats` measures that THIS LIBRARY implements that
 * reading exactly -- 15 probes, a word with one field set, a real draw, 15 in the
 * channel the reading claims. What no measurement on this machine can reach is whether
 * the reading is APPLE's. Nothing here states the bit positions: Apple's own
 * MTLPixelFormat.h carries no comment on them and Metal.framework's binary carries the
 * names and the values and no layout. The remaining support is an inference from the
 * packed 32-bit formats whose layouts other standards fix -- RGB9E5Float, RG11B10Float,
 * RGB10A2Unorm, all of which put the FIRST-NAMED channel in the LOW bits -- and an
 * inference is not a measurement.
 *
 * The experiment that settles it needs Apple's own code to display a word this library
 * wrote, which is a boot and not a test here: under the other reading red and blue are
 * exchanged, so a surface this port draws cyan would be yellow and the other way round.
 * SERVED BY DEFAULT NOW, and the order is the MIRROR of what it was. The boot that
 * served them under the first reading drew the home screen's wallpaper layer -- flat,
 * unblended, the bottom of the stack and 8 004 sampled pixels of it -- as exactly
 * (255, 12, 12) where the wallpaper is blue. That is the sign this file wrote down in
 * advance: under the other reading red and blue are exchanged. Inverting that pixel
 * through our own packing and back out through each candidate layout, the only
 * readings that make it a blue are the ones with red and blue exchanged, and of those
 * only the PURE EXCHANGE -- same field offsets, same widths -- leaves the rest of the
 * picture clean: moving the one-bit alpha as well misaligns the five-bit fields and
 * shows as visible striping across the icons, which the real screen does not have.
 * Re-rendered through the exchange, the whole page reads as iOS: a solid blue ground,
 * white labels, a blue-to-violet Weather icon with a white cloud (logs/packed.png).
 *
 * So Gallium's names and Metal's run in OPPOSITE directions over the colour channels,
 * with the alpha where the name puts it -- Metal's B5G6R5 is Gallium's R5G6B5, Metal's
 * BGR5A1 is Gallium's R5G5B5A1. All four of the first mappings were made by one rule
 * and one measurement falsified the rule, so all four move together.
 *
 * `RWA_PACKED16=0` declines them again, which is how the boot before this one ran and
 * is what to set if they ever need taking out of the picture; nothing else is a knob
 * here.
 *
 * Read once, because a texture is created per frame and getenv is not free.
 */
static bool
packed16_served(void)
{
   static int on = -1;
   if (on < 0) {
      const char *v = getenv("RWA_PACKED16");
      on = !(v && *v == '0');
   }
   return on != 0;
}

static const struct rwa_pixel_format *
pixel_format(unsigned mtl)
{
   if (mtl >= 40 && mtl <= 43 && !packed16_served())
      return NULL;
   for (unsigned i = 0; i < ARRAY_SIZE(rwa_pixel_formats); i++)
      if (rwa_pixel_formats[i].mtl == mtl)
         return &rwa_pixel_formats[i];
   return NULL;
}
