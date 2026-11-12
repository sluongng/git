#include "git-compat-util.h"
#include "blob-chunks.h"
#include "config.h"
#include "environment.h"
#include "gettext.h"
#include "hex.h"
#include "object-file.h"
#include "odb.h"
#include "odb/chunked.h"
#include "odb/fastcdc.h"
#include "oidset.h"
#include "pkt-line.h"
#include "refs.h"
#include "repository.h"
#include "run-command.h"
#include "strbuf.h"
#include "tempfile.h"
#include "trace.h"
#include "upload-pack.h"

#define MAX_BLOBS 16
#define MAX_CHUNKS 4096
#define MAX_BLOB_BYTES ((size_t)1 << 30)
#define MAX_TRANSFER_BYTES ((size_t)1 << 29)
#define MAX_REFS_SCANNED 65536
#define MAX_REACHABILITY_OBJECTS 1000000
#define REACHABILITY_TIMEOUT_MS 5000

#ifdef GIT_WINDOWS_NATIVE
#define REACHABILITY_STOP_SIGNAL SIGTERM
#else
/* git-daemon starts upload-pack with SIGTERM ignored. */
#define REACHABILITY_STOP_SIGNAL SIGKILL
#endif

static const struct git_hash_algo *chunk_hash_algo(void)
{
	return &hash_algos[GIT_HASH_SHA256];
}

static void manifest_id(const struct odb_blob_manifest *manifest,
			unsigned char *digest)
{
	static const char domain[] = "blob-chunks-v1";
	struct git_hash_ctx ctx;
	unsigned char size[8], len[4];

	git_hash_init(&ctx, chunk_hash_algo());
	git_hash_update(&ctx, domain, sizeof(domain));
	put_be64(size, manifest->size);
	git_hash_update(&ctx, size, sizeof(size));
	for (size_t i = 0; i < manifest->nr; i++) {
		const struct odb_blob_chunk *chunk = &manifest->chunks[i];

		put_be32(len, chunk->len);
		git_hash_update(&ctx, len, sizeof(len));
		git_hash_update(&ctx, chunk->digest, sizeof(chunk->digest));
	}
	git_hash_final(digest, &ctx);
}

static bool text_packet(const struct packet_reader *reader)
{
	return reader->status == PACKET_READ_NORMAL &&
		!memchr(reader->line, 0, reader->pktlen);
}

static int read_number(const char **p, size_t limit, size_t *result)
{
	uintmax_t n;
	char *end;

	if (!isdigit(**p))
		return -1;
	errno = 0;
	n = strtoumax(*p, &end, 10);
	if (errno || n > limit)
		return -1;
	*p = end;
	*result = n;
	return 0;
}

struct requested_blob {
	struct object_id oid;
	struct odb_blob_manifest manifest;
	unsigned char id[GIT_SHA256_RAWSZ];
	size_t *ordinals;
	size_t nr, alloc;
	size_t transfer_bytes;
};

struct request_batch {
	struct requested_blob blobs[MAX_BLOBS];
	size_t nr;
	bool chunks;
};

static void release_batch(struct request_batch *batch)
{
	for (size_t i = 0; i < batch->nr; i++) {
		free(batch->blobs[i].manifest.chunks);
		free(batch->blobs[i].ordinals);
	}
}

static int read_request(struct repository *repo, struct packet_reader *reader,
			struct request_batch *batch)
{
	struct requested_blob *blob = NULL;
	size_t nr_ordinals = 0;

	if (packet_reader_read(reader) != PACKET_READ_NORMAL || !text_packet(reader))
		return -1;
	if (!strcmp(reader->line, "chunks"))
		batch->chunks = true;
	else if (strcmp(reader->line, "manifest"))
		return -1;

	while (packet_reader_read(reader) == PACKET_READ_NORMAL) {
		const char *arg;

		if (!text_packet(reader))
			return -1;

		if (skip_prefix(reader->line, "oid ", &arg)) {
			if (batch->nr == MAX_BLOBS ||
			    (batch->chunks && blob && !blob->nr))
				return -1;
			blob = &batch->blobs[batch->nr++];
			if (parse_oid_hex_algop(arg, &blob->oid, &arg, repo->hash_algo))
				return -1;
			for (size_t i = 0; i + 1 < batch->nr; i++)
				if (oideq(&batch->blobs[i].oid, &blob->oid))
					return -1;
			if (!batch->chunks) {
				if (*arg)
					return -1;
			} else if (*arg != ' ' || strlen(arg + 1) != GIT_SHA256_HEXSZ ||
				   get_hash_hex_algop(arg + 1, blob->id, chunk_hash_algo())) {
				return -1;
			}
		} else if (batch->chunks && blob &&
			   skip_prefix(reader->line, "chunk ", &arg)) {
			size_t ordinal;

			if (++nr_ordinals > MAX_CHUNKS ||
			    read_number(&arg, MAX_CHUNKS - 1, &ordinal) || *arg ||
			    (blob->nr && ordinal <= blob->ordinals[blob->nr - 1]))
				return -1;
			ALLOC_GROW(blob->ordinals, blob->nr + 1, blob->alloc);
			blob->ordinals[blob->nr++] = ordinal;
		} else {
			return -1;
		}
	}

	return reader->status != PACKET_READ_FLUSH || !blob ||
		(batch->chunks && !blob->nr) ? -1 : 0;
}

static void load_manifests(struct repository *repo, struct request_batch *batch,
			   struct oidset *eligible)
{
	for (size_t i = 0; i < batch->nr; i++) {
		struct requested_blob *blob = &batch->blobs[i];
		struct odb_blob_manifest *manifest = &blob->manifest;
		unsigned char digest[GIT_SHA256_RAWSZ];

		if (odb_chunked_read_manifest(repo, &blob->oid, MAX_CHUNKS, manifest))
			continue;
		if (manifest->size > MAX_BLOB_BYTES)
			continue;

		if (batch->chunks) {
			if (blob->ordinals[blob->nr - 1] >= manifest->nr)
				continue;
			manifest_id(manifest, digest);
			if (memcmp(blob->id, digest, sizeof(digest)))
				continue;
			for (size_t j = 0; j < blob->nr; j++)
				blob->transfer_bytes += manifest->chunks[blob->ordinals[j]].len;
			if (blob->transfer_bytes > MAX_TRANSFER_BYTES)
				continue;
		}
		oidset_insert(eligible, &blob->oid);
	}
}

static void limit_response(const struct request_batch *batch, struct oidset *authorized)
{
	size_t nr_chunks = 0, blob_bytes = 0, transfer_bytes = 0;

	for (size_t i = 0; i < batch->nr; i++) {
		const struct requested_blob *blob = &batch->blobs[i];
		const struct odb_blob_manifest *manifest = &blob->manifest;

		if (!oidset_contains(authorized, &blob->oid))
			continue;
		if (manifest->nr > MAX_CHUNKS - nr_chunks ||
		    manifest->size > MAX_BLOB_BYTES - blob_bytes ||
		    blob->transfer_bytes > MAX_TRANSFER_BYTES - transfer_bytes) {
			oidset_remove(authorized, &blob->oid);
			continue;
		}
		nr_chunks += manifest->nr;
		blob_bytes += manifest->size;
		transfer_bytes += blob->transfer_bytes;
	}
}

struct visible_roots {
	struct strvec hidden_refs;
	size_t scanned;
	size_t nr;
	int fd;
};

static int hide_refs_config(const char *key, const char *value,
			    const struct config_context *ctx UNUSED, void *data)
{
	struct visible_roots *roots = data;

	return parse_hide_refs_config(key, value, "uploadpack", &roots->hidden_refs);
}

static int write_visible_root(const struct reference *ref, void *data)
{
	struct visible_roots *roots = data;
	const char *name = strip_namespace(ref->name);
	char line[GIT_MAX_HEXSZ + 2];
	int len;

	if (++roots->scanned > MAX_REFS_SCANNED)
		return -1;
	if (!ref->oid || !name || starts_with(name, "refs/namespaces/") ||
	    ref_is_hidden(name, ref->name, &roots->hidden_refs))
		return 0;
	roots->nr++;
	len = xsnprintf(line, sizeof(line), "%s\n", oid_to_hex(ref->oid));
	return write_in_full(roots->fd, line, len) < 0 ? -1 : 0;
}

static int write_detached_head(const struct reference *ref, void *data)
{
	/* A symbolic HEAD must not expose the target of a hidden ref. */
	return ref->flags & REF_ISSYMREF ? 0 : write_visible_root(ref, data);
}

/* Return 0 at EOF, 1 when done early, and -1 on timeout or bad output. */
static int read_reachable_oids(struct repository *repo, int fd,
			       const struct oidset *eligible, struct oidset *authorized)
{
	const size_t hexsz = repo->hash_algo->hexsz, stride = hexsz + 1;
	const uint64_t ns_per_ms = 1000000;
	const uint64_t deadline = getnanotime() + REACHABILITY_TIMEOUT_MS * ns_per_ms;
	struct pollfd ready = { .fd = fd, .events = POLLIN };
	char buf[8192];
	size_t pending = 0, nr = 0;

	for (;;) {
		uint64_t now = getnanotime();
		size_t consumed = 0;
		ssize_t n;
		int ret;

		if (now >= deadline)
			return -1;
		ret = poll(&ready, 1, (deadline - now + ns_per_ms - 1) / ns_per_ms);
		if (ret < 0 && errno == EINTR)
			continue;
		if (ret <= 0)
			return -1;
		n = xread(fd, buf + pending, sizeof(buf) - pending);
		if (n <= 0)
			return n < 0 || pending ? -1 : 0;
		pending += n;

		while (pending - consumed >= stride) {
			const char *line = buf + consumed;
			struct object_id oid;

			if (line[hexsz] != '\n' || get_oid_hex_algop(line, &oid, repo->hash_algo))
				return -1;
			if (oidset_contains(eligible, &oid))
				oidset_insert(authorized, &oid);
			if (++nr >= MAX_REACHABILITY_OBJECTS ||
			    oidset_size(authorized) == oidset_size(eligible))
				return 1;
			consumed += stride;
		}
		pending -= consumed;
		memmove(buf, buf + consumed, pending);
	}
}

/*
 * Prove reachability from positive visible roots. A negative commit tip
 * does not subtract all of its blobs from a blob-only positive rev-list.
 * Stop after the requested OIDs are proven or the traversal budget runs out.
 */
static void authorize_blobs(struct repository *repo, struct oidset *eligible,
			    struct oidset *authorized)
{
	const char *prefixes[] = { "", NULL };
	struct refs_for_each_ref_options opts = { 0 };
	struct visible_roots roots = { .hidden_refs = STRVEC_INIT };
	struct child_process walk = CHILD_PROCESS_INIT;
	struct ref_store *refs = get_main_ref_store(repo);
	struct tempfile *input;
	int status, ret;

	if (!oidset_size(eligible))
		return;
	input = mks_tempfile_t("blob-chunks-refs-XXXXXX");
	if (!input)
		return;

	roots.fd = get_tempfile_fd(input);
	repo_config(repo, hide_refs_config, &roots);
	opts.namespace = get_git_namespace();
	ret = refs_head_ref_namespaced(refs, write_detached_head, &roots);
	if (!ret)
		ret = refs_for_each_ref_in_prefixes(refs, prefixes, &opts,
						    write_visible_root, &roots);
	strvec_clear(&roots.hidden_refs);
	if (ret || !roots.nr || lseek(roots.fd, 0, SEEK_SET) < 0)
		goto out;

	walk.in = dup(roots.fd);
	if (walk.in < 0)
		goto out;
	walk.out = -1;
	walk.no_stderr = 1;
	walk.git_cmd = 1;
	strvec_pushl(&walk.args, "rev-list", "--objects", "--no-object-names",
		     "--in-commit-order", "--missing=allow-any", "--stdin", NULL);
	strvec_pushl(&walk.env, NO_LAZY_FETCH_ENVIRONMENT "=1",
		     NO_REPLACE_OBJECTS_ENVIRONMENT "=1", NULL);
	if (start_command(&walk))
		goto out;

	ret = read_reachable_oids(repo, walk.out, eligible, authorized);
	close(walk.out);
	if (ret) {
		/* The walk can keep working without writing to the closed pipe. */
		kill(walk.pid, REACHABILITY_STOP_SIGNAL);
		status = finish_command_in_signal(&walk);
		child_process_clear(&walk);
	} else {
		status = finish_command(&walk);
	}
	if (ret < 0 || (status && status != 128 + SIGPIPE &&
			!(ret > 0 && status == 128 + REACHABILITY_STOP_SIGNAL)))
		oidset_clear(authorized);

out:
	delete_tempfile(&input);
}

int blob_chunks_advertise(struct repository *repo, struct strbuf *value)
{
	int allow = 0;
	char *hook;

	if (repo->object_storage_format != OBJECT_STORAGE_FORMAT_FILEV2 ||
	    repo_config_get_bool(repo, "uploadpack.allowblobchunks", &allow) ||
	    !allow)
		return 0;

	/* A pack-objects hook can enforce server policy on ordinary fetches. */
	hook = upload_pack_get_pack_objects_hook();
	if (hook) {
		free(hook);
		return 0;
	}
	if (value)
		strbuf_addstr(value, "1");
	return 1;
}

static void send_manifests(const struct request_batch *batch,
			   const struct oidset *authorized)
{
	for (size_t i = 0; i < batch->nr; i++) {
		const struct requested_blob *blob = &batch->blobs[i];
		const struct odb_blob_manifest *manifest = &blob->manifest;

		if (!oidset_contains(authorized, &blob->oid)) {
			packet_write_fmt(1, "unavailable %s\n", oid_to_hex(&blob->oid));
			continue;
		}
		packet_write_fmt(1, "manifest %s %"PRIuMAX" %"PRIuMAX"\n",
				 oid_to_hex(&blob->oid), (uintmax_t)manifest->size,
				 (uintmax_t)manifest->nr);
		for (size_t j = 0; j < manifest->nr; j++) {
			const struct odb_blob_chunk *chunk = &manifest->chunks[j];

			packet_write_fmt(1, "chunk %"PRIu32" %s\n", chunk->len,
					 hash_to_hex_algop(chunk->digest, chunk_hash_algo()));
		}
	}
}

static void send_chunks(struct repository *repo, const struct request_batch *batch,
			const struct oidset *authorized)
{
	unsigned char *buf = xmalloc(ODB_FASTCDC_MAX);
	struct strbuf compressed = STRBUF_INIT;
	const char *object_dir = repo_get_object_directory(repo);

	for (size_t i = 0; i < batch->nr; i++) {
		const struct requested_blob *blob = &batch->blobs[i];
		bool can_read = oidset_contains(authorized, &blob->oid);

		for (size_t j = 0; j < blob->nr; j++) {
			size_t ordinal = blob->ordinals[j];

			if (!can_read ||
			    odb_chunked_read_chunk(object_dir, &blob->manifest.chunks[ordinal],
						    buf, &compressed)) {
				packet_write_fmt(1, "unavailable %s %"PRIuMAX"\n",
						 oid_to_hex(&blob->oid), (uintmax_t)ordinal);
				continue;
			}
			packet_write_fmt(1, "data %s %"PRIuMAX" %"PRIuMAX"\n",
					 oid_to_hex(&blob->oid), (uintmax_t)ordinal,
					 (uintmax_t)compressed.len);
			for (size_t off = 0; off < compressed.len;) {
				size_t len = MIN(compressed.len - off, LARGE_PACKET_DATA_MAX);

				if (packet_write_payload(1, compressed.buf + off, len))
					die(_("unable to send blob chunk"));
				off += len;
			}
		}
	}
	strbuf_release(&compressed);
	free(buf);
}

int serve_blob_chunks(struct repository *repo, struct packet_reader *request)
{
	struct request_batch batch = { 0 };
	struct oidset eligible = OIDSET_INIT, authorized = OIDSET_INIT;

	if (read_request(repo, request, &batch))
		die(_("invalid blob-chunks request"));

	load_manifests(repo, &batch, &eligible);
	authorize_blobs(repo, &eligible, &authorized);
	limit_response(&batch, &authorized);
	if (batch.chunks)
		send_chunks(repo, &batch, &authorized);
	else
		send_manifests(&batch, &authorized);

	packet_flush(1);
	oidset_clear(&eligible);
	oidset_clear(&authorized);
	release_batch(&batch);
	return 0;
}
