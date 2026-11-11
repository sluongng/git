#include "git-compat-util.h"
#include "hash.h"
#include "path.h"
#include "object-file.h"
#include "odb.h"
#include "odb/source-files.h"
#include "hex.h"
#include "repository.h"
#include "wrapper.h"
#include "gettext.h"
#include "loose.h"
#include "lockfile.h"
#include "oidtree.h"
#include "write-or-die.h"

static const char *loose_object_header = "# loose-object-idx\n";

static inline int should_use_loose_object_map(struct repository *repo)
{
	return repo->compat_hash_algo && repo->gitdir;
}

void loose_object_map_init(struct loose_object_map **map)
{
	struct loose_object_map *m;
	m = xmalloc(sizeof(**map));
	m->to_compat = kh_init_oid_map();
	m->to_storage = kh_init_oid_map();
	*map = m;
}

static int insert_oid_pair(kh_oid_map_t *map, const struct object_id *key, const struct object_id *value)
{
	khiter_t pos;
	int ret;
	struct object_id *stored;

	pos = kh_put_oid_map(map, *key, &ret);

	/* This item already exists in the map. */
	if (ret == 0)
		return 0;

	stored = xmalloc(sizeof(*stored));
	oidcpy(stored, value);
	kh_value(map, pos) = stored;
	return 1;
}

static void ensure_loose_map_cache(struct odb_source_loose *loose)
{
	if (!loose->cache) {
		ALLOC_ARRAY(loose->cache, 1);
		oidtree_init(loose->cache);
	}
}

static int insert_loose_map(struct odb_source_loose *loose,
			    const struct object_id *oid,
			    const struct object_id *compat_oid)
{
	struct loose_object_map *map = loose->map;
	int inserted = 0;

	inserted |= insert_oid_pair(map->to_compat, oid, compat_oid);
	inserted |= insert_oid_pair(map->to_storage, compat_oid, oid);
	if (inserted) {
		ensure_loose_map_cache(loose);
		oidtree_insert(loose->cache, compat_oid, NULL);
	}

	return inserted;
}

int loose_object_map_load(struct odb_source_loose *loose)
{
	struct repository *repo = loose->base.odb->repo;
	struct strbuf buf = STRBUF_INIT;
	char *path;
	FILE *fp;
	int ret = -1;

	if (!should_use_loose_object_map(repo))
		return 0;

	if (!loose->map)
		loose_object_map_init(&loose->map);
	ensure_loose_map_cache(loose);

	insert_loose_map(loose, repo->hash_algo->empty_tree, repo->compat_hash_algo->empty_tree);
	insert_loose_map(loose, repo->hash_algo->empty_blob, repo->compat_hash_algo->empty_blob);
	insert_loose_map(loose, repo->hash_algo->null_oid, repo->compat_hash_algo->null_oid);

	path = xstrfmt("%s/loose-object-idx", loose->base.path);
	fp = fopen(path, "rb");
	if (!fp) {
		ret = errno == ENOENT ? 0 : -1;
		free(path);
		return ret;
	}

	if (strbuf_getwholeline(&buf, fp, '\n') || strcmp(buf.buf, loose_object_header))
		goto err;
	while (!strbuf_getline_lf(&buf, fp)) {
		const char *p;
		struct object_id oid, compat_oid;
		if (parse_oid_hex_algop(buf.buf, &oid, &p, repo->hash_algo) ||
		    *p++ != ' ' ||
		    parse_oid_hex_algop(p, &compat_oid, &p, repo->compat_hash_algo) ||
		    p != buf.buf + buf.len)
			goto err;
		insert_loose_map(loose, &oid, &compat_oid);
	}

	ret = ferror(fp) ? -1 : 0;
err:
	fclose(fp);
	strbuf_release(&buf);
	free(path);
	return ret;
}

int repo_read_loose_object_map(struct repository *repo)
{
	struct odb_source_files *files = odb_source_files_downcast(repo->objects->source);

	for (struct odb_files_dir *dir = files->dirs; dir; dir = dir->next) {
		if (loose_object_map_load(dir->loose) < 0)
			return -1;
	}

	return 0;
}

static int write_one_object(struct odb_source_loose *loose,
			    const struct object_id *oid,
			    const struct object_id *compat_oid)
{
	struct lock_file lock;
	int fd, close_result;
	struct stat st;
	struct strbuf buf = STRBUF_INIT, path = STRBUF_INIT;

	strbuf_addf(&path, "%s/loose-object-idx", loose->base.path);
	repo_hold_lock_file_for_update_timeout(loose->base.odb->repo, &lock,
					       path.buf, LOCK_DIE_ON_ERROR, -1);

	fd = open(path.buf, O_WRONLY | O_CREAT | O_APPEND, 0666);
	if (fd < 0)
		goto errout;
	if (fstat(fd, &st) < 0)
		goto errout;
	if (!st.st_size)
		strbuf_addstr(&buf, loose_object_header);
	strbuf_addf(&buf, "%s %s\n", oid_to_hex(oid), oid_to_hex(compat_oid));
	if (write_in_full(fd, buf.buf, buf.len) < 0) {
		int saved_errno = errno;

		/* A partial mapping would prevent readers from loading later entries. */
		if (ftruncate(fd, st.st_size))
			warning_errno(_("unable to roll back loose object index %s"), path.buf);
		errno = saved_errno;
		goto errout;
	}
	close_result = close(fd);
	fd = -1;
	if (close_result)
		goto errout;
	adjust_shared_perm(loose->base.odb->repo, path.buf);
	rollback_lock_file(&lock);
	strbuf_release(&buf);
	strbuf_release(&path);
	return 0;
errout:
	error_errno(_("failed to write loose object index %s"), path.buf);
	if (fd >= 0)
		close(fd);
	rollback_lock_file(&lock);
	strbuf_release(&buf);
	strbuf_release(&path);
	return -1;
}

int repo_add_loose_object_map(struct odb_source_loose *loose,
			      const struct object_id *oid,
			      const struct object_id *compat_oid)
{
	struct loose_object_map *map = loose->map;

	if (!should_use_loose_object_map(loose->base.odb->repo))
		return 0;

	if (kh_get_oid_map(map->to_compat, *oid) < kh_end(map->to_compat) &&
	    kh_get_oid_map(map->to_storage, *compat_oid) < kh_end(map->to_storage))
		return 0;
	if (write_one_object(loose, oid, compat_oid))
		return -1;
	insert_loose_map(loose, oid, compat_oid);
	return 0;
}

int loose_object_map_fsync(struct odb_source_loose *loose)
{
	char *path;
	int fd, ret = 0;

	if (fsync_object_files <= 0 &&
	    !(fsync_components & (FSYNC_COMPONENT_LOOSE_OBJECT | FSYNC_COMPONENT_OBJECT_MAP)))
		return 0;

	path = xstrfmt("%s/loose-object-idx", loose->base.path);
	fd = open(path, O_WRONLY);
	if (fd < 0) {
		if (errno != ENOENT)
			ret = error_errno(_("unable to open loose object index %s"), path);
	} else {
		if (fsync_file(fd) < 0)
			ret = error_errno(_("unable to fsync loose object index %s"), path);
		if (close(fd) < 0 && !ret)
			ret = error_errno(_("unable to close loose object index %s"), path);
	}
	free(path);
	return ret;
}

int repo_loose_object_map_oid(struct repository *repo,
			      const struct object_id *src,
			      const struct git_hash_algo *to,
			      struct object_id *dest)
{
	struct odb_source_files *files = odb_source_files_downcast(repo->objects->source);
	kh_oid_map_t *map;
	khiter_t pos;

	for (struct odb_files_dir *dir = files->dirs; dir; dir = dir->next) {
		struct loose_object_map *loose_map = dir->loose->map;
		if (!loose_map)
			continue;
		map = (to == repo->compat_hash_algo) ?
			loose_map->to_compat :
			loose_map->to_storage;
		pos = kh_get_oid_map(map, *src);
		if (pos < kh_end(map)) {
			oidcpy(dest, kh_value(map, pos));
			return 0;
		}
	}
	return -1;
}

void loose_object_map_clear(struct loose_object_map **map)
{
	struct loose_object_map *m = *map;
	struct object_id *oid;

	if (!m)
		return;

	kh_foreach_value(m->to_compat, oid, free(oid));
	kh_foreach_value(m->to_storage, oid, free(oid));
	kh_destroy_oid_map(m->to_compat);
	kh_destroy_oid_map(m->to_storage);
	free(m);
	*map = NULL;
}
