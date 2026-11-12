#ifndef BLOB_CHUNKS_H
#define BLOB_CHUNKS_H

struct oidset;
struct packet_reader;
struct ref;
struct repository;
struct strbuf;
struct string_list;

/* Exit status used by an internal lazy fetch to request a pack-only retry. */
#define BLOB_CHUNKS_RETRY 75
#define BLOB_CHUNKS_ENVIRONMENT "GIT_INTERNAL_BLOB_CHUNKS"

int blob_chunks_advertise(struct repository *repo, struct strbuf *value);
int serve_blob_chunks(struct repository *repo, struct packet_reader *request);

/* Return 1 after an exchange, 0 if no request was sent, or -1 on error. */
int fetch_blob_chunks(struct repository *repo, int fd[2], int stateless_rpc,
		      const struct string_list *server_options,
		      struct ref **wants, size_t nr_wants, struct oidset *fetched);

#endif
