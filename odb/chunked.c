#include "git-compat-util.h"
#include "config.h"
#include "gettext.h"
#include "git-zlib.h"
#include "hex.h"
#include "loose.h"
#include "object-file.h"
#include "odb/chunked.h"
#include "odb/fastcdc.h"
#include "odb/source-loose.h"
#include "odb/streaming.h"
#include "path.h"
#include "strbuf.h"
#include "strvec.h"
#include "tempfile.h"
#include "write-or-die.h"

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

static bool chunk_fsync_enabled(void)
{
	return fsync_object_files > 0 || (fsync_components & FSYNC_COMPONENT_LOOSE_OBJECT);
}

static bool chunk_write_is_staged(struct odb_source_loose *loose)
{
	struct object_database *odb = loose->base.odb;

	return odb->transaction &&
		strcmp(loose->base.path, repo_get_object_directory(odb->repo));
}

static int harden_existing_chunk(const char *path)
{
	int fd, ret;

	if (!chunk_fsync_enabled())
		return 0;

	fd = git_open(path);
	if (fd < 0)
		return -1;
	ret = fsync_file(fd);
	if (close(fd) < 0)
		ret = -1;
	return ret;
}

int odb_chunked_install_file(struct repository *repo, const char *src,
			     const char *dst)
{
	const char *leaf = strrchr(src, '/');
	char hex[GIT_SHA256_HEXSZ + 1];
	unsigned char digest[GIT_SHA256_RAWSZ], *data;
	int ret = -1;

	if (!leaf || leaf - src < 3 || leaf[-3] != '/' ||
	    strlen(leaf + 1) != GIT_SHA256_HEXSZ - 2)
		return error(_("invalid staged blob chunk path '%s'"), src);
	memcpy(hex, leaf - 2, 2);
	memcpy(hex + 2, leaf + 1, GIT_SHA256_HEXSZ - 1);
	if (get_hash_hex_algop(hex, digest, &hash_algos[GIT_HASH_SHA256]))
		return error(_("invalid staged blob chunk path '%s'"), src);

	data = xmalloc(ODB_FASTCDC_MAX);
	/*
	 * Batch mode has already hardened the staged copy. Publish it without
	 * flushing each existing destination. If replacement fails, try to
	 * sync and reuse a healthy destination instead.
	 */
	if (batch_fsync_enabled(FSYNC_COMPONENT_LOOSE_OBJECT) &&
	    !read_chunk_file(src, data, 0, digest, true) &&
	    !rename(src, dst)) {
		ret = adjust_shared_perm(repo, dst);
		if (ret)
			error(_("unable to set permission to '%s'"), dst);
		goto out;
	}

	/* A healthy destination is already usable by every referring manifest. */
	if (!read_chunk_file(dst, data, 0, digest, true) &&
	    !harden_existing_chunk(dst))
		ret = unlink_or_warn(src);
	else if (!read_chunk_file(src, data, 0, digest, false))
		ret = finalize_object_file_flags(repo, src, dst, FOF_REPLACE_EXISTING);
out:
	free(data);
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

static int write_chunk_file(struct odb_source_loose *loose,
			    struct strbuf *path, const void *data, size_t len,
			    const time_t *mtime, enum odb_write_object_flags flags,
			    enum finalize_object_file_flags finalize_flags)
{
	struct repository *repo = loose->base.odb->repo;
	struct tempfile *tmp = NULL;
	struct strbuf template = STRBUF_INIT;
	int fd, ret = -1;

	if (safe_create_leading_directories(repo, path->buf) != SCLD_OK)
		goto out;
	strbuf_add(&template, path->buf, strrchr(path->buf, '/') - path->buf + 1);
	strbuf_addstr(&template, "tmp_obj_XXXXXX");
	tmp = mks_tempfile_m(template.buf, 0444);
	if (!tmp)
		goto out;
	fd = get_tempfile_fd(tmp);
	if (write_in_full(fd, data, len) < 0)
		goto out;

	if (!loose->will_destroy) {
		if (batch_fsync_enabled(FSYNC_COMPONENT_LOOSE_OBJECT))
			odb_transaction_files_fsync(loose->base.odb->transaction,
						    fd, get_tempfile_path(tmp));
		else if (fsync_object_files > 0)
			fsync_or_die(fd, get_tempfile_path(tmp));
		else
			fsync_component_or_die(FSYNC_COMPONENT_LOOSE_OBJECT,
					       fd, get_tempfile_path(tmp));
	}
	if (close_tempfile_gently(tmp) < 0)
		goto out;
	if (mtime) {
		struct utimbuf times = { .actime = *mtime, .modtime = *mtime };

		if (utime(get_tempfile_path(tmp), &times) < 0 &&
		    !(flags & ODB_WRITE_OBJECT_SILENT))
			warning_errno(_("failed utime() on '%s'"), path->buf);
	}

	/*
	 * The name describes the content, and concurrent writes can differ in
	 * zlib settings or in the representation of the loose object.
	 */
	ret = finalize_object_file_flags(repo, get_tempfile_path(tmp), path->buf,
					 finalize_flags);
	goto cleanup;

out:
	if (!(flags & ODB_WRITE_OBJECT_SILENT))
		error_errno(_("unable to write chunked blob data to '%s'"), path->buf);
cleanup:
	delete_tempfile(&tmp);
	strbuf_release(&template);
	return ret;
}

static int write_chunk(struct odb_source_loose *loose, struct strbuf *path,
		       struct strbuf *compressed, const unsigned char *data,
		       size_t len, const unsigned char *digest,
		       enum odb_write_object_flags flags, struct strvec *written)
{
	struct z_stream_s z = { 0 };
	int status, ret;
	enum finalize_object_file_flags finalize_flags =
		chunk_fsync_enabled() && !loose->will_destroy ?
		FOF_REPLACE_EXISTING : FOF_SKIP_COLLISION_CHECK;

	chunk_path(path, loose->base.path, digest);
	if (check_and_freshen_file(path->buf, 0, NULL)) {
		strbuf_grow(compressed, len);
		ret = read_chunk_file(path->buf, (unsigned char *)compressed->buf,
				      len, digest, true);
		if (!ret && (loose->will_destroy ||
			     (batch_fsync_enabled(FSYNC_COMPONENT_LOOSE_OBJECT) &&
			      chunk_write_is_staged(loose)) ||
			     !harden_existing_chunk(path->buf)))
			return 0;

		/* Replace a corrupt or unsyncable chunk with a verified copy. */
		finalize_flags = FOF_REPLACE_EXISTING;
		/* Another manifest may already depend on this staged path. */
		written = NULL;
	}

	if (deflateInit(&z, repo_config_values(loose->base.odb->repo)->zlib_compression_level) != Z_OK)
		goto compress_error;
	z.avail_out = deflateBound(&z, len);
	strbuf_grow(compressed, z.avail_out);
	z.next_out = (unsigned char *)compressed->buf;
	z.next_in = (unsigned char *)data;
	z.avail_in = len;
	status = deflate(&z, Z_FINISH);
	deflateEnd(&z);
	if (status != Z_STREAM_END || z.avail_in)
		goto compress_error;

	ret = write_chunk_file(loose, path, compressed->buf, z.total_out, NULL,
			       flags, finalize_flags);
	if (!ret && written)
		strvec_push(written, path->buf);
	return ret;

compress_error:
	if (!(flags & ODB_WRITE_OBJECT_SILENT))
		error(_("unable to compress blob chunk '%s'"), path->buf);
	return -1;
}

int odb_chunked_write(struct odb_source_loose *loose,
		      struct odb_stream *stream,
		      const struct object_id *expected_oid,
		      const struct object_id *compat_oid,
		      const time_t *mtime,
		      enum odb_write_object_flags flags,
		      struct object_id *result_oid)
{
	struct repository *repo = loose->base.odb->repo;
	const struct git_hash_algo *compat = repo->compat_hash_algo;
	struct git_hash_ctx ctx, compat_ctx;
	struct object_id oid, computed_compat_oid;
	struct strbuf manifest = STRBUF_INIT;
	struct strbuf compressed = STRBUF_INIT;
	struct strbuf path = STRBUF_INIT;
	struct strvec written = STRVEC_INIT;
	unsigned char *buf;
	unsigned char size_buf[sizeof(uint64_t)];
	unsigned char checksum[GIT_SHA256_RAWSZ];
	char header[MAX_HEADER_LEN];
	size_t begin = 0, end = 0, remaining = stream->size;
	int header_len, ret = -1;
	bool publishing_manifest = false;

	if (stream->type != OBJ_BLOB || stream->size <= ODB_FASTCDC_MAX)
		BUG("chunk writer requires a large blob stream");

	buf = xmalloc(2 * ODB_FASTCDC_MAX);
	header_len = format_object_header(header, sizeof(header), OBJ_BLOB, stream->size);
	git_hash_init(&ctx, repo->hash_algo);
	git_hash_update(&ctx, header, header_len);
	if (compat && !compat_oid) {
		git_hash_init(&compat_ctx, compat);
		git_hash_update(&compat_ctx, header, header_len);
	}
	strbuf_add(&manifest, manifest_magic, sizeof(manifest_magic));
	put_be64(size_buf, stream->size);
	strbuf_add(&manifest, size_buf, sizeof(size_buf));

	while (remaining || begin < end) {
		unsigned char entry[MANIFEST_ENTRY_SIZE];
		unsigned char *digest = entry + sizeof(uint32_t);
		size_t len;

		if (end - begin < ODB_FASTCDC_MAX && remaining) {
			memmove(buf, buf + begin, end - begin);
			end -= begin;
			begin = 0;
			while (end < 2 * ODB_FASTCDC_MAX && remaining) {
				size_t want = MIN(remaining, 2 * ODB_FASTCDC_MAX - end);
				ssize_t n = odb_stream_read(stream, buf + end, want);

				if (n <= 0) {
					error(_("unable to read complete blob for chunking"));
					goto discard_hash;
				}
				end += n;
				remaining -= n;
			}
		}

		len = odb_fastcdc_cut(buf + begin, MIN(end - begin, ODB_FASTCDC_MAX));
		hash_sha256(buf + begin, len, digest);
		if (write_chunk(loose, &path, &compressed, buf + begin, len, digest,
				flags, expected_oid ? NULL : &written))
			goto discard_hash;
		git_hash_update(&ctx, buf + begin, len);
		if (compat && !compat_oid)
			git_hash_update(&compat_ctx, buf + begin, len);
		put_be32(entry, len);
		strbuf_add(&manifest, entry, sizeof(entry));
		begin += len;
	}
	if (odb_stream_read(stream, buf, 1) != 0) {
		error(_("blob stream size changed while chunking"));
		goto discard_hash;
	}

	git_hash_final_oid(&oid, &ctx);
	if (compat && !compat_oid) {
		git_hash_final_oid(&computed_compat_oid, &compat_ctx);
		compat_oid = &computed_compat_oid;
	}
	if (expected_oid && !oideq(&oid, expected_oid)) {
		error(_("object data changed while chunking %s"), oid_to_hex(expected_oid));
		goto out;
	}
	if (!expected_oid && odb_freshen_object(loose->base.odb, &oid)) {
		ret = 0;
		goto out;
	}

	/* A failed map write must leave the object eligible for a retry. */
	if (compat_oid && (repo_add_loose_object_map(loose, &oid, compat_oid) ||
			   (!loose->will_destroy && !chunk_write_is_staged(loose) &&
			    loose_object_map_fsync(loose))))
		goto out;

	odb_loose_path(loose, &path, &oid);
	hash_sha256(manifest.buf, manifest.len, checksum);
	strbuf_add(&manifest, checksum, sizeof(checksum));
	publishing_manifest = true;
	ret = write_chunk_file(loose, &path, manifest.buf, manifest.len, mtime,
			       flags, FOF_SKIP_COLLISION_CHECK);
	goto out;

discard_hash:
	git_hash_discard(&ctx);
	if (compat && !compat_oid)
		git_hash_discard(&compat_ctx);
out:
	if (!publishing_manifest) {
		for (size_t i = 0; i < written.nr; i++) {
			if (unlink_or_warn(written.v[i]))
				ret = -1;
		}
	}
	if (!ret && result_oid)
		oidcpy(result_oid, &oid);
	free(buf);
	strbuf_release(&manifest);
	strbuf_release(&compressed);
	strbuf_release(&path);
	strvec_clear(&written);
	return ret;
}
