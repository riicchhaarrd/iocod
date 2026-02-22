/*
===========================================================================
tr_bsp_cod1.c -- CoD1 IBSP version 59 BSP loader for the renderer.

The CoD1 BSP format differs from Q3 in:
  - 33 lumps vs 17; lump entries are [filelen, fileofs] (reversed)
  - Geometry: TriangleSoups + Vertices + Triangles instead of Q3 Surfaces
  - BSP nodes: identical 36-byte Q3 format
  - BSP leafs: 36-byte format (no per-leaf bounding box)
  - Materials: 72-byte entries, same layout as Q3 dshader_t
  - Lightmaps: 128x128x3 RGB, identical to Q3
===========================================================================
*/

#include "tr_local.h"

/* Access the globals defined in tr_bsp.c */
extern world_t   s_worldData;
extern byte     *fileBase;

/* Functions defined in tr_bsp.c that we reuse */
void R_ColorShiftLightingBytes( byte in[4], byte out[4] );
void R_SetParent( mnode_t *node, mnode_t *parent );
void R_LoadEntities( lump_t *l );

/* -------------------------------------------------------------------------
   Helpers
   ------------------------------------------------------------------------- */

/*
 * R_GetCod1Lump – extract a CoD1 lump as a Q3-compatible lump_t.
 * CoD1 lump entries: [filelen: int, fileofs: int]  (reversed vs Q3).
 */
static lump_t R_GetCod1Lump( const byte *base, int idx ) {
	const cod1_dheader_t *hdr = (const cod1_dheader_t *)base;
	lump_t out;
	out.filelen = LittleLong( hdr->lumps[idx].filelen );
	out.fileofs = LittleLong( hdr->lumps[idx].fileofs );
	return out;
}

/* -------------------------------------------------------------------------
   Shaders / materials

   CoD1 material entry is 72 bytes identical to Q3 dshader_t layout:
     char name[64] + int surfaceFlags + int contentFlags
   So we can call the existing Q3 R_LoadShaders directly.
   ------------------------------------------------------------------------- */
static void R_LoadShadersCod1( const byte *base ) {
	lump_t l = R_GetCod1Lump( base, COD1_LUMP_MATERIALS );

	/* R_LoadShaders reads s_worldData.shaders which it allocates from fileBase */
	fileBase = (byte *)base;
	/* Temporarily redirect: call the existing Q3 loader */
	{
		dshader_t *in;
		int        i, count;

		in    = (dshader_t *)( base + l.fileofs );
		count = l.filelen / sizeof( dshader_t );

		if ( count < 1 )
			ri.Error( ERR_DROP, "R_LoadShadersCod1: map with no shaders" );

		s_worldData.shaders    = ri.Hunk_Alloc( count * sizeof( *s_worldData.shaders ), h_low );
		s_worldData.numShaders = count;

		Com_Memcpy( s_worldData.shaders, in, count * sizeof( *s_worldData.shaders ) );
		for ( i = 0; i < count; i++ ) {
			s_worldData.shaders[i].surfaceFlags = LittleLong( s_worldData.shaders[i].surfaceFlags );
			s_worldData.shaders[i].contentFlags = LittleLong( s_worldData.shaders[i].contentFlags );
		}
	}
}

/* -------------------------------------------------------------------------
   Lightmaps – identical 128×128×3 format; reuse existing Q3 loader.
   ------------------------------------------------------------------------- */
static void R_LoadLightmapsCod1( const byte *base ) {
	lump_t      l = R_GetCod1Lump( base, COD1_LUMP_LIGHTMAPS );
	byte       *buf, *buf_p;
	int         len, i, j;
	static byte image[128 * 128 * 4];  /* static to avoid large stack */
	byte        tmp[4];

	len = l.filelen;
	if ( !len )
		return;
	buf = (byte *)base + l.fileofs;

	R_IssuePendingRenderCommands();

	tr.numLightmaps = len / ( 128 * 128 * 3 );
	if ( tr.numLightmaps == 1 )
		tr.numLightmaps++;   /* Q3 hack: avoid fullbright on single-lightmap maps */

	if ( r_vertexLight->integer || glConfig.hardwareType == GLHW_PERMEDIA2 )
		return;

	tr.lightmaps = ri.Hunk_Alloc( tr.numLightmaps * sizeof( image_t * ), h_low );
	for ( i = 0; i < tr.numLightmaps; i++ ) {
		buf_p = buf + i * 128 * 128 * 3;
		for ( j = 0; j < 128 * 128; j++ ) {
			tmp[0] = buf_p[j*3+0];
			tmp[1] = buf_p[j*3+1];
			tmp[2] = buf_p[j*3+2];
			tmp[3] = 255;
			R_ColorShiftLightingBytes( tmp, &image[j*4] );
		}
		tr.lightmaps[i] = R_CreateImage( va( "*lightmap%d", i ), image,
			128, 128, IMGTYPE_COLORALPHA,
			IMGFLAG_NOLIGHTSCALE | IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, 0 );
	}
}

/* -------------------------------------------------------------------------
   Planes – identical 16-byte Q3 format.
   ------------------------------------------------------------------------- */
static void R_LoadPlanesCod1( const byte *base ) {
	lump_t    l = R_GetCod1Lump( base, COD1_LUMP_PLANES );
	dplane_t *in;
	cplane_t *out;
	int        i, j, count;
	int        bits;

	in    = (dplane_t *)( base + l.fileofs );
	count = l.filelen / sizeof( dplane_t );

	out = ri.Hunk_Alloc( count * sizeof( *out ), h_low );
	s_worldData.planes    = out;
	s_worldData.numplanes = count;

	for ( i = 0; i < count; i++, in++, out++ ) {
		bits = 0;
		for ( j = 0; j < 3; j++ ) {
			out->normal[j] = LittleFloat( in->normal[j] );
			if ( out->normal[j] < 0 )
				bits |= 1 << j;
		}
		out->dist     = LittleFloat( in->dist );
		out->type     = PlaneTypeForNormal( out->normal );
		out->signbits = bits;
	}
}

/* -------------------------------------------------------------------------
   Geometry – TriangleSoups + Vertices + Triangles → srfTriangles_t

   CoD1 vertex layout is byte-for-byte identical to Q3 drawVert_t:
     float xyz[3] + float st[2] + float lightmap[2] + float normal[3] + byte color[4]
   Triangle indices are u16 in CoD1 (vs i32 in Q3).
   ------------------------------------------------------------------------- */
static void R_LoadCod1Surfaces( const byte *base ) {
	const cod1_trianglesoup_t *ts_in;
	const cod1_vertex_t       *verts_base;
	const unsigned short      *tris_base;
	int  num_ts, i, j;
	lump_t ts_l, vt_l, tr_l;

	ts_l = R_GetCod1Lump( base, COD1_LUMP_TRIANGLESOUPS );
	vt_l = R_GetCod1Lump( base, COD1_LUMP_VERTICES );
	tr_l = R_GetCod1Lump( base, COD1_LUMP_TRIANGLES );

	if ( ts_l.filelen % sizeof( cod1_trianglesoup_t ) )
		ri.Error( ERR_DROP, "R_LoadCod1Surfaces: bad TriangleSoup lump size" );

	num_ts     = ts_l.filelen / sizeof( cod1_trianglesoup_t );
	ts_in      = (const cod1_trianglesoup_t *)( base + ts_l.fileofs );
	verts_base = (const cod1_vertex_t *)( base + vt_l.fileofs );
	tris_base  = (const unsigned short *)( base + tr_l.fileofs );

	s_worldData.surfaces    = ri.Hunk_Alloc( num_ts * sizeof( msurface_t ), h_low );
	s_worldData.numsurfaces = num_ts;

	ri.Printf( PRINT_ALL, "...loading %d CoD1 triangle soups\n", num_ts );

	for ( i = 0; i < num_ts; i++ ) {
		msurface_t      *surf = &s_worldData.surfaces[i];
		srfTriangles_t  *tri;
		const cod1_trianglesoup_t *ts = &ts_in[i];
		int   mat_idx    = LittleShort( ts->materialIdx );
		int   verts_off  = LittleLong ( ts->vertsOffset );
		int   verts_len  = LittleShort( ts->vertsLength );
		int   tris_off   = LittleLong ( ts->trisOffset  );
		int   tris_len   = LittleShort( ts->trisLength  );

		/* Assign shader from material name */
		if ( mat_idx >= 0 && mat_idx < s_worldData.numShaders ) {
			dshader_t *dsh = &s_worldData.shaders[mat_idx];
			if ( r_vertexLight->integer || glConfig.hardwareType == GLHW_PERMEDIA2 ) {
				surf->shader = R_FindShader( dsh->shader, LIGHTMAP_BY_VERTEX, qtrue );
			} else {
				surf->shader = R_FindShader( dsh->shader, LIGHTMAP_BY_VERTEX, qtrue );
			}
		} else {
			surf->shader = tr.defaultShader;
		}
		if ( r_singleShader->integer && !surf->shader->isSky )
			surf->shader = tr.defaultShader;

		surf->fogIndex = 0;

		/* Allocate srfTriangles_t with embedded verts + indexes */
		tri = ri.Hunk_Alloc(
			sizeof( srfTriangles_t ) +
			verts_len * sizeof( drawVert_t ) +
			tris_len  * sizeof( int ),
			h_low );
		tri->surfaceType = SF_TRIANGLES;
		tri->numVerts    = verts_len;
		tri->numIndexes  = tris_len;
		tri->verts       = (drawVert_t *)( tri + 1 );
		tri->indexes     = (int *)( tri->verts + verts_len );

		surf->data = (surfaceType_t *)tri;

		/* Copy vertices – CoD1 vertex is byte-identical to Q3 drawVert_t */
		ClearBounds( tri->bounds[0], tri->bounds[1] );
		for ( j = 0; j < verts_len; j++ ) {
			const cod1_vertex_t *src = &verts_base[verts_off + j];
			drawVert_t          *dst = &tri->verts[j];

			dst->xyz[0]       = LittleFloat( src->position[0] );
			dst->xyz[1]       = LittleFloat( src->position[1] );
			dst->xyz[2]       = LittleFloat( src->position[2] );
			dst->st[0]        = LittleFloat( src->uv[0] );
			dst->st[1]        = LittleFloat( src->uv[1] );
			dst->lightmap[0]  = LittleFloat( src->lightmapUV[0] );
			dst->lightmap[1]  = LittleFloat( src->lightmapUV[1] );
			dst->normal[0]    = LittleFloat( src->normal[0] );
			dst->normal[1]    = LittleFloat( src->normal[1] );
			dst->normal[2]    = LittleFloat( src->normal[2] );
			R_ColorShiftLightingBytes( (byte *)src->color, dst->color );

			AddPointToBounds( dst->xyz, tri->bounds[0], tri->bounds[1] );
		}

		/* Copy indices (u16 → int); they are LOCAL (relative to verts_off) */
		for ( j = 0; j < tris_len; j++ ) {
			tri->indexes[j] = (int)LittleShort( tris_base[tris_off + j] );
		}
	}
}

/* -------------------------------------------------------------------------
   Marksurfaces – lump 13 contains int32 TriangleSoup indices.
   Each entry maps a "leaf surface slot" to a surface in s_worldData.surfaces.
   ------------------------------------------------------------------------- */
static void R_LoadCod1Marksurfaces( const byte *base ) {
	lump_t      l = R_GetCod1Lump( base, COD1_LUMP_LEAFSURFACES );
	const int  *in;
	msurface_t **out;
	int          i, count, idx;

	in    = (const int *)( base + l.fileofs );
	count = l.filelen / sizeof( int );

	out = ri.Hunk_Alloc( count * sizeof( *out ), h_low );
	s_worldData.marksurfaces    = out;
	s_worldData.nummarksurfaces = count;

	for ( i = 0; i < count; i++ ) {
		idx = LittleLong( in[i] );
		if ( idx < 0 || idx >= s_worldData.numsurfaces )
			ri.Error( ERR_DROP, "R_LoadCod1Marksurfaces: bad surface index %d", idx );
		out[i] = &s_worldData.surfaces[idx];
	}
}

/* -------------------------------------------------------------------------
   BSP nodes + leafs

   Nodes: 36-byte Q3 format, identical to Q3 dnode_t.
   Leafs: 36-byte CoD1 format (cod1_dleaf_t), no per-leaf bounding box.
   ------------------------------------------------------------------------- */
static void R_LoadCod1NodesAndLeafs( const byte *base ) {
	lump_t       node_l = R_GetCod1Lump( base, COD1_LUMP_BSPNODES );
	lump_t       leaf_l = R_GetCod1Lump( base, COD1_LUMP_BSPLEAFS );
	const dnode_t    *node_in;
	const cod1_dleaf_t *leaf_in;
	mnode_t      *out;
	int           num_nodes, num_leafs, i, j, p;

	if ( node_l.filelen % sizeof( dnode_t ) )
		ri.Error( ERR_DROP, "R_LoadCod1NodesAndLeafs: bad node lump" );
	if ( leaf_l.filelen % sizeof( cod1_dleaf_t ) )
		ri.Error( ERR_DROP, "R_LoadCod1NodesAndLeafs: bad leaf lump" );

	num_nodes = node_l.filelen / sizeof( dnode_t );
	num_leafs = leaf_l.filelen / sizeof( cod1_dleaf_t );

	out = ri.Hunk_Alloc( ( num_nodes + num_leafs ) * sizeof( *out ), h_low );
	s_worldData.nodes            = out;
	s_worldData.numnodes         = num_nodes + num_leafs;
	s_worldData.numDecisionNodes = num_nodes;

	node_in = (const dnode_t *)( base + node_l.fileofs );

	/* Load nodes */
	for ( i = 0; i < num_nodes; i++, node_in++, out++ ) {
		for ( j = 0; j < 3; j++ ) {
			out->mins[j] = LittleLong( node_in->mins[j] );
			out->maxs[j] = LittleLong( node_in->maxs[j] );
		}
		p         = LittleLong( node_in->planeNum );
		out->plane = s_worldData.planes + p;
		out->contents = CONTENTS_NODE;

		for ( j = 0; j < 2; j++ ) {
			p = LittleLong( node_in->children[j] );
			if ( p >= 0 )
				out->children[j] = s_worldData.nodes + p;
			else
				out->children[j] = s_worldData.nodes + num_nodes + ( -1 - p );
		}
	}

	/* Load leafs */
	leaf_in = (const cod1_dleaf_t *)( base + leaf_l.fileofs );
	for ( i = 0; i < num_leafs; i++, leaf_in++, out++ ) {
		/* CoD1 leafs have no bounding box; inherit from parent node */
		out->mins[0] = out->mins[1] = out->mins[2] = -MAX_WORLD_COORD;
		out->maxs[0] = out->maxs[1] = out->maxs[2] =  MAX_WORLD_COORD;

		out->cluster = LittleLong( leaf_in->cluster );
		out->area    = LittleLong( leaf_in->area );

		if ( out->cluster >= s_worldData.numClusters )
			s_worldData.numClusters = out->cluster + 1;

		out->firstmarksurface = s_worldData.marksurfaces +
			LittleLong( leaf_in->firstLeafSurface );
		out->nummarksurfaces = LittleLong( leaf_in->numLeafSurfaces );
	}

	/* Link tree */
	R_SetParent( s_worldData.nodes, NULL );
}

/* -------------------------------------------------------------------------
   Visibility – CoD1 vis format is not yet fully understood.
   For now mark everything visible (all clusters see each other).
   ------------------------------------------------------------------------- */
static void R_LoadVisibilityCod1( const byte *base ) {
	int len;

	/* numClusters set by R_LoadCod1NodesAndLeafs */
	len = ( s_worldData.numClusters + 63 ) & ~63;
	s_worldData.novis = ri.Hunk_Alloc( len, h_low );
	Com_Memset( s_worldData.novis, 0xff, len );

	/* For now, all clusters visible from all clusters */
	s_worldData.numClusters  = s_worldData.numClusters  ? s_worldData.numClusters  : 1;
	s_worldData.clusterBytes = ( s_worldData.numClusters + 7 ) & ~7;

	if ( tr.externalVisData ) {
		s_worldData.vis = tr.externalVisData;
	} else {
		byte *vis = ri.Hunk_Alloc( s_worldData.numClusters * s_worldData.clusterBytes, h_low );
		Com_Memset( vis, 0xff, s_worldData.numClusters * s_worldData.clusterBytes );
		s_worldData.vis = vis;
	}
}

/* -------------------------------------------------------------------------
   Entities – plain text lump, same as Q3 (lump 29).
   ------------------------------------------------------------------------- */
static void R_LoadEntitiesCod1( const byte *base ) {
	lump_t l = R_GetCod1Lump( base, COD1_LUMP_ENTITIES );
	fileBase = (byte *)base;
	R_LoadEntities( &l );
}

/* -------------------------------------------------------------------------
   Submodels – create one bmodel per cod1_dmodel_t entry.
   ------------------------------------------------------------------------- */
static void R_LoadSubmodelsCod1( const byte *base ) {
	lump_t         l = R_GetCod1Lump( base, COD1_LUMP_MODELS );
	cod1_dmodel_t *in;
	bmodel_t      *out;
	int            i, j, count;

	if ( l.filelen == 0 ) {
		/* No models lump: create a single world model */
		s_worldData.bmodels = ri.Hunk_Alloc( sizeof( bmodel_t ), h_low );
		model_t *model = R_AllocModel();
		if ( model ) {
			model->type   = MOD_BRUSH;
			model->bmodel = s_worldData.bmodels;
			Com_sprintf( model->name, sizeof( model->name ), "*0" );
			for ( j = 0; j < 3; j++ ) {
				s_worldData.bmodels[0].bounds[0][j] = -MAX_WORLD_COORD;
				s_worldData.bmodels[0].bounds[1][j] =  MAX_WORLD_COORD;
			}
			s_worldData.bmodels[0].firstSurface = s_worldData.surfaces;
			s_worldData.bmodels[0].numSurfaces  = 0;
		}
		return;
	}

	if ( l.filelen % sizeof( cod1_dmodel_t ) )
		ri.Error( ERR_DROP, "R_LoadSubmodelsCod1: funny lump size" );

	count = l.filelen / sizeof( cod1_dmodel_t );
	in    = (cod1_dmodel_t *)( base + l.fileofs );
	out   = ri.Hunk_Alloc( count * sizeof( *out ), h_low );
	s_worldData.bmodels = out;

	for ( i = 0; i < count; i++, in++, out++ ) {
		model_t *model = R_AllocModel();
		if ( !model )
			ri.Error( ERR_DROP, "R_LoadSubmodelsCod1: R_AllocModel() failed" );
		model->type   = MOD_BRUSH;
		model->bmodel = out;
		Com_sprintf( model->name, sizeof( model->name ), "*%d", i );

		for ( j = 0; j < 3; j++ ) {
			out->bounds[0][j] = LittleFloat( in->mins[j] );
			out->bounds[1][j] = LittleFloat( in->maxs[j] );
		}

		int firstSurf = LittleLong( in->firstSurface );
		int numSurfs  = LittleLong( in->numSurfaces  );
		if ( firstSurf >= 0 && firstSurf + numSurfs <= s_worldData.numsurfaces ) {
			out->firstSurface = s_worldData.surfaces + firstSurf;
			out->numSurfaces  = numSurfs;
		} else {
			out->firstSurface = s_worldData.surfaces;
			out->numSurfaces  = 0;
		}
	}
}

/* =========================================================================
   R_LoadCod1WorldMap – main entry point called from RE_LoadWorldMap.
   ========================================================================= */
void R_LoadCod1WorldMap( const byte *base ) {
	byte *startMarker;

	startMarker = ri.Hunk_Alloc( 0, h_low );

	fileBase = (byte *)base;

	ri.Printf( PRINT_ALL, "Loading CoD1 IBSP v59 map...\n" );

	R_LoadShadersCod1    ( base );   /* lump 0  – materials    */
	R_LoadLightmapsCod1  ( base );   /* lump 1  – lightmaps    */
	R_LoadPlanesCod1     ( base );   /* lump 2  – planes       */
	/* Fogs: none in CoD1 */
	s_worldData.fogs    = ri.Hunk_Alloc( sizeof( *s_worldData.fogs ), h_low );
	s_worldData.numfogs = 0;
	R_LoadCod1Surfaces   ( base );   /* lumps 6/7/8 – geometry */
	R_LoadCod1Marksurfaces( base );  /* lump 13 – leaf-surface indices */
	R_LoadCod1NodesAndLeafs( base ); /* lumps 20/21 – BSP tree */
	R_LoadSubmodelsCod1  ( base );   /* lump 27 – submodels    */
	R_LoadVisibilityCod1 ( base );   /* lump 26 – vis (stub)   */
	R_LoadEntitiesCod1   ( base );   /* lump 29 – entities     */

	s_worldData.dataSize = (byte *)ri.Hunk_Alloc( 0, h_low ) - startMarker;
}
