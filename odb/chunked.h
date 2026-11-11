#ifndef ODB_CHUNKED_H
#define ODB_CHUNKED_H

#include "git-compat-util.h"

#define ODB_CHUNK_DIRECTORY "chunks"

struct odb_stream;

bool odb_chunked_is_manifest(const void *data, size_t len);

/*
 * Open the blob represented by a manifest. The stream owns a copy of the
 * manifest entries and does not retain the caller's buffer.
 */
int odb_chunked_open(struct odb_stream **out, const char *object_dir,
		     const void *manifest, size_t len);

void *odb_chunked_read_contents(struct odb_stream *stream);

#endif
