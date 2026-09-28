/*
 * test_polyrec_points.cpp — Host test for the .lba2polyrec vertex record layout.
 *
 * On disk a FILL_POLY event stores exactly POLYREC_POINT_SIZE (16) bytes per
 * vertex: the leading bytes of Struc_Point (Pt_XE..Pt_W). In memory
 * Struc_Point is wider (Pt_RepMask follows), so both the recorder and the
 * replay reader must convert one vertex at a time. Serializing or parsing the
 * vertex array as a flat byte block makes vertex N's record start inside
 * vertex N-1's trailing bytes, shifting every field after vertex 0.
 *
 * This test links the real recorder (LIB386/SNAPSHOT/POLY_RECORDING.CPP) and
 * checks both seams:
 *   - polyrec_pack_point / polyrec_unpack_point round-trip (replay side)
 *   - a recorded file's FILL_POLY payload, parsed per vertex (writer side)
 */

#include <SNAPSHOT/POLY_RECORDING.H>
#include <POLYGON/POLY.H>
#include <SVGA/SCREEN.H>
#include <SVGA/CLIP.H>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Engine globals referenced by POLY_RECORDING.CPP ───────────────────────── */

U32 ModeDesiredX = 640;
U32 ModeDesiredY = 480;
U32 ScreenPitch = 640;
void *Log = NULL; /* NULL: stop() then skips the reference framebuffer */
U8 Fill_Logical_Palette[256];
U32 Fill_Patch = 0;
S32 Fill_Z_Fog_Near = 0;
S32 Fill_Z_Fog_Far = 0;
S32 RepMask = 0;
S32 ClipXMin = 0;
S32 ClipYMin = 0;
S32 ClipXMax = 639;
S32 ClipYMax = 479;
PTR_U8 PtrCLUTGouraud = NULL;
PTR_U8 PtrCLUTFog = NULL;
PTR_U8 PtrTruePal = NULL;
PTR_U8 PtrMap = NULL;

/* ── Helpers ───────────────────────────────────────────────────────────────── */

#define CHECK(cond, msg)                            \
    do {                                            \
        if (!(cond)) {                              \
            fprintf(stderr, "FAIL: %s (line %d)\n", \
                    (msg), __LINE__);               \
            return 1;                               \
        }                                           \
    } while (0)

static void fill_point(Struc_Point *p, int k) {
    p->Pt_XE = (S16)(-1000 - k * 7);
    p->Pt_YE = (S16)(2000 + k * 13);
    p->Pt_MapU = (U16)(1000 + k);
    p->Pt_MapV = (U16)(2000 + k);
    p->Pt_Light = (U16)(300 + k);
    p->Pt_ZO = (U16)(40000 + k);
    p->Pt_W = 123456 + k * 1000000;
    p->Pt_RepMask = (U16)(0xBEEF ^ k);
}

/* All serialized fields equal (Pt_RepMask is checked separately). */
static int points_equal(const Struc_Point *a, const Struc_Point *b) {
    return a->Pt_XE == b->Pt_XE && a->Pt_YE == b->Pt_YE &&
           a->Pt_MapU == b->Pt_MapU && a->Pt_MapV == b->Pt_MapV &&
           a->Pt_Light == b->Pt_Light && a->Pt_ZO == b->Pt_ZO &&
           a->Pt_W == b->Pt_W;
}

/* Walk one event's payload; returns bytes consumed, 0 on unknown type. */
static U32 event_payload_size(U8 type, const U8 *p) {
    S32 nb;
    switch (type) {
    case POLYREC_EVT_SWITCH_FILLERS:
    case POLYREC_EVT_SET_CLUT:
    case POLYREC_EVT_SET_TEXTURE:
    case POLYREC_EVT_SET_REPMASK:
    case POLYREC_EVT_SET_FILL_PATCH:
    case POLYREC_EVT_SET_CLUT_OFFSET:
        return 4;
    case POLYREC_EVT_SET_FOG:
        return 8;
    case POLYREC_EVT_SET_CLIP:
        return 16;
    case POLYREC_EVT_FILL_POLY:
        memcpy(&nb, p + 8, 4);
        if (nb < 0 || nb > 64)
            return 0;
        return 12 + (U32)nb * POLYREC_POINT_SIZE;
    case POLYREC_EVT_FILL_SPHERE:
        return 24;
    case POLYREC_EVT_LINE_A:
        return 28;
    case POLYREC_EVT_EOF:
        return 0;
    default:
        return 0;
    }
}

/* ── Tests ─────────────────────────────────────────────────────────────────── */

/* Shared helpers round-trip: payload slot k == vertex k, RepMask never stored. */
static int test_pack_unpack_roundtrip(void) {
    Struc_Point src[4], out[4];
    U8 buf[4 * POLYREC_POINT_SIZE];
    int k;

    for (k = 0; k < 4; k++)
        fill_point(&src[k], k);

    for (k = 0; k < 4; k++)
        polyrec_pack_point(buf + k * POLYREC_POINT_SIZE, &src[k]);

    /* Struc_Point is packed: its leading POLYREC_POINT_SIZE bytes are XE..W. */
    for (k = 0; k < 4; k++)
        CHECK(memcmp(buf + k * POLYREC_POINT_SIZE, &src[k],
                     POLYREC_POINT_SIZE) == 0,
              "pack slot does not match the vertex's leading bytes");

    /* Pt_RepMask must never reach the payload. */
    for (k = 0; k < 4; k++) {
        U16 mask = (U16)(0xBEEF ^ k);
        int j;
        for (j = 0; j + 1 < POLYREC_POINT_SIZE; j++) {
            U16 v = (U16)(buf[k * POLYREC_POINT_SIZE + j] |
                          (buf[k * POLYREC_POINT_SIZE + j + 1] << 8));
            CHECK(v != mask, "Pt_RepMask leaked into the payload");
        }
    }

    memset(out, 0xCD, sizeof(out));
    for (k = 0; k < 4; k++)
        polyrec_unpack_point(&out[k], buf + k * POLYREC_POINT_SIZE);

    for (k = 0; k < 4; k++) {
        CHECK(points_equal(&out[k], &src[k]), "unpack field mismatch");
        CHECK(out[k].Pt_RepMask == 0, "unpack must zero Pt_RepMask");
    }
    return 0;
}

/* End-to-end: record two polys, parse the file's FILL_POLY payloads per vertex. */
static int test_recorded_file_layout(void) {
    static const char *path = "test_polyrec_points.lba2polyrec";
    Struc_Point pts[4];
    FILE *f;
    U8 *file;
    long file_size;
    U32 off;
    int k, fills_seen = 0;
    T_POLYREC_FILE_HEADER hdr;
    int rc = 1;

    for (k = 0; k < 4; k++)
        fill_point(&pts[k], k);

    if (polyrec_start(path) != 0) {
        fprintf(stderr, "FAIL: polyrec_start failed\n");
        return 1;
    }
    polyrec_record_fill_poly(9, 0x11111111, 3, pts);
    polyrec_record_fill_poly(10, 0x22222222, 4, pts);
    if (polyrec_stop() != 0) {
        fprintf(stderr, "FAIL: polyrec_stop failed\n");
        return 1;
    }

    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "FAIL: recording file not written\n");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    file = (U8 *)malloc((size_t)file_size);
    if (!file || fread(file, 1, (size_t)file_size, f) != (size_t)file_size) {
        fprintf(stderr, "FAIL: short read of recording\n");
        goto done;
    }
    fclose(f);
    f = NULL;

    if (file_size < (long)sizeof(hdr)) {
        fprintf(stderr, "FAIL: file shorter than header\n");
        goto done;
    }
    memcpy(&hdr, file, sizeof(hdr));
    CHECK(memcmp(hdr.magic, POLYREC_MAGIC, POLYREC_MAGIC_SIZE) == 0,
          "bad magic");
    CHECK(hdr.version == POLYREC_VERSION, "bad version");
    CHECK(hdr.num_events == 3, "expected two FILL_POLY events plus EOF");

    /* Locate the EVNT section. */
    off = (U32)sizeof(hdr);
    while (off + sizeof(T_POLYREC_SECTION_HEADER) <= (U32)file_size) {
        T_POLYREC_SECTION_HEADER sh;
        memcpy(&sh, file + off, sizeof(sh));
        off += (U32)sizeof(sh);
        if (sh.type == POLYREC_SECTION_EVENTS) {
            U32 ev_end = off + sh.size;
            U32 ev_off;
            U32 stored_count;

            CHECK(sh.size >= 4, "EVNT section too small");
            memcpy(&stored_count, file + off, 4);
            ev_off = off + 4;

            while (ev_off < ev_end) {
                U8 type = file[ev_off];
                const U8 *payload = file + ev_off + 1;
                U32 payload_size = event_payload_size(type, payload);
                CHECK(payload_size > 0 || type == POLYREC_EVT_EOF,
                      "unknown or malformed event in EVNT");

                if (type == POLYREC_EVT_FILL_POLY) {
                    S32 type_poly, color_poly, nb_points;
                    memcpy(&type_poly, payload, 4);
                    memcpy(&color_poly, payload + 4, 4);
                    memcpy(&nb_points, payload + 8, 4);
                    CHECK(nb_points > 0 && nb_points <= 4, "unexpected nb_points");
                    CHECK(type_poly == (fills_seen == 0 ? 9 : 10) &&
                              color_poly == (fills_seen == 0 ? 0x11111111 : 0x22222222),
                          "FILL_POLY header fields mismatch");

                    for (k = 0; k < nb_points; k++) {
                        Struc_Point got;
                        polyrec_unpack_point(&got,
                                             payload + 12 + k * POLYREC_POINT_SIZE);
                        CHECK(points_equal(&got, &pts[k]),
                              "recorded vertex slot does not match source vertex");
                        CHECK(got.Pt_RepMask == 0,
                              "unpack must zero Pt_RepMask");
                    }
                    fills_seen++;
                } else if (type == POLYREC_EVT_EOF) {
                    break;
                }
                ev_off += 1 + payload_size;
            }
            CHECK(fills_seen == 2, "expected exactly two FILL_POLY events");
            CHECK(stored_count == 3, "header event count mismatch");
            rc = 0;
            goto done;
        }
        off += sh.size;
    }
    fprintf(stderr, "FAIL: EVNT section not found\n");

done:
    if (f)
        fclose(f);
    free(file);
    remove(path);
    return rc;
}

int main(void) {
    if (test_pack_unpack_roundtrip() != 0)
        return 1;
    if (test_recorded_file_layout() != 0)
        return 1;
    printf("test_polyrec_points: OK\n");
    return 0;
}
