#ifndef UPLOAD_PACK_H
#define UPLOAD_PACK_H

void upload_pack(const int advertise_refs, const int stateless_rpc,
		 const int timeout);

struct repository;
struct packet_reader;
int upload_pack_v2(struct repository *r, struct packet_reader *request);

struct strbuf;
int upload_pack_advertise(struct repository *r,
			  struct strbuf *value);

/* Return the caller-owned pack-objects hook from protected configuration. */
char *upload_pack_get_pack_objects_hook(void);

/* Is this repo trusted for lazy fetching? */
bool upload_pack_lazy_fetch_trusted(struct repository *r);

#endif /* UPLOAD_PACK_H */
