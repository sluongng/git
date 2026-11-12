#ifndef ODB_CHUNKED_H
#define ODB_CHUNKED_H

#include "object-file.h"
#include "odb.h"

#define ODB_CHUNK_DIRECTORY "chunks"

struct odb_source_loose;
struct odb_stream;
struct repository;

struct odb_blob_chunk {
	uint32_t len;
	unsigned char digest[GIT_SHA256_RAWSZ];
};

struct odb_blob_manifest {
	size_t size;
	size_t nr;
	struct odb_blob_chunk *chunks;
};

bool odb_chunked_is_manifest(const void *data, size_t len);
bool odb_chunked_is_manifest_file(const char *path);

/* Read a native manifest from the primary object directory. */
int odb_chunked_read_manifest(struct repository *repo, const struct object_id *oid,
			      size_t max_chunks, struct odb_blob_manifest *out);

/* Verify a chunk and optionally return its zlib stream. */
int odb_chunked_read_chunk(const char *object_dir, const struct odb_blob_chunk *chunk,
			   void *data, struct strbuf *compressed);
int odb_chunked_inflate(const struct odb_blob_chunk *chunk,
			const void *compressed, size_t compressed_len, void *data);

/*
 * Open the blob represented by a manifest. The stream owns a copy of the
 * manifest entries and does not retain the caller's buffer.
 */
int odb_chunked_open(struct odb_stream **out, const char *object_dir,
		     const void *manifest, size_t len);

void *odb_chunked_read_contents(struct odb_stream *stream);

int odb_chunked_install_file(struct repository *repo, const char *src,
			     const char *dst);

int odb_chunked_for_each_file(struct odb_source *source,
			      each_loose_object_fn chunk_cb,
			      each_loose_cruft_fn cruft_cb);

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
