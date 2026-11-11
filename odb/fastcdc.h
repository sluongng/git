#ifndef ODB_FASTCDC_H
#define ODB_FASTCDC_H

#include <stddef.h>

#define ODB_FASTCDC_AVG (1024 * 1024)
#define ODB_FASTCDC_MIN (ODB_FASTCDC_AVG / 4)
#define ODB_FASTCDC_MAX (ODB_FASTCDC_AVG * 4)

/*
 * Return the length of the first chunk using FastCDC 2020, normalization 2
 * and seed 0. Pass at least ODB_FASTCDC_MAX bytes unless at end of input.
 * An empty input has no chunk and returns zero.
 */
size_t odb_fastcdc_cut(const unsigned char *data, size_t len);

#endif
