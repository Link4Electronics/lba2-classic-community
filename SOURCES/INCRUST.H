#ifndef INCRUST_H
#define INCRUST_H

/*--------------------------------------------------------------------------*/
extern S32 InitIncrustDisp(S16 type, S16 num, S32 x, S32 y, S16 info, S16 move, S32 timeout);
/*--------------------------------------------------------------------------*/
extern void PosObjIncrust(T_INCRUST_DISP *ptrdisp, S32 num);
/*--------------------------------------------------------------------------*/
extern void ChangeIncrustPos(S32 xp0, S32 yp0, S32 xp1, S32 yp1);
/*--------------------------------------------------------------------------*/
extern void ClearIncrusts(S32 type);
/*--------------------------------------------------------------------------*/
extern void DrawBulle(S32 numobj);
/*--------------------------------------------------------------------------*/
/* GpuScene HUD hooks used by the AffScene incrust loop and DrawBulle. Both
   return TRUE when the quad landed in the FBO (Log's footprint punched);
   FALSE leaves the caller on Font / PtrAffGraph. */
extern S32 TryGpuIncrustText(S32 x, S32 y, const char *str);
extern S32 TryGpuIncrustSprite(S32 xp, S32 yp, S32 sprite);
/*--------------------------------------------------------------------------*/

#endif // INCRUST_H
