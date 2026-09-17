#ifndef ANIMTEX_H
#define ANIMTEX_H

/*--------------------------------------------------------------------------*/
//				fonctions ASM

#ifdef __cplusplus
extern "C" {
#endif

/* GPU renderer signals, both defined once in LIB386/GL/RENDER_GL.CPP with C
   linkage. AtlasTextureDirty is raised here whenever DoTextureAnimation bakes
   page content in place (the GPU must re-upload the atlas page); DisplayTexFilter
   is the bilinear sampling toggle the Display settings table and menu own. */
extern S32 AtlasTextureDirty;
extern S32 DisplayTexFilter;

#ifdef __cplusplus
}
#endif

/*--------------------------------------------------------------------------*/
//				fonctions C (ANIMTEX.CPP)

extern S32 NbTexEffects;

/*--------------------------------------------------------------------------*/
extern void ScanTextureAnimation(U8 *ptrtex, S32 nbscanlines);
/*--------------------------------------------------------------------------*/
extern void InitTextureAnimation(void);
/*--------------------------------------------------------------------------*/
extern void DoTextureAnimation();
/*--------------------------------------------------------------------------*/
extern void InitPlasmaMenu();
/*--------------------------------------------------------------------------*/
extern void SelectPlasmaBank(U8 bank);
/*--------------------------------------------------------------------------*/
extern void ClearPlasmaMenu(void);
/*--------------------------------------------------------------------------*/
extern void FixePlasmaPtrMap(void);
/*--------------------------------------------------------------------------*/

#endif // ANIMTEX_H
