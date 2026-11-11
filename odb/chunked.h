#ifndef ODB_CHUNKED_H
#define ODB_CHUNKED_H

#include "object-file.h"
#include "odb.h"

#define ODB_CHUNK_DIRECTORY "chunks"

struct odb_source_loose;
struct odb_stream;
struct repository;

bool odb_chunked_is_manifest(const void *data, size_t len);

/*
 * Open the blob represented by a manifest. The stream owns a copy of the
 * manifest entries and does not retain the caller's buffer.
 */
int odb_chunked_open(struct odb_stream **out, const char *object_dir,
		     const void *manifest, size_t len);

void *odb_chunked_read_contents(struct odb_stream *stream);

/*
 * Write a chunked blob to a loose source. For a stream with no expected
 * object ID, the loose source must be a private staging directory so any
 * newly written chunks can be discarded on a duplicate write. A supplied
 * expected object ID is checked before publishing the manifest.
 */
int odb_chunked_write(struct odb_source_loose *loose,
		      struct odb_stream *stream,
		      const struct object_id *expected_oid,
		      const struct object_id *compat_oid,
		      const time_t *mtime,
		      enum odb_write_object_flags flags,
		      struct object_id *result_oid);

#endif
