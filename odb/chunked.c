#include "git-compat-util.h"
#include "gettext.h"
#include "git-zlib.h"
#include "hex.h"
#include "object-file.h"
#include "odb/chunked.h"
#include "odb/fastcdc.h"
#include "odb/streaming.h"
#include "strbuf.h"

/*
 * The first byte distinguishes the manifest from a zlib-compressed loose
 * object. The last byte identifies the layout version. A big-endian uint64
 * blob size follows, then one big-endian uint32 length and raw SHA-256
 * digest per chunk, in blob order. A trailing SHA-256 checksum covers
 * the header and all entries.
 */
static const unsigned char manifest_magic[] = { 0, 'F', 'I', 'L', 'E', 'V', '2', 1 };
#define MANIFEST_HEADER_SIZE (sizeof(manifest_magic) + sizeof(uint64_t))
#define MANIFEST_ENTRY_SIZE (sizeof(uint32_t) + GIT_SHA256_RAWSZ)

struct chunked_read_stream {
	struct odb_stream base;
	char *object_dir;
	unsigned char *entries;
	size_t entry_nr;
	size_t next_entry;
	unsigned char *chunk;
	size_t chunk_alloc;
	size_t chunk_len;
	size_t chunk_offset;
	struct strbuf path;
};

static void hash_sha256(const void *data, size_t len, unsigned char *digest)
{
	struct git_hash_ctx ctx;

	git_hash_init(&ctx, &hash_algos[GIT_HASH_SHA256]);
	git_hash_update(&ctx, data, len);
	git_hash_final(digest, &ctx);
}

static void chunk_path(struct strbuf *path, const char *object_dir,
		       const unsigned char *digest)
{
	char hex[GIT_SHA256_HEXSZ + 1];

	hash_to_hex_algop_r(hex, digest, &hash_algos[GIT_HASH_SHA256]);
	strbuf_reset(path);
	strbuf_addf(path, "%s/%s/%.2s/%s",
		    object_dir, ODB_CHUNK_DIRECTORY, hex, hex + 2);
}

bool odb_chunked_is_manifest(const void *data, size_t len)
{
	size_t prefix_len = sizeof(manifest_magic) - 1;

	return len >= prefix_len && !memcmp(data, manifest_magic, prefix_len);
}

static int read_chunk_file(const char *path, unsigned char *data, size_t len,
			   const unsigned char *digest, bool quiet)
{
	unsigned char actual_digest[GIT_SHA256_RAWSZ];
	unsigned char *compressed = NULL;
	struct z_stream_s z = { 0 };
	struct stat st;
	int fd, ret = -1;
	int status;

	fd = git_open(path);
	if (fd < 0)
		return quiet ? -1 : error_errno(_("unable to open blob chunk '%s'"), path);
	if (fstat(fd, &st) < 0) {
		if (!quiet)
			error_errno(_("unable to stat blob chunk '%s'"), path);
		goto out;
	}
	/* Bound the compressed input even when the on-disk file is corrupt. */
	if (!S_ISREG(st.st_mode) || st.st_size <= 0 ||
	    st.st_size > 2 * ODB_FASTCDC_MAX)
		goto corrupt;
	ALLOC_ARRAY(compressed, st.st_size);
	if (read_in_full(fd, compressed, st.st_size) != st.st_size)
		goto corrupt;

	z.next_in = compressed;
	z.avail_in = st.st_size;
	z.next_out = data;
	z.avail_out = len ? len : ODB_FASTCDC_MAX;
	if (inflateInit(&z) != Z_OK)
		goto corrupt;
	status = inflate(&z, Z_FINISH);
	inflateEnd(&z);
	if (status != Z_STREAM_END || z.avail_in || !z.total_out ||
	    (len && z.total_out != len))
		goto corrupt;

	hash_sha256(data, z.total_out, actual_digest);
	if (memcmp(digest, actual_digest, sizeof(actual_digest)))
		goto corrupt;

	ret = 0;
	goto out;

corrupt:
	if (!quiet)
		error(_("corrupt blob chunk '%s'"), path);
out:
	close(fd);
	free(compressed);
	return ret;
}

static ssize_t read_chunked_stream(struct odb_stream *base, char *buf, size_t len)
{
	struct chunked_read_stream *stream = container_of(base,
							 struct chunked_read_stream,
							 base);
	size_t copied = 0;

	while (copied < len) {
		size_t count;

		if (stream->chunk_offset == stream->chunk_len) {
			const unsigned char *entry;
			const unsigned char *digest;
			size_t chunk_len;

			if (stream->next_entry == stream->entry_nr)
				break;
			entry = stream->entries + stream->next_entry * MANIFEST_ENTRY_SIZE;
			chunk_len = get_be32(entry);
			digest = entry + sizeof(uint32_t);
			chunk_path(&stream->path, stream->object_dir, digest);
			ALLOC_GROW(stream->chunk, chunk_len, stream->chunk_alloc);
			if (read_chunk_file(stream->path.buf, stream->chunk, chunk_len, digest, false) < 0)
				return -1;
			stream->chunk_len = chunk_len;
			stream->chunk_offset = 0;
			stream->next_entry++;
		}
		count = MIN(len - copied, stream->chunk_len - stream->chunk_offset);
		memcpy(buf + copied, stream->chunk + stream->chunk_offset, count);
		stream->chunk_offset += count;
		copied += count;
	}
	return copied;
}

static int close_chunked_stream(struct odb_stream *base)
{
	struct chunked_read_stream *stream = container_of(base,
							 struct chunked_read_stream,
							 base);

	free(stream->object_dir);
	free(stream->entries);
	free(stream->chunk);
	strbuf_release(&stream->path);
	return 0;
}

int odb_chunked_open(struct odb_stream **out, const char *object_dir,
		     const void *manifest, size_t len)
{
	const unsigned char *data = manifest, *entries;
	struct chunked_read_stream *stream;
	uint64_t size, remaining;
	size_t entries_len;
	unsigned char checksum[GIT_SHA256_RAWSZ];

	if (len < MANIFEST_HEADER_SIZE + GIT_SHA256_RAWSZ ||
	    memcmp(data, manifest_magic, sizeof(manifest_magic)))
		return error(_("invalid blob manifest header"));

	len -= GIT_SHA256_RAWSZ;
	hash_sha256(data, len, checksum);
	if (memcmp(checksum, data + len, GIT_SHA256_RAWSZ))
		return error(_("invalid blob manifest checksum"));

	size = get_be64(data + sizeof(manifest_magic));
	entries = data + MANIFEST_HEADER_SIZE;
	entries_len = len - MANIFEST_HEADER_SIZE;
	if (size > SIZE_MAX || size <= ODB_FASTCDC_MAX || !entries_len ||
	    entries_len % MANIFEST_ENTRY_SIZE)
		return error(_("invalid blob manifest size"));

	remaining = size;
	for (size_t offset = 0; offset < entries_len; offset += MANIFEST_ENTRY_SIZE) {
		uint32_t chunk_len = get_be32(entries + offset);

		if (!chunk_len || chunk_len > ODB_FASTCDC_MAX ||
		    chunk_len > remaining ||
		    (offset + MANIFEST_ENTRY_SIZE < entries_len &&
		     chunk_len < ODB_FASTCDC_MIN))
			return error(_("invalid chunk length in blob manifest"));
		remaining -= chunk_len;
	}
	if (remaining)
		return error(_("blob manifest size does not match its chunks"));

	CALLOC_ARRAY(stream, 1);
	stream->base.type = OBJ_BLOB;
	stream->base.size = size;
	stream->base.read = read_chunked_stream;
	stream->base.close = close_chunked_stream;
	stream->object_dir = xstrdup(object_dir);
	stream->entries = xmemdupz(entries, entries_len);
	stream->entry_nr = entries_len / MANIFEST_ENTRY_SIZE;
	strbuf_init(&stream->path, 0);
	*out = &stream->base;
	return 0;
}

void *odb_chunked_read_contents(struct odb_stream *stream)
{
	char *content = xmallocz(stream->size);

	for (size_t offset = 0; offset < stream->size;) {
		size_t want = MIN(stream->size - offset, ODB_FASTCDC_MAX);
		ssize_t len = odb_stream_read(stream, content + offset, want);

		if (len <= 0) {
			free(content);
			return NULL;
		}
		offset += len;
	}
	return content;
}
