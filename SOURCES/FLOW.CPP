#include "C_EXTERN.H"
#include "DIRECTORIES.H"

#include <GPU/GPURENDERER.H> // GpuRenderer_DrawFlowDot
#include <GL/RENDER_GL.H>    // GpuScene
#include <POLYGON/POLY.H>    // GET_ZO

/*══════════════════════════════════════════════════════════════════════════*
	     █▀▀▀█ █▀▀▀█ █▀▀▀█ ▀▀█▀▀  █    █▀▀▀▀ █     █▀▀▀▀ ██▀▀▀
	     ██▀▀▀ ██▀▀█ ██▀█▀   ██   ██   ██    ██    ██▀▀  ▀▀▀▀█
	     ▀▀    ▀▀  ▀ ▀▀  ▀   ▀▀   ▀▀   ▀▀▀▀▀ ▀▀▀▀▀ ▀▀▀▀▀ ▀▀▀▀▀
 *══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/

// type de point
#define DOT_DEAD 0    // point mort, on ne le gere plus
#define DOT_DISPLAY 1 // point vivant et visible
#define DOT_WAIT 2    // point vivant mais attendant d'etre lancé

#ifdef LBA_EDITOR
extern S32 FlagInfos;

#define MAX_PART_FLOW 100
#else
//T_HQR_HEADER	*HQR_Flows ;
#endif

T_FLOW *TabPartFlow;

S_PART_FLOW ListPartFlow[MAX_FLOWS];
S_ONE_DOT *ListFlowDots;

//▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀
void RazListPartFlow() {
    S_PART_FLOW *ptrf = ListPartFlow;
    S32 n;

    for (n = 0; n < MAX_FLOWS; n++, ptrf++) {
        ptrf->NbDot = 0;
    }
}

//▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀
U32 InitPartFlow() {
    S_ONE_DOT *ptrd;
    S_PART_FLOW *ptrf = ListPartFlow;
    S32 n;
    char tmpFilePath[ADELINE_MAX_PATH];

#ifdef LBA_EDITOR
    char filename[ADELINE_MAX_PATH];
    S32 handle;
    S32 flag = FALSE;
    T_FLOW *flow;
    char *ptr;
    char *com; // pointeur sur le commentaire

    Message("Chargement des flots de pixels...", FALSE);
    BoxUpdate();

    TabPartFlow = (T_FLOW *)Malloc(sizeof(T_FLOW) * MAX_PART_FLOW);
    if (!TabPartFlow)
        return (FALSE);

    flow = TabPartFlow;

    n = 0;

    while (!flag AND n < MAX_PART_FLOW) {
        strcpy(filename, PATH_FLOW);

        InitDefFile(PATH_FLOW "flow.def");
        ptr = DefFileBufferReadString(Itoa(n));

        if (ptr) {
            // Vire commentaire
            com = (char *)strchr(ptr, ';');
            if (com)
                *com = '\0';

            strcat(filename, ptr);
            AddExt(filename, ".FLW");

            handle = OpenRead(filename);

            if (handle) {
                Read(handle, flow, sizeof(T_FLOW));
                Close(handle);
                flow->Bank *= 16;

                // protec par rapport à l'outil
                if (flow->NbDot > MAX_FLOW_DOTS) {
                    flow->NbDot = MAX_FLOW_DOTS;
                    Beep();
                    MessageF(TRUE, "Warning: Flow %s : NbDots>%d", filename, MAX_FLOW_DOTS);
                }
            } else {
                strcat(filename, " introuvable (InitPartFlow()).");
                Message(filename, TRUE);
            }

            n++;
            flow++;
        } else
            flag = TRUE;
    }

    if (n >= MAX_PART_FLOW) {
        Message("Warning: Tableau de flows trop petit !", TRUE);
    }

    Cls(); // Efface message
    BoxUpdate();
#else
    GetResPath(tmpFilePath, ADELINE_MAX_PATH, RESS_HQR_NAME);
    Load_HQR(tmpFilePath, TabPartFlow, RESS_FLOW);
#endif

#ifdef LBA_EDITOR
    ListFlowDots = (S_ONE_DOT *)Malloc(sizeof(S_ONE_DOT) * MAX_FLOW_DOTS * MAX_FLOWS);
    if (!ListFlowDots)
        return FALSE;
#endif

    ptrd = ListFlowDots;

    for (n = 0; n < MAX_FLOWS; n++, ptrf++) {
        ptrf->PtrListDot = ptrd;
        ptrd += MAX_FLOW_DOTS;
        ptrf->NbDot = 0;
    }

    return (TRUE);
}

//▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀
#ifdef LBA_EDITOR
void FreePartFlow() {
    if (TabPartFlow)
        Free(TabPartFlow);

    if (ListFlowDots) {
        Free(ListFlowDots);
        ListFlowDots = NULL;
    }
}
#endif

//▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀

#define LoadPartFlow(num_flow) (TabPartFlow + num_flow)

//▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀
S_PART_FLOW *GetFirstFlowDispo(void) {
    S32 n;
    S_PART_FLOW *ptrf = ListPartFlow;
    // cherche slot

    for (n = 0; n < MAX_FLOWS; n++, ptrf++) {
        if (ptrf->NbDot == 0)
            return ptrf;
    }

    return NULL;
}

//▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀
U32 CreateParticleFlow(S32 flag, S32 owner, S32 num_point,
                       S32 orgx, S32 orgy, S32 orgz,
                       S32 beta, S32 num_flow) {
    S32 n;
    S_PART_FLOW *ptrf;
    S_ONE_DOT *ptrd;
    T_FLOW *flow;
    S32 vitesse;
    S32 demi_speed;
    S32 demi_weight;
    S32 demi_ouvalpha;
    S32 demi_ouvbeta;
    S32 bank;

    // cherche slot
    ptrf = GetFirstFlowDispo();
    if (!ptrf)
        return (FALSE); // pas de place

    flow = LoadPartFlow(num_flow);
    if (!flow)
        return (FALSE);

    bank = flow->Bank;

#ifndef LBA_EDITOR // Dans l'outil, c'est fait dans InitPartFlow
    bank *= 16;

    // protec par rapport à l'outil makeflow
    if (flow->NbDot > MAX_FLOW_DOTS) {
        flow->NbDot = MAX_FLOW_DOTS;
    }
#endif

    if (flow->NbDot <= MAX_FLOW_DOTS) {
        ptrf->NbDot = flow->NbDot;
    } else {
        ptrf->NbDot = MAX_FLOW_DOTS;
    }

    ptrf->Flag = flag;

    // copie zv particule
    ptrf->XMin = flow->XMin;
    ptrf->YMin = flow->YMin;
    ptrf->ZMin = flow->ZMin;
    ptrf->XMax = flow->XMax;
    ptrf->YMax = flow->YMax;
    ptrf->ZMax = flow->ZMax;

    ptrf->FlowTimerStart = TimerRefHR;

    // est-ce un flow qui va attendre des coor
    if (!(flag & FLOW_WAIT_COOR)) {
        ptrf->OrgX = orgx;
        ptrf->OrgY = orgy;
        ptrf->OrgZ = orgz;
    } else {
        ptrf->Owner = owner;
        ptrf->NumPoint = num_point;
    }

    demi_speed = (flow->Speed >> 1);
    if (!demi_speed)
        demi_speed = 1;

    demi_weight = (flow->Weight >> 1);
    if (!demi_weight)
        demi_weight = 1;

    demi_ouvalpha = (flow->OuvertureAlpha >> 1);
    demi_ouvbeta = (flow->OuvertureBeta >> 1);

    beta += flow->Beta;

    ptrd = ptrf->PtrListDot;

    for (n = 0; n < ptrf->NbDot; n++, ptrd++) {
        if (!(flag & FLOW_WAIT_COOR)) {
            ptrd->X = orgx;
            ptrd->Y = orgy;
            ptrd->Z = orgz;
        }

        vitesse = (MyRnd(demi_speed) + flow->Speed);

        ptrd->Poids = MyRnd(demi_weight) + flow->Weight;

        Rotate(vitesse, 0, flow->Alpha + MyRnd(flow->OuvertureAlpha) - demi_ouvalpha);
        ptrd->Vy = -Z0;

        Rotate(0, X0, beta + MyRnd(flow->OuvertureBeta) - demi_ouvbeta);
        ptrd->Vx = X0;
        ptrd->Vz = Z0;

        if (flow->Delay)
            ptrd->Delay = MyRnd(flow->Delay);
        else
            ptrd->Delay = 0;
        ptrd->Couleur = bank + MyRnd(flow->Range) + flow->Coul;

        ptrd->Mode = DOT_WAIT;
    }

    return (TRUE);
}

//▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀
// Générateur d'extras à partir du moteur de flows

// ATTENTION:

//	FLOW_EXTRA_SPRITE: num et num2 correspondent à sprite_deb et sprite_fin

//	FLOW_EXTRA_OBJ: num correspond au body en mode Jeu, sinon le nom du
//			body doit se trouver dans FicName en mode Outil

//	FLOW_EXTRA_POF: num correspond au numéro de pof dans 'pof.def'
//                      num2 correspond à SpeedRot
//                      scale correspond à scale_deb
//                      transparent correspond à scale_fin

U32 CreateExtraParticleFlow(S32 type, S32 owner, S32 num, S32 num2,
                            S32 orgx, S32 orgy, S32 orgz,
                            S32 beta, S32 num_flow, S32 hitforce,
                            S32 scale, S32 transparent, S32 tempo) {
    T_FLOW *flow;
    S32 n;
    S32 vitesse;
    S32 demi_speed;
    S32 demi_weight;
    S32 demi_ouvalpha;
    S32 demi_ouvbeta;
    S32 num_extra;
    S32 nbdot;
    S32 poids;
    S32 delay;
    S32 speedrot;
    S32 shadow = 0; // yaz

    flow = LoadPartFlow(num_flow);
    if (!flow)
        return (FALSE);

    nbdot = flow->NbDot;

    if (nbdot > MAX_FLOW_EXTRA_DOTS) {
        nbdot = MAX_FLOW_EXTRA_DOTS;
    }

    demi_speed = (flow->Speed >> 1);
    if (!demi_speed)
        demi_speed = 1;

    demi_weight = (flow->Weight >> 1);
    if (!demi_weight)
        demi_weight = 1;

    demi_ouvalpha = (flow->OuvertureAlpha >> 1);
    demi_ouvbeta = (flow->OuvertureBeta >> 1);

    beta += flow->Beta;

    for (n = 0; n < nbdot; n++) {
        vitesse = MyRnd(demi_speed) + flow->Speed;

        poids = MyRnd(demi_weight) + flow->Weight;

        if (flow->Delay)
            delay = MyRnd(flow->Delay);
        else
            delay = 0;

        switch (type) {
        case FLOW_EXTRA_OBJ:
            num_extra = ThrowExtraObj((U8)owner, orgx, orgy, orgz,
#ifdef LBA_EDITOR
                                      FicName,
#else
                                      num,
#endif
                                      flow->Alpha + MyRnd(flow->OuvertureAlpha) - demi_ouvalpha,
                                      beta + MyRnd(flow->OuvertureBeta) - demi_ouvbeta,
                                      vitesse,
                                      -1, // alpharot suivant la trajectoire du throw
                                      (U8)poids, (U8)hitforce);

#ifdef LBA_EDITOR
            shadow = TRUE;
#else
            shadow = 2;
#endif
            break;

        case FLOW_EXTRA_SPRITE:
            if (num == num2) // Pas une anim
            {
                num_extra = ThrowExtra((U8)owner, orgx, orgy, orgz,
                                       (S16)num,
                                       flow->Alpha + MyRnd(flow->OuvertureAlpha) - demi_ouvalpha,
                                       beta + MyRnd(flow->OuvertureBeta) - demi_ouvbeta,
                                       vitesse, (U8)poids, (U8)hitforce);

                if (num_extra != -1) {
                    ListExtra[num_extra].Scale = scale;
                }
            } else // c'est une anim de sprites
            {
                num_extra = InitExtraAnimSprite((U8)owner, orgx, orgy, orgz,
                                                (S16)num, (S16)num2,
                                                tempo, scale,
                                                transparent, (U8)hitforce);

                if (num_extra != -1) {
                    InitFly(&ListExtra[num_extra],
                            flow->Alpha + MyRnd(flow->OuvertureAlpha) - demi_ouvalpha,
                            beta + MyRnd(flow->OuvertureBeta) - demi_ouvbeta,
                            vitesse, (U8)poids);
                }
            }

#ifdef LBA_EDITOR
            shadow = TRUE;
#else
            shadow = 3;
#endif
            break;

        case FLOW_EXTRA_POF:
            if (num2 == -1)
                speedrot = vitesse;
            else
                speedrot = num2;

            num_extra = InitExtraPof(orgx, orgy, orgz,
                                     (S16)num,
                                     flow->Alpha + MyRnd(flow->OuvertureAlpha) - demi_ouvalpha,
                                     beta + MyRnd(flow->OuvertureBeta) - demi_ouvbeta,
                                     vitesse, poids,
                                     scale, transparent, tempo,
                                     speedrot, 0);
            break;
        }

        // ATTENTION: les extras générés par le moteur de flow peuvent
        // également toucher leur propriétaire
        if (num_extra != -1) {
            ListExtra[num_extra].Flags |= (EXTRA_HIT_ANY | EXTRA_WAIT_NO_COL_OWNER);

            if (Shadow < shadow) {
                ListExtra[num_extra].Flags |= EXTRA_NO_SHADOW;
            }

            if (delay) {
                ListExtra[num_extra].Flags |= EXTRA_WAIT;
                ListExtra[num_extra].Timer = TimerRefHR + delay;
            }
        }
    }

    return (TRUE);
}

//▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀
/* GpuScene exterior: ZBufBoxOverWrite2's depth test is row-constant across
   the flow's bounding box — the clip the caller's DrawRecover gets after
   SetClip of the bounds this function publishes — so the dots cannot be
   stamped until every dot has projected. They collect here and draw after
   the loop with that interpolated depth; one entry per displayed dot. */
typedef struct {
    S32 x;
    S32 y;
    U8 coul;
} T_FLOW_GPUDOT;

static T_FLOW_GPUDOT *ListFlowGpuDots = NULL;
static S32 NbFlowGpuDotsCap = 0;

S32 AffParticleFlow(S_PART_FLOW *ptrf) {
    S32 n;
    S32 flag = FALSE;
    S_ONE_DOT *ptrd;
    S32 defer;
    S32 dotN = 0;

    defer = (GpuRenderer_IsActive() && GpuScene &&
             CubeMode == CUBE_EXTERIEUR);

    ClearScreenMinMax();
    ptrd = ptrf->PtrListDot;
    for (n = 0; n < ptrf->NbDot; n++, ptrd++) {
        if (ptrd->Mode == DOT_DISPLAY) {
            X0 = ptrd->X;
            Y0 = ptrd->Y;
            Z0 = ptrd->Z;

            LongWorldRotatePoint(X0, Y0, Z0);
            if (LongProjectPoint(X0, Y0, Z0)) {
                if (defer) {
                    if (dotN >= NbFlowGpuDotsCap) {
                        S32 newCap = NbFlowGpuDotsCap ? NbFlowGpuDotsCap * 2 : 64;
                        T_FLOW_GPUDOT *grown =
                            (T_FLOW_GPUDOT *)realloc(ListFlowGpuDots,
                                                     newCap * sizeof(T_FLOW_GPUDOT));
                        if (grown != NULL) {
                            ListFlowGpuDots = grown;
                            NbFlowGpuDotsCap = newCap;
                        }
                    }
                    if (dotN < NbFlowGpuDotsCap) {
                        ListFlowGpuDots[dotN].x = Xp;
                        ListFlowGpuDots[dotN].y = Yp;
                        ListFlowGpuDots[dotN].coul = (U8)ptrd->Couleur;
                        dotN++;
                    } else {
                        /* No buffer: software draw; the caller's recovery
                           re-applies terrain occlusion as it always did. */
                        BoxFlow(Xp, Yp, ptrd->Couleur);
                    }
                } else {
#ifdef LBA_EDITOR
                    if (!(FlagInfos & INFO_BOX_FLOW))
                        Plot(Xp, Yp, ptrd->Couleur);
                    else
#endif
                        if (!GpuRenderer_DrawFlowDot(Xp, Yp,
                                                     (U8)ptrd->Couleur, 0,
                                                     0)) {
                        BoxFlow(Xp, Yp, ptrd->Couleur);
                    }
                }

                if (Xp < ScreenXMin)
                    ScreenXMin = Xp;
                if (Xp + 1 > ScreenXMax)
                    ScreenXMax = Xp + 1;
                if (Yp < ScreenYMin)
                    ScreenYMin = Yp;
                if (Yp + 1 > ScreenYMax)
                    ScreenYMax = Yp + 1;
                flag = TRUE;
            }
        }
    }

    if (defer && flag && dotN > 0) {
        S32 z0;
        S32 z1;
        S32 scaledZ0;
        S32 scaledZ1;
        S32 incZ;
        S32 deltaY;
        S32 i;

        GetRecoverDepth(ptrf->OrgX, ptrf->OrgY, ptrf->OrgZ, ptrf->YMax,
                        ade_max((ptrf->XMax - ptrf->XMin) / 2,
                                (ptrf->ZMax - ptrf->ZMin) / 2),
                        &z0, &z1);

        deltaY = ScreenYMax - ScreenYMin + 1;
        scaledZ0 = Fill_ZBuffer_Factor * z0;
        scaledZ1 = Fill_ZBuffer_Factor * z1;
        incZ = (S32)((S64)(scaledZ0 - scaledZ1) / deltaY);

        for (i = 0; i < dotN; i++) {
            /* Row r's z0Int: same U32 accumulation and >>16 the software
               sweep does, so GEQUAL and the ZBuf test decide alike. */
            U32 r = (U32)(ListFlowGpuDots[i].y - ScreenYMin);
            U16 zoTop = (U16)(((U32)scaledZ1 + (U32)incZ * r) >> 16);
            U16 zoBot = (U16)(((U32)scaledZ1 + (U32)incZ * (r + 1)) >> 16);

            if (!GpuRenderer_DrawFlowDot(ListFlowGpuDots[i].x,
                                         ListFlowGpuDots[i].y,
                                         ListFlowGpuDots[i].coul, zoTop,
                                         zoBot)) {
                BoxFlow(ListFlowGpuDots[i].x, ListFlowGpuDots[i].y,
                        ListFlowGpuDots[i].coul);
            }
        }
    }

#ifdef LBA_EDITOR
    if (FlagInfos & INFO_DRAW_ZV) {
        S32 x, y, z;
        S32 xmin, ymin, zmin;
        S32 xmax, ymax, zmax;

        x = ptrf->OrgX;
        y = ptrf->OrgY;
        z = ptrf->OrgZ;

        xmin = x + ptrf->XMin;
        ymin = y + ptrf->YMin;
        zmin = z + ptrf->ZMin;
        xmax = x + ptrf->XMax;
        ymax = y + ptrf->YMax;
        zmax = z + ptrf->ZMax;

        DrawCube(xmin, ymin, zmin, xmax, ymax, zmax, 0);
        //		if( ClipXMin<0 )	ClipXMin = 0 ;
    } else if (FlagInfos & INFO_VISU_BOX) {
        Rect(ScreenXMin, ScreenYMin, ScreenXMax, ScreenYMax, RED);
    }

#endif

    return flag; // si 0 pas besoin aff
}

//▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀
// Anim particule sans l'afficher
S32 AnimParticleFlow(S_PART_FLOW *ptrf) {
    S32 n;
    S32 time;
    S32 timew;
    S32 flag = FALSE;
    S32 x, y, z;
    S32 xmin, ymin, zmin;
    S32 xmax, ymax, zmax;

    S_ONE_DOT *ptrd;

    time = TimerRefHR - ptrf->FlowTimerStart;

    x = ptrf->OrgX;
    y = ptrf->OrgY;
    z = ptrf->OrgZ;

    xmin = x + ptrf->XMin;
    ymin = y + ptrf->YMin;
    zmin = z + ptrf->ZMin;
    xmax = x + ptrf->XMax;
    ymax = y + ptrf->YMax;
    zmax = z + ptrf->ZMax;

    ptrd = ptrf->PtrListDot;
    for (n = 0; n < ptrf->NbDot; n++, ptrd++) {
        if (ptrd->Mode) // si point vivant
        {
            if (time >= ptrd->Delay) {
                timew = (time - ptrd->Delay) / 20;

                X0 = (ptrd->Vx * timew) + x;
                Y0 = (ptrd->Vy * timew) + y - (ptrd->Poids * timew * timew) / (20);
                Z0 = (ptrd->Vz * timew) + z;

                // test in ZV
                if ((X0 >= xmin)
                        AND(X0 <= xmax)
                            AND(Y0 >= ymin)
                                AND(Y0 <= ymax)
                                    AND(Z0 >= zmin)
                                        AND(Z0 <= zmax)) {
                    ptrd->X = X0;
                    ptrd->Y = Y0;
                    ptrd->Z = Z0;
                    ptrd->Mode = DOT_DISPLAY;

                    flag = TRUE;
                } else {
                    ptrd->Mode = DOT_DEAD; // end dot
                }
            } else {
                // si on attends le point le flow vie
                flag = TRUE;
            }
        }
    }

    if (!flag) {
        // au revoir ma jolie
        ptrf->NbDot = 0;
    }

    return flag; // si 0 pas besoin aff
}

//▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀
void AnimAllFlow() {
    S32 n;
    S_PART_FLOW *ptrf = ListPartFlow;

    for (n = 0; n < MAX_FLOWS; n++, ptrf++) {
        if (ptrf->NbDot > 0) {
            AnimParticleFlow(ptrf);
        }
    }
}
