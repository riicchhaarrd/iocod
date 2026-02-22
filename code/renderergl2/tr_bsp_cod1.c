/*
===========================================================================
tr_bsp_cod1.c -- CoD1 IBSP version 59 BSP loader for the GL2 renderer.

Adapted from renderergl1/tr_bsp_cod1.c to use GL2 types:
  - srfBspSurface_t / srfVert_t instead of srfTriangles_t / drawVert_t
  - marksurfaces stored as int* indices (not msurface_t** pointers)
  - bmodel_t uses int firstSurface/numSurfaces indices
  - Extra per-surface arrays required by GL2
===========================================================================
*/

#include "tr_local.h"

/* Access globals defined in tr_bsp.c */
extern world_t   s_worldData;
extern byte     *fileBase;

/* Functions in tr_bsp.c made non-static for our use */
void R_ColorShiftLightingBytes( byte in[4], byte out[4] );
void R_SetParent( mnode_t *node, mnode_t *parent );
void R_LoadEntities( lump_t *l );

/* -------------------------------------------------------------------------
   Helpers
   ------------------------------------------------------------------------- */

static lump_t R_GetCod1Lump( const byte *base, int idx ) {
	const cod1_dheader_t *hdr = (const cod1_dheader_t *)base;
	lump_t out;
	out.filelen = LittleLong( hdr->lumps[idx].filelen );
	out.fileofs = LittleLong( hdr->lumps[idx].fileofs );
	return out;
}

/* -------------------------------------------------------------------------
   Shaders / materials (identical to GL1 version)
   ------------------------------------------------------------------------- */
static void R_LoadShadersCod1( const byte *base ) {
	lump_t l = R_GetCod1Lump( base, COD1_LUMP_MATERIALS );
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

/* -------------------------------------------------------------------------
   Lightmaps (identical 128x128x3 format, identical to GL1 version)
   ------------------------------------------------------------------------- */
static void R_LoadLightmapsCod1( const byte *base ) {
	lump_t      l = R_GetCod1Lump( base, COD1_LUMP_LIGHTMAPS );
	byte       *buf, *buf_p;
	int         len, i, j;
	static byte image[128 * 128 * 4];
	byte        tmp[4];

	len = l.filelen;
	if ( !len )
		return;
	buf = (byte *)base + l.fileofs;

	R_IssuePendingRenderCommands();

	tr.numLightmaps = len / ( 128 * 128 * 3 );
	if ( tr.numLightmaps == 1 )
		tr.numLightmaps++;

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
   Planes (identical to GL1 version)
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
   Geometry – GL2 version using srfBspSurface_t / srfVert_t / glIndex_t
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

	/* GL2 requires these per-surface arrays */
	s_worldData.surfacesViewCount  = ri.Hunk_Alloc( num_ts * sizeof( *s_worldData.surfacesViewCount  ), h_low );
	s_worldData.surfacesDlightBits = ri.Hunk_Alloc( num_ts * sizeof( *s_worldData.surfacesDlightBits ), h_low );
	s_worldData.surfacesPshadowBits= ri.Hunk_Alloc( num_ts * sizeof( *s_worldData.surfacesPshadowBits), h_low );

	ri.Printf( PRINT_ALL, "...loading %d CoD1 triangle soups\n", num_ts );

	for ( i = 0; i < num_ts; i++ ) {
		msurface_t            *surf = &s_worldData.surfaces[i];
		srfBspSurface_t       *cv;
		const cod1_trianglesoup_t *ts = &ts_in[i];
		int   mat_idx   = LittleShort( ts->materialIdx );
		int   verts_off = LittleLong ( ts->vertsOffset );
		int   verts_len = LittleShort( ts->vertsLength );
		int   tris_off  = LittleLong ( ts->trisOffset  );
		int   tris_len  = LittleShort( ts->trisLength  );

		/* Assign shader */
		surf->cubemapIndex = 0;
		if ( mat_idx >= 0 && mat_idx < s_worldData.numShaders ) {
			dshader_t *dsh = &s_worldData.shaders[mat_idx];
			surf->shader = R_FindShader( dsh->shader, LIGHTMAP_WHITEIMAGE, qtrue );
		} else {
			surf->shader = tr.defaultShader;
		}
		if ( r_singleShader->integer && !surf->shader->isSky )
			surf->shader = tr.defaultShader;

		surf->fogIndex = 0;

		/* Allocate srfBspSurface_t + vertices + indices */
		cv = ri.Hunk_Alloc( sizeof( srfBspSurface_t ), h_low );
		cv->surfaceType = SF_TRIANGLES;
		cv->numVerts    = verts_len;
		cv->verts       = ri.Hunk_Alloc( verts_len * sizeof( srfVert_t ), h_low );
		cv->numIndexes  = tris_len;
		cv->indexes     = ri.Hunk_Alloc( tris_len * sizeof( glIndex_t ), h_low );

		surf->data = (surfaceType_t *)cv;

		/* Cull info */
		surf->cullinfo.type = CULLINFO_BOX;
		ClearBounds( surf->cullinfo.bounds[0], surf->cullinfo.bounds[1] );

		/* Copy vertices */
		for ( j = 0; j < verts_len; j++ ) {
			const cod1_vertex_t *src = &verts_base[verts_off + j];
			srfVert_t           *dst = &cv->verts[j];
			byte rbytes[4], shifted[4];
			vec3_t n;
			vec4_t c;

			dst->xyz[0] = LittleFloat( src->position[0] );
			dst->xyz[1] = LittleFloat( src->position[1] );
			dst->xyz[2] = LittleFloat( src->position[2] );
			AddPointToBounds( dst->xyz, surf->cullinfo.bounds[0], surf->cullinfo.bounds[1] );

			dst->st[0]      = LittleFloat( src->uv[0] );
			dst->st[1]      = LittleFloat( src->uv[1] );
			dst->lightmap[0]= LittleFloat( src->lightmapUV[0] );
			dst->lightmap[1]= LittleFloat( src->lightmapUV[1] );

			n[0] = LittleFloat( src->normal[0] );
			n[1] = LittleFloat( src->normal[1] );
			n[2] = LittleFloat( src->normal[2] );
			R_VaoPackNormal( dst->normal, n );

			/* tangent: zero for now, R_CalcTangentVectors will fill it */
			dst->tangent[0] = dst->tangent[1] = dst->tangent[2] = dst->tangent[3] = 0;

			rbytes[0] = src->color[0];
			rbytes[1] = src->color[1];
			rbytes[2] = src->color[2];
			rbytes[3] = src->color[3];
			R_ColorShiftLightingBytes( rbytes, shifted );
			c[0] = shifted[0] / 255.0f;
			c[1] = shifted[1] / 255.0f;
			c[2] = shifted[2] / 255.0f;
			c[3] = shifted[3] / 255.0f;
			R_VaoPackColor( dst->color, c );
		}

		/* Copy indices (u16 → glIndex_t) */
		for ( j = 0; j < tris_len; j++ ) {
			cv->indexes[j] = (glIndex_t)LittleShort( tris_base[tris_off + j] );
		}

		/* Calculate tangent vectors per triangle */
		{
			glIndex_t *tri;
			for ( j = 0, tri = cv->indexes; j < tris_len; j += 3, tri += 3 ) {
				srfVert_t *dv[3];
				dv[0] = &cv->verts[tri[0]];
				dv[1] = &cv->verts[tri[1]];
				dv[2] = &cv->verts[tri[2]];
				R_CalcTangentVectors( dv );
			}
		}
	}
}

/* -------------------------------------------------------------------------
   Marksurfaces – GL2 stores int indices (not msurface_t* pointers)
   ------------------------------------------------------------------------- */
static void R_LoadCod1Marksurfaces( const byte *base ) {
	int        *out;
	int         i, count;

	/* CoD1 leaves have cell indices, invalid in Q3.
	   Fix: generate a flat list 0..numsurfaces-1 and make all leaves visible. */
	count = s_worldData.numsurfaces;
	out = ri.Hunk_Alloc( count * sizeof( *out ), h_low );

	s_worldData.marksurfaces    = out;
	s_worldData.nummarksurfaces = count;

	for ( i = 0; i < count; i++ ) {
		out[i] = i;
	}
}

/* -------------------------------------------------------------------------
   BSP nodes + leafs – GL2 mnode_t uses int firstmarksurface/nummarksurfaces
   ------------------------------------------------------------------------- */
static void R_LoadCod1NodesAndLeafs( const byte *base ) {
	lump_t       node_l = R_GetCod1Lump( base, COD1_LUMP_BSPNODES );
	lump_t       leaf_l = R_GetCod1Lump( base, COD1_LUMP_BSPLEAFS );
	const dnode_t      *node_in;
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
		out->mins[0] = out->mins[1] = out->mins[2] = -MAX_WORLD_COORD;
		out->maxs[0] = out->maxs[1] = out->maxs[2] =  MAX_WORLD_COORD;

		out->cluster = LittleLong( leaf_in->cluster );
		out->area    = LittleLong( leaf_in->area );

		if ( out->cluster >= s_worldData.numClusters )
			s_worldData.numClusters = out->cluster + 1;

		/* GL2: make all surfaces visible from every leaf to avoid CoD1 index issues */
		out->firstmarksurface = 0;
		out->nummarksurfaces  = s_worldData.numsurfaces;
	}

	R_SetParent( s_worldData.nodes, NULL );
}

/* -------------------------------------------------------------------------
   Visibility stub (identical to GL1 version)
   ------------------------------------------------------------------------- */
static void R_LoadVisibilityCod1( const byte *base ) {
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
   Entities (identical to GL1 version)
   ------------------------------------------------------------------------- */
static void R_LoadEntitiesCod1( const byte *base ) {
	lump_t l = R_GetCod1Lump( base, COD1_LUMP_ENTITIES );
	fileBase = (byte *)base;
	R_LoadEntities( &l );
}

/* -------------------------------------------------------------------------
   Submodels – GL2 bmodel_t uses int firstSurface/numSurfaces indices
   ------------------------------------------------------------------------- */
static void R_LoadSubmodelsCod1( const byte *base ) {
	lump_t         l = R_GetCod1Lump( base, COD1_LUMP_MODELS );
	cod1_dmodel_t *in;
	bmodel_t      *out;
	int            i, j, count;

	if ( l.filelen == 0 ) {
		/* No models lump: create a single world model */
		s_worldData.numBModels = 1;
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
			s_worldData.bmodels[0].firstSurface = 0;
			s_worldData.bmodels[0].numSurfaces  = 0;
			s_worldData.numWorldSurfaces = 0;
		}
		return;
	}

	if ( l.filelen % sizeof( cod1_dmodel_t ) )
		ri.Error( ERR_DROP, "R_LoadSubmodelsCod1: funny lump size" );

	count = l.filelen / sizeof( cod1_dmodel_t );
	in    = (cod1_dmodel_t *)( base + l.fileofs );
	s_worldData.numBModels = count;
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
			out->firstSurface = firstSurf;
			out->numSurfaces  = numSurfs;
		} else {
			out->firstSurface = 0;
			out->numSurfaces  = 0;
		}

		if ( i == 0 ) {
			/* GL2 uses numWorldSurfaces to limit VAO surface creation */
			s_worldData.numWorldSurfaces = out->numSurfaces;
		}
	}
}

/* =========================================================================
   R_LoadCod1WorldMap – main entry point called from RE_LoadWorldMap.
   ========================================================================= */
void R_LoadCod1WorldMap( const byte *base ) {
	ri.Printf( PRINT_ALL, "Loading CoD1 IBSP v59 map (GL2)...\n" );

	fileBase = (byte *)base;

	R_LoadShadersCod1    ( base );
	R_LoadLightmapsCod1  ( base );
	R_LoadPlanesCod1     ( base );

	s_worldData.fogs    = ri.Hunk_Alloc( sizeof( *s_worldData.fogs ), h_low );
	s_worldData.numfogs = 0;

	R_LoadCod1Surfaces   ( base );
	R_LoadCod1Marksurfaces( base );
	R_LoadCod1NodesAndLeafs( base );
	R_LoadSubmodelsCod1  ( base );
	R_LoadVisibilityCod1 ( base );
	R_LoadEntitiesCod1   ( base );
}
