#ifndef BLOB_CHUNKS_H
#define BLOB_CHUNKS_H

struct packet_reader;
struct repository;
struct strbuf;

int blob_chunks_advertise(struct repository *repo, struct strbuf *value);
int serve_blob_chunks(struct repository *repo, struct packet_reader *request);

#endif
