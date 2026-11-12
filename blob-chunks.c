#include "git-compat-util.h"
#include "blob-chunks.h"
#include "config.h"
#include "connect.h"
#include "environment.h"
#include "gettext.h"
#include "hex.h"
#include "object-file.h"
#include "odb.h"
#include "odb/chunked.h"
#include "odb/fastcdc.h"
#include "odb/source-files.h"
#include "odb/streaming.h"
#include "oidmap.h"
#include "oidset.h"
#include "packfile.h"
#include "pkt-line.h"
#include "refs.h"
#include "remote.h"
#include "repository.h"
#include "run-command.h"
#include "sigchain.h"
#include "strbuf.h"
#include "tempfile.h"
#include "trace.h"
#include "trace2.h"
#include "upload-pack.h"

#define MAX_BLOBS 16
#define MAX_CHUNKS 4096
#define MAX_BLOB_BYTES ((size_t)1 << 30)
#define MAX_TRANSFER_BYTES ((size_t)1 << 29)
#define MAX_REFS_SCANNED 65536
#define MAX_REACHABILITY_OBJECTS 1000000
#define REACHABILITY_TIMEOUT_MS 5000
#define MAX_PROMISOR_ENTRIES 65536

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

static int send_request(int fd, struct strbuf *request)
{
	int ret;

	packet_buf_flush(request);
	sigchain_push(SIGPIPE, SIG_IGN);
	ret = write_in_full(fd, request->buf, request->len) < 0 ? -1 : 0;
	sigchain_pop(SIGPIPE);
	return ret;
}

static int read_response_end(struct packet_reader *reader, int stateless_rpc)
{
	if (packet_reader_read(reader) != PACKET_READ_FLUSH ||
	    (stateless_rpc && packet_reader_read(reader) != PACKET_READ_RESPONSE_END))
		return -1;
	return 0;
}

static int read_manifests(struct repository *repo, struct packet_reader *reader,
			   struct request_batch *batch, int stateless_rpc)
{
	size_t nr_chunks = 0, blob_bytes = 0;

	for (size_t i = 0; i < batch->nr; i++) {
		struct requested_blob *blob = &batch->blobs[i];
		struct odb_blob_manifest *manifest = &blob->manifest;
		struct object_id oid;
		const char *arg;
		bool unavailable;
		size_t remaining;

		if (packet_reader_read(reader) != PACKET_READ_NORMAL || !text_packet(reader))
			return -1;
		unavailable = skip_prefix(reader->line, "unavailable ", &arg);
		if (!unavailable && !skip_prefix(reader->line, "manifest ", &arg))
			return -1;
		if (parse_oid_hex_algop(arg, &oid, &arg, repo->hash_algo) ||
		    !oideq(&oid, &blob->oid))
			return -1;
		if (unavailable) {
			if (*arg)
				return -1;
			continue;
		}
		if (*arg++ != ' ' ||
		    read_number(&arg, MAX_BLOB_BYTES - blob_bytes, &manifest->size) ||
		    manifest->size <= ODB_FASTCDC_MAX ||
		    *arg++ != ' ' ||
		    read_number(&arg, MAX_CHUNKS - nr_chunks, &manifest->nr) ||
		    !manifest->nr || *arg)
			return -1;

		nr_chunks += manifest->nr;
		blob_bytes += manifest->size;
		remaining = manifest->size;
		ALLOC_ARRAY(manifest->chunks, manifest->nr);
		for (size_t j = 0; j < manifest->nr; j++) {
			struct odb_blob_chunk *chunk = &manifest->chunks[j];
			size_t len;

			if (packet_reader_read(reader) != PACKET_READ_NORMAL ||
			    !text_packet(reader) ||
			    !skip_prefix(reader->line, "chunk ", &arg) ||
			    read_number(&arg, ODB_FASTCDC_MAX, &len) ||
			    !len || len > remaining ||
			    (j + 1 < manifest->nr && len < ODB_FASTCDC_MIN) ||
			    *arg++ != ' ' || strlen(arg) != GIT_SHA256_HEXSZ ||
			    get_hash_hex_algop(arg, chunk->digest, chunk_hash_algo()))
				return -1;
			chunk->len = len;
			remaining -= len;
		}
		if (remaining)
			return -1;
		manifest_id(manifest, blob->id);
	}
	return read_response_end(reader, stateless_rpc);
}

struct chunk_slot {
	struct oidmap_entry entry;
	off_t offset;
	uint32_t len;
	bool local;
};

static struct chunk_slot *find_chunk(const struct oidmap *slots,
				    const struct odb_blob_chunk *chunk)
{
	struct object_id oid;

	oidread(&oid, chunk->digest, chunk_hash_algo());
	return oidmap_get(slots, &oid);
}

/* Queue each missing digest once across the entire batch. */
static int select_chunks(struct repository *repo, struct request_batch *batch,
			 struct oidmap *slots, unsigned char *raw,
			 size_t *reused, size_t *transfer_bytes)
{
	const char *object_dir = repo_get_object_directory(repo);

	for (size_t i = 0; i < batch->nr; i++) {
		struct requested_blob *blob = &batch->blobs[i];

		for (size_t j = 0; j < blob->manifest.nr; j++) {
			const struct odb_blob_chunk *chunk = &blob->manifest.chunks[j];
			struct chunk_slot *slot = find_chunk(slots, chunk);

			if (slot) {
				if (slot->len != chunk->len)
					return -1;
				continue;
			}
			CALLOC_ARRAY(slot, 1);
			oidread(&slot->entry.oid, chunk->digest, chunk_hash_algo());
			slot->len = chunk->len;
			slot->offset = -1;
			slot->local = !odb_chunked_read_chunk(object_dir, chunk, raw, NULL);
			oidmap_put(slots, slot);
			if (slot->local) {
				(*reused)++;
			} else {
				*transfer_bytes += chunk->len;
				ALLOC_GROW(blob->ordinals, blob->nr + 1, blob->alloc);
				blob->ordinals[blob->nr++] = j;
			}
		}
	}
	return 0;
}

static int read_chunk_payload(struct packet_reader *reader, struct strbuf *encoded,
			      size_t size)
{
	unsigned options = reader->options;
	int ret = -1;

	strbuf_reset(encoded);
	strbuf_grow(encoded, size);
	reader->options &= ~PACKET_READ_CHOMP_NEWLINE;
	reader->options |= PACKET_READ_NO_TRACE;
	while (encoded->len < size) {
		size_t len = MIN(size - encoded->len, LARGE_PACKET_DATA_MAX);

		if (packet_reader_read(reader) != PACKET_READ_NORMAL ||
		    reader->pktlen != (int)len)
			goto out;
		strbuf_add(encoded, reader->line, len);
	}
	ret = 0;

out:
	reader->options = options;
	return ret;
}

static int receive_chunks(struct repository *repo, struct packet_reader *reader,
			   struct request_batch *batch, int stateless_rpc,
			   struct oidmap *slots, unsigned char *raw,
			   int spool, size_t *downloaded)
{
	struct strbuf encoded = STRBUF_INIT;
	off_t offset = 0;
	int ret = -1;

	for (size_t i = 0; i < batch->nr; i++) {
		struct requested_blob *blob = &batch->blobs[i];

		for (size_t j = 0; j < blob->nr; j++) {
			size_t ordinal = blob->ordinals[j], got_ordinal, size;
			const struct odb_blob_chunk *chunk = &blob->manifest.chunks[ordinal];
			struct chunk_slot *slot = find_chunk(slots, chunk);
			struct object_id oid;
			const char *arg;
			bool unavailable;

			if (packet_reader_read(reader) != PACKET_READ_NORMAL || !text_packet(reader))
				goto out;
			unavailable = skip_prefix(reader->line, "unavailable ", &arg);
			if (!unavailable && !skip_prefix(reader->line, "data ", &arg))
				goto out;
			if (parse_oid_hex_algop(arg, &oid, &arg, repo->hash_algo) ||
			    !oideq(&oid, &blob->oid) || *arg++ != ' ' ||
			    read_number(&arg, MAX_CHUNKS - 1, &got_ordinal) ||
			    got_ordinal != ordinal)
				goto out;

			if (unavailable) {
				if (*arg)
					goto out;
				continue;
			}
			if (*arg++ != ' ' || read_number(&arg, 2 * ODB_FASTCDC_MAX, &size) ||
			    !size || *arg ||
			    read_chunk_payload(reader, &encoded, size) ||
			    odb_chunked_inflate(chunk, encoded.buf, encoded.len, raw))
				goto out;
			if (size > 2 * MAX_TRANSFER_BYTES - *downloaded ||
			    write_in_full(spool, raw, chunk->len) < 0)
				goto out;
			slot->offset = offset;
			offset += chunk->len;
			*downloaded += size;
		}
	}
	ret = read_response_end(reader, stateless_rpc);

out:
	strbuf_release(&encoded);
	return ret;
}

struct reconstructed_blob {
	struct odb_stream stream;
	const struct odb_blob_manifest *manifest;
	const struct oidmap *slots;
	const char *object_dir;
	unsigned char *raw;
	int spool;
	size_t next, len, offset;
};

static ssize_t read_reconstructed_blob(struct odb_stream *stream, char *buf, size_t len)
{
	struct reconstructed_blob *input = container_of(stream, struct reconstructed_blob, stream);

	if (input->offset == input->len) {
		const struct odb_blob_chunk *chunk;
		struct chunk_slot *slot;

		if (input->next == input->manifest->nr)
			return 0;
		chunk = &input->manifest->chunks[input->next++];
		slot = find_chunk(input->slots, chunk);
		if (slot->local) {
			if (odb_chunked_read_chunk(input->object_dir, chunk, input->raw, NULL))
				return -1;
		} else if (slot->offset < 0 ||
			   pread_in_full(input->spool, input->raw, chunk->len, slot->offset) != chunk->len) {
			return -1;
		}
		input->offset = 0;
		input->len = chunk->len;
	}

	len = MIN(len, input->len - input->offset);
	memcpy(buf, input->raw + input->offset, len);
	input->offset += len;
	return len;
}

static int install_blob(struct repository *repo, const struct requested_blob *blob,
			 const struct oidmap *slots, unsigned char *raw, int spool)
{
	struct odb_source_files *files = odb_source_files_downcast(repo->objects->source);
	struct reconstructed_blob input = {
		.stream = {
			.type = OBJ_BLOB,
			.size = blob->manifest.size,
			.read = read_reconstructed_blob,
		},
		.manifest = &blob->manifest,
		.slots = slots,
		.object_dir = repo_get_object_directory(repo),
		.raw = raw,
		.spool = spool,
	};

	/*
	 * Verify the requested object before writing anything into the ODB.
	 * The writer also checks the expected OID before publishing its
	 * manifest, in case a local chunk changes between the two reads.
	 */
	if (stream_object_signature(repo, &input.stream, &blob->oid))
		return -1;
	input.next = input.len = input.offset = 0;
	return odb_chunked_write(files->dirs->loose, &input.stream, &blob->oid,
				 NULL, NULL, ODB_WRITE_OBJECT_SILENT, NULL);
}

int fetch_blob_chunks(struct repository *repo, int fd[2], int stateless_rpc,
		      const struct string_list *server_options,
		      struct ref **wants, size_t nr_wants, struct oidset *fetched)
{
	struct request_batch batch = { 0 };
	struct oidset requested = OIDSET_INIT;
	struct oidset promised = OIDSET_INIT;
	struct oidmap slots = OIDMAP_INIT;
	struct packet_reader reader;
	struct strbuf request = STRBUF_INIT;
	struct tempfile *spool = NULL;
	unsigned char *raw = NULL;
	size_t reused = 0, transfer_bytes = 0, downloaded = 0;
	int spool_fd = -1, ret = -1;

	/*
	 * A loose manifest carries no promisor marker. Use the pack path
	 * when the client has no local record of the promise.
	 */
	for (size_t i = 0; i < nr_wants && oidset_size(&requested) < MAX_BLOBS; i++) {
		const struct object_id *oid = &wants[i]->old_oid;

		if (!odb_has_object(repo->objects, oid, 0))
			oidset_insert(&requested, oid);
	}
	collect_local_promisor_blobs(repo, &requested, &promised, MAX_PROMISOR_ENTRIES);
	for (size_t i = 0; i < nr_wants && oidset_size(&promised); i++) {
		const struct object_id *oid = &wants[i]->old_oid;

		if (oidset_remove(&promised, oid))
			oidcpy(&batch.blobs[batch.nr++].oid, oid);
	}
	oidset_clear(&promised);
	if (!batch.nr) {
		ret = 0;
		goto out;
	}

	packet_reader_init(&reader, fd[0], NULL, 0, PACKET_READ_CHOMP_NEWLINE |
			   PACKET_READ_GENTLE_ON_EOF | PACKET_READ_GENTLE_ON_READ_ERROR);
	write_command_and_capabilities(&request, "blob-chunks", server_options);
	packet_buf_write(&request, "manifest\n");
	for (size_t i = 0; i < batch.nr; i++)
		packet_buf_write(&request, "oid %s\n", oid_to_hex(&batch.blobs[i].oid));
	if (send_request(fd[1], &request) || read_manifests(repo, &reader, &batch, stateless_rpc))
		goto out;

	raw = xmalloc(ODB_FASTCDC_MAX);
	if (select_chunks(repo, &batch, &slots, raw, &reused, &transfer_bytes))
		goto out;
	if (transfer_bytes > MAX_TRANSFER_BYTES) {
		ret = 1;
		goto out;
	}

	if (transfer_bytes) {
		spool = mks_tempfile_t("blob-chunks-XXXXXX");
		if (!spool) {
			ret = 1;
			goto out;
		}
		spool_fd = get_tempfile_fd(spool);
		strbuf_reset(&request);
		write_command_and_capabilities(&request, "blob-chunks", server_options);
		packet_buf_write(&request, "chunks\n");
		for (size_t i = 0; i < batch.nr; i++) {
			const struct requested_blob *blob = &batch.blobs[i];

			if (!blob->nr)
				continue;
			packet_buf_write(&request, "oid %s %s\n", oid_to_hex(&blob->oid),
					 hash_to_hex_algop(blob->id, chunk_hash_algo()));
			for (size_t j = 0; j < blob->nr; j++)
				packet_buf_write(&request, "chunk %"PRIuMAX"\n",
						 (uintmax_t)blob->ordinals[j]);
		}
		if (send_request(fd[1], &request) ||
		    receive_chunks(repo, &reader, &batch, stateless_rpc, &slots, raw,
				   spool_fd, &downloaded))
			goto out;
	}

	for (size_t i = 0; i < batch.nr; i++) {
		const struct requested_blob *blob = &batch.blobs[i];

		if (blob->manifest.nr && !install_blob(repo, blob, &slots, raw, spool_fd))
			oidset_insert(fetched, &blob->oid);
	}

	ret = 1;
	trace2_data_intmax("blob-chunks", repo, "reused_chunks", reused);
	trace2_data_intmax("blob-chunks", repo, "received_bytes", downloaded);
	trace2_data_intmax("blob-chunks", repo, "received_blobs", oidset_size(fetched));

out:
	free(raw);
	delete_tempfile(&spool);
	strbuf_release(&request);
	oidmap_clear(&slots, 1);
	oidset_clear(&requested);
	release_batch(&batch);
	return ret;
}
