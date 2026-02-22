/*
===========================================================================
DDS (DirectDraw Surface) texture loader for renderergl1.

Decompresses DXT1, DXT3, DXT5 blocks to RGBA and handles uncompressed
DDS formats. CoD1 uses DDS textures extensively for its assets.
===========================================================================
*/

#include "tr_local.h"

/* ---- DDS header structures ---- */

#define DDS_MAGIC           0x20534444  /* "DDS " little-endian */
#define DDPF_ALPHAPIXELS    0x1
#define DDPF_FOURCC         0x4
#define DDPF_RGB            0x40
#define DDPF_LUMINANCE      0x20000

#define FOURCC(a,b,c,d) ((unsigned int)((a)|((b)<<8)|((c)<<16)|((d)<<24)))
#define FOURCC_DXT1  FOURCC('D','X','T','1')
#define FOURCC_DXT3  FOURCC('D','X','T','3')
#define FOURCC_DXT5  FOURCC('D','X','T','5')

typedef struct {
	unsigned int size;
	unsigned int flags;
	unsigned int height;
	unsigned int width;
	unsigned int pitchOrLinearSize;
	unsigned int depth;
	unsigned int mipMapCount;
	unsigned int reserved1[11];
	unsigned int pfSize;
	unsigned int pfFlags;
	unsigned int pfFourCC;
	unsigned int pfRGBBitCount;
	unsigned int pfRBitMask;
	unsigned int pfGBitMask;
	unsigned int pfBBitMask;
	unsigned int pfABitMask;
	unsigned int caps;
	unsigned int caps2;
	unsigned int caps3;
	unsigned int caps4;
	unsigned int reserved2;
} ddsHeader_t;

/* ---- DXT decompression helpers ---- */

static void DecodeDXT1Block( const byte *src, byte *dst, int stride, qboolean dxt1Alpha )
{
	unsigned short c0 = (unsigned short)(src[0] | (src[1] << 8));
	unsigned short c1 = (unsigned short)(src[2] | (src[3] << 8));
	unsigned int   indices = (unsigned int)(src[4] | (src[5]<<8) | (src[6]<<16) | (src[7]<<24));

	byte col[4][4];

	/* Expand RGB565 -> RGB888 */
	col[0][0] = (byte)(((c0 >> 11) & 0x1F) * 255 / 31);
	col[0][1] = (byte)(((c0 >>  5) & 0x3F) * 255 / 63);
	col[0][2] = (byte)(((c0      ) & 0x1F) * 255 / 31);
	col[0][3] = 255;

	col[1][0] = (byte)(((c1 >> 11) & 0x1F) * 255 / 31);
	col[1][1] = (byte)(((c1 >>  5) & 0x3F) * 255 / 63);
	col[1][2] = (byte)(((c1      ) & 0x1F) * 255 / 31);
	col[1][3] = 255;

	if( c0 > c1 || !dxt1Alpha )
	{
		/* 4-color block */
		col[2][0] = (byte)((2*col[0][0] + col[1][0] + 1) / 3);
		col[2][1] = (byte)((2*col[0][1] + col[1][1] + 1) / 3);
		col[2][2] = (byte)((2*col[0][2] + col[1][2] + 1) / 3);
		col[2][3] = 255;

		col[3][0] = (byte)((col[0][0] + 2*col[1][0] + 1) / 3);
		col[3][1] = (byte)((col[0][1] + 2*col[1][1] + 1) / 3);
		col[3][2] = (byte)((col[0][2] + 2*col[1][2] + 1) / 3);
		col[3][3] = 255;
	}
	else
	{
		/* 3-color block + transparent */
		col[2][0] = (byte)((col[0][0] + col[1][0]) / 2);
		col[2][1] = (byte)((col[0][1] + col[1][1]) / 2);
		col[2][2] = (byte)((col[0][2] + col[1][2]) / 2);
		col[2][3] = 255;

		col[3][0] = 0;
		col[3][1] = 0;
		col[3][2] = 0;
		col[3][3] = 0;
	}

	{
		int x, y;
		for( y = 0; y < 4; y++ )
		{
			for( x = 0; x < 4; x++ )
			{
				int idx = (indices >> (y*8 + x*2)) & 3;
				byte *p = dst + y * stride + x * 4;
				p[0] = col[idx][0];
				p[1] = col[idx][1];
				p[2] = col[idx][2];
				p[3] = col[idx][3];
			}
		}
	}
}

static void DecompressDXT1( const byte *src, byte *dst, int width, int height )
{
	int bx, by;
	int bw = (width  + 3) / 4;
	int bh = (height + 3) / 4;
	int stride = width * 4;

	for( by = 0; by < bh; by++ )
	{
		for( bx = 0; bx < bw; bx++ )
		{
			DecodeDXT1Block( src, dst + (by*4)*stride + bx*4*4, stride, qtrue );
			src += 8;
		}
	}
}

static void DecompressDXT3( const byte *src, byte *dst, int width, int height )
{
	int bx, by;
	int bw = (width  + 3) / 4;
	int bh = (height + 3) / 4;
	int stride = width * 4;

	for( by = 0; by < bh; by++ )
	{
		for( bx = 0; bx < bw; bx++ )
		{
			byte *blockDst = dst + (by*4)*stride + bx*4*4;
			int x, y;

			/* First 8 bytes: explicit 4-bit alpha per pixel */
			for( y = 0; y < 4; y++ )
			{
				for( x = 0; x < 4; x++ )
				{
					int shift = (y*4 + x) * 4;
					int alphaByte = shift / 8;
					int alphaBit  = shift % 8;
					byte a = (byte)(((src[alphaByte] >> alphaBit) & 0xF) * 255 / 15);
					blockDst[y*stride + x*4 + 3] = a;
				}
			}

			/* Next 8 bytes: DXT1 color (force 4-color, no transparency) */
			DecodeDXT1Block( src + 8, blockDst, stride, qfalse );

			/* Restore alpha that DXT1 decode may have set to 255 */
			for( y = 0; y < 4; y++ )
			{
				for( x = 0; x < 4; x++ )
				{
					int shift = (y*4 + x) * 4;
					int alphaByte = shift / 8;
					int alphaBit  = shift % 8;
					blockDst[y*stride + x*4 + 3] =
						(byte)(((src[alphaByte] >> alphaBit) & 0xF) * 255 / 15);
				}
			}

			src += 16;
		}
	}
}

static void DecompressDXT5( const byte *src, byte *dst, int width, int height )
{
	int bx, by;
	int bw = (width  + 3) / 4;
	int bh = (height + 3) / 4;
	int stride = width * 4;

	for( by = 0; by < bh; by++ )
	{
		for( bx = 0; bx < bw; bx++ )
		{
			byte *blockDst = dst + (by*4)*stride + bx*4*4;
			int x, y;

			byte a0 = src[0];
			byte a1 = src[1];
			/* 6 bytes of 3-bit alpha indices (48 bits, 16 pixels) */
			unsigned long long alphaBits =
				(unsigned long long)src[2]        |
				((unsigned long long)src[3] <<  8) |
				((unsigned long long)src[4] << 16) |
				((unsigned long long)src[5] << 24) |
				((unsigned long long)src[6] << 32) |
				((unsigned long long)src[7] << 40);

			/* Build 8-entry alpha palette */
			byte apal[8];
			apal[0] = a0;
			apal[1] = a1;
			if( a0 > a1 )
			{
				apal[2] = (byte)((6*a0 + 1*a1 + 3) / 7);
				apal[3] = (byte)((5*a0 + 2*a1 + 3) / 7);
				apal[4] = (byte)((4*a0 + 3*a1 + 3) / 7);
				apal[5] = (byte)((3*a0 + 4*a1 + 3) / 7);
				apal[6] = (byte)((2*a0 + 5*a1 + 3) / 7);
				apal[7] = (byte)((1*a0 + 6*a1 + 3) / 7);
			}
			else
			{
				apal[2] = (byte)((4*a0 + 1*a1 + 2) / 5);
				apal[3] = (byte)((3*a0 + 2*a1 + 2) / 5);
				apal[4] = (byte)((2*a0 + 3*a1 + 2) / 5);
				apal[5] = (byte)((1*a0 + 4*a1 + 2) / 5);
				apal[6] = 0;
				apal[7] = 255;
			}

			/* Decode color block (force 4-color, no 1-bit alpha) */
			DecodeDXT1Block( src + 8, blockDst, stride, qfalse );

			/* Apply alpha */
			for( y = 0; y < 4; y++ )
			{
				for( x = 0; x < 4; x++ )
				{
					int pix  = y*4 + x;
					int aidx = (int)((alphaBits >> (pix*3)) & 7);
					blockDst[y*stride + x*4 + 3] = apal[aidx];
				}
			}

			src += 16;
		}
	}
}

/* ---- Uncompressed DDS ---- */

static byte SampleChannel( unsigned int val, unsigned int mask )
{
	unsigned int m = mask;
	int shift = 0;
	unsigned int maxVal;

	if( !m ) return 0;

	while( !(m & 1) ) { m >>= 1; shift++; }
	maxVal = m;

	val = (val >> shift) & maxVal;
	if( maxVal == 0xFF ) return (byte)val;
	return (byte)(val * 255 / maxVal);
}

static void DecodeUncompressed( const byte *src, byte *dst, int width, int height,
	unsigned int rMask, unsigned int gMask, unsigned int bMask, unsigned int aMask,
	int bytesPerPixel )
{
	int i;
	int numPixels = width * height;

	for( i = 0; i < numPixels; i++ )
	{
		unsigned int val = 0;
		int b;
		for( b = 0; b < bytesPerPixel && b < 4; b++ )
			val |= ((unsigned int)src[b]) << (b*8);

		dst[0] = SampleChannel( val, rMask );
		dst[1] = SampleChannel( val, gMask );
		dst[2] = SampleChannel( val, bMask );
		dst[3] = aMask ? SampleChannel( val, aMask ) : 255;

		src += bytesPerPixel;
		dst += 4;
	}
}

/* ---- Public loader ---- */

void R_LoadDDS( const char *name, byte **pic, int *width, int *height )
{
	union { byte *b; void *v; } buffer;
	int len;
	const byte *data;
	const ddsHeader_t *hdr;

	*pic    = NULL;
	*width  = 0;
	*height = 0;

	len = ri.FS_ReadFile( (char *)name, &buffer.v );
	if( !buffer.b || len < 0 )
		return;

	if( len < 4 + (int)sizeof(ddsHeader_t) )
	{
		ri.Printf( PRINT_WARNING, "R_LoadDDS: %s too small\n", name );
		ri.FS_FreeFile( buffer.v );
		return;
	}

	if( *(unsigned int *)buffer.b != DDS_MAGIC )
	{
		ri.Printf( PRINT_WARNING, "R_LoadDDS: %s is not a DDS file\n", name );
		ri.FS_FreeFile( buffer.v );
		return;
	}

	hdr = (const ddsHeader_t *)(buffer.b + 4);

	if( hdr->size != sizeof(ddsHeader_t) )
	{
		ri.Printf( PRINT_WARNING, "R_LoadDDS: %s has bad header size\n", name );
		ri.FS_FreeFile( buffer.v );
		return;
	}

	*width  = (int)hdr->width;
	*height = (int)hdr->height;

	if( *width <= 0 || *height <= 0 || *width > 4096 || *height > 4096 )
	{
		ri.Printf( PRINT_WARNING, "R_LoadDDS: %s has invalid dimensions %dx%d\n",
			name, *width, *height );
		ri.FS_FreeFile( buffer.v );
		*width = *height = 0;
		return;
	}

	data = buffer.b + 4 + sizeof(ddsHeader_t);

	/* Output is always RGBA */
	*pic = ri.Malloc( (*width) * (*height) * 4 );
	Com_Memset( *pic, 0, (*width) * (*height) * 4 );

	if( hdr->pfFlags & DDPF_FOURCC )
	{
		unsigned int fourCC = hdr->pfFourCC;
		if( fourCC == FOURCC_DXT1 )
			DecompressDXT1( data, *pic, *width, *height );
		else if( fourCC == FOURCC_DXT3 )
			DecompressDXT3( data, *pic, *width, *height );
		else if( fourCC == FOURCC_DXT5 )
			DecompressDXT5( data, *pic, *width, *height );
		else
		{
			ri.Printf( PRINT_WARNING, "R_LoadDDS: %s unsupported FourCC 0x%08X\n",
				name, fourCC );
			ri.Free( *pic );
			*pic = NULL;
			*width = *height = 0;
		}
	}
	else if( hdr->pfFlags & DDPF_RGB )
	{
		int bpp = (int)(hdr->pfRGBBitCount + 7) / 8;
		if( bpp < 1 || bpp > 4 )
		{
			ri.Printf( PRINT_WARNING, "R_LoadDDS: %s unsupported bit depth %d\n",
				name, hdr->pfRGBBitCount );
			ri.Free( *pic );
			*pic = NULL;
			*width = *height = 0;
		}
		else
		{
			DecodeUncompressed( data, *pic, *width, *height,
				hdr->pfRBitMask, hdr->pfGBitMask, hdr->pfBBitMask,
				(hdr->pfFlags & DDPF_ALPHAPIXELS) ? hdr->pfABitMask : 0,
				bpp );
		}
	}
	else if( hdr->pfFlags & DDPF_LUMINANCE )
	{
		/* Expand luminance to RGBA */
		int i, numPixels = (*width) * (*height);
		const byte *s = data;
		byte *d = *pic;
		for( i = 0; i < numPixels; i++, s++, d += 4 )
		{
			d[0] = d[1] = d[2] = *s;
			d[3] = (hdr->pfFlags & DDPF_ALPHAPIXELS) ? s[1] : 255;
		}
	}
	else
	{
		ri.Printf( PRINT_WARNING, "R_LoadDDS: %s unknown pixel format flags 0x%X\n",
			name, hdr->pfFlags );
		ri.Free( *pic );
		*pic = NULL;
		*width = *height = 0;
	}

	ri.FS_FreeFile( buffer.v );
}
