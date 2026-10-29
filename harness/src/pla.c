/* pla.c - C64 PLA, transcribed term by term from the ABEL equations of a
 * dumped 906114 PLA (roms/c64pla.txt).  '&' = AND, '#' = OR, '!' = NOT. */
#include "pla.h"

uint32_t pla_tick(uint32_t pins) {
#define I(n) ((pins & PLA_##n) ? 1 : 0)
    const int A12 = I(A12), A13 = I(A13), A14 = I(A14), A15 = I(A15);
    const int _VA14 = I(_VA14), _CHAREN = I(_CHAREN), _HIRAM = I(_HIRAM), _LORAM = I(_LORAM);
    const int _CAS = I(_CAS), VA12 = I(VA12), VA13 = I(VA13), _GAME = I(_GAME);
    const int _EXROM = I(_EXROM), R__W = I(R__W), _AEC = I(_AEC), BA = I(BA);
#undef I

    const int Q7 = (_HIRAM & A15 & !A14 & A13 & !_AEC & R__W & !_EXROM & !_GAME)
                 | (A15 & A14 & A13 & !_AEC & _EXROM & !_GAME)
                 | (_AEC & _EXROM & !_GAME & VA13 & VA12);

    const int Q6 = (_LORAM & _HIRAM & A15 & !A14 & !A13 & !_AEC & R__W & !_EXROM)
                 | (A15 & !A14 & !A13 & !_AEC & _EXROM & !_GAME);

    const int Q5 = (_HIRAM & _CHAREN & A15 & A14 & !A13 & A12 & !_AEC & (BA | !R__W) & (!_EXROM | _GAME))
                 | (_LORAM & _CHAREN & A15 & A14 & !A13 & A12 & !_AEC & (BA | !R__W) & (!_EXROM | _GAME))
                 | (A15 & A14 & !A13 & A12 & !_AEC & (BA | !R__W) & _EXROM & !_GAME);

    const int Q3 = (_LORAM & !_CHAREN & A15 & A14 & !A13 & A12 & !_AEC & R__W & _GAME)
                 | (_HIRAM & !_CHAREN & A15 & A14 & !A13 & A12 & !_AEC & R__W & (!_EXROM | _GAME))
                 | (_VA14 & _AEC & _GAME & !VA13 & VA12)
                 | (_VA14 & _AEC & !_EXROM & !_GAME & !VA13 & VA12);

    const int Q2 = (_HIRAM & A15 & A14 & A13 & !_AEC & R__W & (!_EXROM | _GAME));

    const int Q1 = (_LORAM & _HIRAM & A15 & !A14 & A13 & !_AEC & R__W & _GAME);

    const int ROMH = !Q7;
    const int ROML = !Q6;
    const int I_O = !Q5;
    const int GR_W = !((!_CAS) & A15 & A14 & !A13 & A12 & !_AEC & !R__W);
    const int CHAROM = !Q3;
    const int KERNAL = !Q2;
    const int BASIC = !Q1;
    const int CASRAM = (_CAS | Q1 | Q2 | Q3 | Q5 | Q6 | Q7
                      | (_EXROM & !_GAME & !A15 & A14)
                      | (_EXROM & !_GAME & !A15 & !A14 & A12)
                      | (_EXROM & !_GAME & !A15 & !A14 & A13)
                      | (_EXROM & !_GAME & A15 & !A14 & A13)
                      | (_EXROM & !_GAME & A15 & A14 & !A13 & !A12));

    uint32_t out = pins & ~PLA_OUT_MASK;
    if (ROMH) out |= PLA_ROMH;
    if (ROML) out |= PLA_ROML;
    if (I_O) out |= PLA_I_O;
    if (GR_W) out |= PLA_GR_W;
    if (CHAROM) out |= PLA_CHAROM;
    if (KERNAL) out |= PLA_KERNAL;
    if (BASIC) out |= PLA_BASIC;
    if (CASRAM) out |= PLA_CASRAM;
    return out;
}
