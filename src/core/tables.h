#ifndef SR_TABLES_H
#define SR_TABLES_H
#include <stdint.h>
extern const uint8_t  sr_quad[74][4];      /* ds:0x322 color quads */
extern const uint16_t sr_cowl[138];        /* ds:0x44a dashboard silhouette */
extern const int16_t  sr_pan[7];           /* ds:0x38 sprite lean nudge */
extern const uint8_t  sr_heightclass[8];   /* ds:0xb77 */
extern const uint8_t  sr_shadow[5][261];   /* ds:0x65e 29x9 row-major */
extern const uint8_t  sr_digits[10][20];   /* ds:0x13c 4x5 digit stencils */
extern const uint8_t  sr_aplight[2][130];  /* ds:0x204 jump-o-master stamps */
#endif
