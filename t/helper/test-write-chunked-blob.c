#define USE_THE_REPOSITORY_VARIABLE

#include "test-tool.h"
#include "hex.h"
#include "odb/chunked.h"
#include "odb/source-files.h"
#include "odb/streaming.h"
#include "repository.h"
#include "setup.h"

static ssize_t read_blob(struct odb_stream *stream UNUSED, char *buf, size_t len)
{
	return xread(0, buf, len);
}

int cmd__write_chunked_blob(int argc, const char **argv)
{
	struct repository *repo = the_repository;
	struct odb_source_files *files;
	struct object_id oid;
	struct stat st;
	struct odb_stream stream = {
		.type = OBJ_BLOB,
		.read = read_blob,
	};

	if (argc != 2)
		usage("test-tool write-chunked-blob <expected-oid> <blob");

	setup_git_directory(repo);
	if (get_oid_hex_algop(argv[1], &oid, repo->hash_algo))
		die("invalid expected object ID: %s", argv[1]);
	if (fstat(0, &st) < 0)
		die_errno("cannot stat blob input");
	if (!S_ISREG(st.st_mode) || (uintmax_t)st.st_size > SIZE_MAX)
		die("blob input must be a regular file of representable size");

	stream.size = st.st_size;
	files = odb_source_files_downcast(repo->objects->source);
	return !!odb_chunked_write(files->dirs->loose, &stream, &oid, NULL,
				   NULL, 0, NULL);
}
