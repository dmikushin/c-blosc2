//
// Created by faltet on 18/12/15.
//

#ifndef BLOSC_DELTA_H
#define BLOSC_DELTA_H

#include "blosc.h"

int delta_encoder8(schunk_header* sc_header, int nbytes, unsigned char* src, unsigned char* dest);

int delta_decoder8(schunk_header* sc_header, int nbytes, unsigned char* src);

#endif //BLOSC_DELTA_H
