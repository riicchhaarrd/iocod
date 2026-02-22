/*
===========================================================================
CoD1 localization support — loads .str files from localizedstrings/english/
and resolves @REFERENCE strings used in menu definitions.
===========================================================================
*/

#include "ui_local.h"

/* Open-addressing hash table (linear probing).
   Strings are stored in a shared pool to keep per-entry overhead small. */

#define LOC_HASH_SIZE   8192            /* must be power of 2 */
#define MAX_LOC_ENTRIES 5500
#define LOC_POOL_SIZE   (512 * 1024)    /* 512 KB for all key+value strings */

static unsigned short loc_hash[LOC_HASH_SIZE]; /* 0=empty, else entry index+1 */
static int   loc_key_ofs[MAX_LOC_ENTRIES];
static int   loc_val_ofs[MAX_LOC_ENTRIES];
static char  loc_pool[LOC_POOL_SIZE];
static int   loc_pool_used;
static int   loc_count;

/* ------------------------------------------------------------------ */

static unsigned int Loc_HashStr(const char *s) {
	unsigned int h = 5381;
	int c;
	while ((c = (unsigned char)*s++) != 0) {
		if (c >= 'a' && c <= 'z') c -= 32; /* to upper */
		h = ((h << 5) + h) ^ c;
	}
	return h & (LOC_HASH_SIZE - 1);
}

static int Loc_PoolAdd(const char *s, int len) {
	int ofs = loc_pool_used;
	if (ofs + len + 1 > LOC_POOL_SIZE) return -1;
	Com_Memcpy(loc_pool + ofs, s, len);
	loc_pool[ofs + len] = '\0';
	loc_pool_used += len + 1;
	return ofs;
}

static void Loc_Store(const char *key, int keyLen, const char *val, int valLen) {
	unsigned int h;
	int slot, i, ko, vo;

	if (loc_count >= MAX_LOC_ENTRIES) return;

	h = Loc_HashStr(key);

	/* linear probe to find empty or matching slot */
	for (i = 0; i < LOC_HASH_SIZE; i++) {
		slot = (h + i) & (LOC_HASH_SIZE - 1);
		if (loc_hash[slot] == 0) break;                         /* empty */
		if (Q_stricmp(loc_pool + loc_key_ofs[loc_hash[slot]-1], key) == 0) {
			/* already stored — update value */
			vo = Loc_PoolAdd(val, valLen);
			if (vo >= 0) loc_val_ofs[loc_hash[slot]-1] = vo;
			return;
		}
	}
	if (i == LOC_HASH_SIZE) return; /* table full */

	ko = Loc_PoolAdd(key, keyLen);
	vo = Loc_PoolAdd(val, valLen);
	if (ko < 0 || vo < 0) return;

	loc_key_ofs[loc_count] = ko;
	loc_val_ofs[loc_count] = vo;
	loc_hash[slot] = (unsigned short)(loc_count + 1);
	loc_count++;
}

/* ------------------------------------------------------------------ */
/* Minimal tokenizer: returns next token from *pp (quoted or bare word).
   Skips whitespace and // line comments.  Returns qfalse at end.     */

static qboolean NextToken(const char **pp, const char *end, char *out, int outSize) {
	const char *p = *pp;
	int i;

	for (;;) {
		while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
		if (p >= end) { *pp = p; return qfalse; }
		if (p + 1 < end && p[0] == '/' && p[1] == '/') {
			while (p < end && *p != '\n') p++;
			continue;
		}
		if (p + 1 < end && p[0] == '/' && p[1] == '*') {
			p += 2;
			while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) p++;
			if (p + 1 < end) p += 2;
			continue;
		}
		break;
	}

	i = 0;
	if (*p == '"') {
		p++;
		while (p < end && *p != '"') {
			if (i < outSize - 1) out[i++] = *p;
			p++;
		}
		if (p < end) p++; /* closing quote */
	} else {
		while (p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
			if (i < outSize - 1) out[i++] = *p;
			p++;
		}
	}
	out[i] = '\0';
	*pp = p;
	return (i > 0);
}

static void ParseStrBuf(const char *buf, int len) {
	const char *p   = buf;
	const char *end = buf + len;
	char token[256];
	char key[64];
	key[0] = '\0';

	while (NextToken(&p, end, token, sizeof(token))) {
		if (Q_stricmp(token, "REFERENCE") == 0) {
			/* next bare token is the key */
			if (NextToken(&p, end, key, sizeof(key))) {
				/* wait for LANG_ENGLISH */
			}
		} else if (Q_stricmp(token, "LANG_ENGLISH") == 0) {
			char val[512];
			if (NextToken(&p, end, val, sizeof(val)) && key[0]) {
				Loc_Store(key, strlen(key), val, strlen(val));
				key[0] = '\0';
			}
		}
		/* all other tokens (LANG_FRENCH etc.) are skipped together with
		   their value by the outer loop's next NextToken call            */
	}
}

/* ------------------------------------------------------------------ */

#define LOC_FILE_BUF (256 * 1024)
static char loc_filebuf[LOC_FILE_BUF];

void Localize_Init(void) {
	char filelist[4096];
	int  count, i;
	const char *fp;

	Com_Memset(loc_hash,    0, sizeof(loc_hash));
	Com_Memset(loc_key_ofs, 0, sizeof(loc_key_ofs));
	Com_Memset(loc_val_ofs, 0, sizeof(loc_val_ofs));
	loc_pool_used = 0;
	loc_count     = 0;

	count = trap_FS_GetFileList("localizedstrings/english", ".str",
	                            filelist, sizeof(filelist));

	fp = filelist;
	for (i = 0; i < count; i++, fp += strlen(fp) + 1) {
		fileHandle_t f;
		char path[MAX_QPATH];
		int  flen;

		Com_sprintf(path, sizeof(path), "localizedstrings/english/%s", fp);
		flen = trap_FS_FOpenFile(path, &f, FS_READ);
		if (!f) continue;
		if (flen >= LOC_FILE_BUF) {
			trap_Print(va(S_COLOR_YELLOW "Localize_Init: %s too large (%i bytes)\n", path, flen));
			trap_FS_FCloseFile(f);
			continue;
		}
		trap_FS_Read(loc_filebuf, flen, f);
		loc_filebuf[flen] = '\0';
		trap_FS_FCloseFile(f);
		ParseStrBuf(loc_filebuf, flen);
	}

	trap_Print(va("Localize_Init: loaded %i strings from %i files\n", loc_count, count));
}

/* ------------------------------------------------------------------ */

static const char *Loc_Lookup(const char *key) {
	unsigned int h = Loc_HashStr(key);
	int i, slot;
	for (i = 0; i < LOC_HASH_SIZE; i++) {
		slot = (h + i) & (LOC_HASH_SIZE - 1);
		if (loc_hash[slot] == 0) break;
		if (Q_stricmp(loc_pool + loc_key_ofs[loc_hash[slot]-1], key) == 0)
			return loc_pool + loc_val_ofs[loc_hash[slot]-1];
	}
	return NULL;
}

const char *Localize_GetString(const char *key) {
	const char *val;
	const char *underscore;

	if (!key || !key[0]) return key;

	/* Try the key as-is first */
	val = Loc_Lookup(key);
	if (val) return val;

	/* CoD1 menu refs use a category prefix (e.g. MENU_BACKTOGAME).
	   The .str files store bare keys (BACKTOGAME).
	   Strip everything up to and including the first '_' and retry. */
	underscore = strchr(key, '_');
	if (underscore && underscore[1]) {
		val = Loc_Lookup(underscore + 1);
		if (val) return val;
	}

	return key; /* not found — return raw key as fallback */
}
