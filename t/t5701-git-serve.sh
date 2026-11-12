#!/bin/sh

test_description='test protocol v2 server commands'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-filev2.sh

unknown_oid=$(printf "test" | git hash-object --stdin)

test_expect_success 'setup to generate files with expected content' '
	printf "agent=git/%s" "$(git version | cut -d" " -f3)" >agent_capability &&

	test_oid_cache <<-EOF &&
	wrong_algo sha1:sha256
	wrong_algo sha256:sha1
	EOF

	if test_have_prereq WINDOWS
	then
		printf "agent=FAKE\n" >agent_capability
	else
		printf -- "-%s\n" $(uname -s | test_redact_non_printables) >>agent_capability
	fi &&
	cat >expect.base <<-EOF &&
	version 2
	$(cat agent_capability)
	ls-refs=unborn
	fetch=shallow wait-for-done
	server-option
	object-format=$(test_oid algo)
	EOF
	cat >expect.trailer <<-EOF
	0000
	EOF
'

test_expect_success 'test capability advertisement' '
	cat expect.base expect.trailer >expect &&

	if test_have_prereq WINDOWS
	then
		GIT_USER_AGENT=FAKE && export GIT_USER_AGENT
	fi &&
	GIT_TEST_SIDEBAND_ALL=0 test-tool serve-v2 \
		--advertise-capabilities >out &&
	test-tool pkt-line unpack <out >actual &&
	test_cmp expect actual
'

test_expect_success 'stateless-rpc flag does not list capabilities' '
	# Empty request
	test-tool pkt-line pack >in <<-EOF &&
	0000
	EOF
	test-tool serve-v2 --stateless-rpc >out <in &&
	test_must_be_empty out &&

	# EOF
	test-tool serve-v2 --stateless-rpc >out &&
	test_must_be_empty out
'

test_expect_success 'request invalid capability' '
	test-tool pkt-line pack >in <<-EOF &&
	foobar
	0000
	EOF
	test_must_fail test-tool serve-v2 --stateless-rpc 2>err <in &&
	test_grep "unknown capability" err
'

test_expect_success 'promisor-remote capability requires an argument' '
	test-tool pkt-line pack >in <<-EOF &&
	command=ls-refs
	object-format=$(test_oid algo)
	promisor-remote
	0000
	EOF
	test_must_fail test-tool serve-v2 --stateless-rpc 2>err <in &&
	test_grep "promisor-remote capability requires an argument" err
'

test_expect_success 'request with no command' '
	test-tool pkt-line pack >in <<-EOF &&
	agent=git/test
	object-format=$(test_oid algo)
	0000
	EOF
	test_must_fail test-tool serve-v2 --stateless-rpc 2>err <in &&
	test_grep "no command requested" err
'

test_expect_success 'request invalid command' '
	test-tool pkt-line pack >in <<-EOF &&
	command=foo
	object-format=$(test_oid algo)
	agent=git/test
	0000
	EOF
	test_must_fail test-tool serve-v2 --stateless-rpc 2>err <in &&
	test_grep "invalid command" err
'

test_expect_success 'request capability as command' '
	test-tool pkt-line pack >in <<-EOF &&
	command=agent
	object-format=$(test_oid algo)
	0000
	EOF
	test_must_fail test-tool serve-v2 --stateless-rpc 2>err <in &&
	test_grep invalid.command.*agent err
'

test_expect_success 'request command as capability' '
	test-tool pkt-line pack >in <<-EOF &&
	command=ls-refs
	object-format=$(test_oid algo)
	fetch
	0000
	EOF
	test_must_fail test-tool serve-v2 --stateless-rpc 2>err <in &&
	test_grep unknown.capability err
'

test_expect_success 'requested command is command=value' '
	test-tool pkt-line pack >in <<-EOF &&
	command=ls-refs=whatever
	object-format=$(test_oid algo)
	0000
	EOF
	test_must_fail test-tool serve-v2 --stateless-rpc 2>err <in &&
	test_grep invalid.command.*ls-refs=whatever err
'

test_expect_success 'wrong object-format' '
	test-tool pkt-line pack >in <<-EOF &&
	command=fetch
	agent=git/test
	object-format=$(test_oid wrong_algo)
	0000
	EOF
	test_must_fail test-tool serve-v2 --stateless-rpc 2>err <in &&
	test_grep "mismatched object format" err
'

# Test the basics of ls-refs
#
test_expect_success 'setup some refs and tags' '
	test_commit one &&
	git branch dev main &&
	test_commit two &&
	git symbolic-ref refs/heads/release refs/heads/main &&
	git tag -a -m "annotated tag" annotated-tag
'

test_expect_success 'basics of ls-refs' '
	test-tool pkt-line pack >in <<-EOF &&
	command=ls-refs
	object-format=$(test_oid algo)
	0000
	EOF

	cat >expect <<-EOF &&
	$(git rev-parse HEAD) HEAD
	$(git rev-parse refs/heads/dev) refs/heads/dev
	$(git rev-parse refs/heads/main) refs/heads/main
	$(git rev-parse refs/heads/release) refs/heads/release
	$(git rev-parse refs/tags/annotated-tag) refs/tags/annotated-tag
	$(git rev-parse refs/tags/one) refs/tags/one
	$(git rev-parse refs/tags/two) refs/tags/two
	0000
	EOF

	test-tool serve-v2 --stateless-rpc <in >out &&
	test-tool pkt-line unpack <out >actual &&
	test_cmp expect actual
'

test_expect_success 'ls-refs complains about unknown options' '
	test-tool pkt-line pack >in <<-EOF &&
	command=ls-refs
	object-format=$(test_oid algo)
	0001
	no-such-arg
	0000
	EOF

	test_must_fail test-tool serve-v2 --stateless-rpc 2>err <in &&
	test_grep unexpected.line.*no-such-arg err
'

test_expect_success 'basic ref-prefixes' '
	test-tool pkt-line pack >in <<-EOF &&
	command=ls-refs
	object-format=$(test_oid algo)
	0001
	ref-prefix refs/heads/main
	ref-prefix refs/tags/one
	0000
	EOF

	cat >expect <<-EOF &&
	$(git rev-parse refs/heads/main) refs/heads/main
	$(git rev-parse refs/tags/one) refs/tags/one
	0000
	EOF

	test-tool serve-v2 --stateless-rpc <in >out &&
	test-tool pkt-line unpack <out >actual &&
	test_cmp expect actual
'

test_expect_success 'refs/heads prefix' '
	test-tool pkt-line pack >in <<-EOF &&
	command=ls-refs
	object-format=$(test_oid algo)
	0001
	ref-prefix refs/heads/
	0000
	EOF

	cat >expect <<-EOF &&
	$(git rev-parse refs/heads/dev) refs/heads/dev
	$(git rev-parse refs/heads/main) refs/heads/main
	$(git rev-parse refs/heads/release) refs/heads/release
	0000
	EOF

	test-tool serve-v2 --stateless-rpc <in >out &&
	test-tool pkt-line unpack <out >actual &&
	test_cmp expect actual
'

test_expect_success 'ignore very large set of prefixes' '
	# generate a large number of ref-prefixes that we expect
	# to match nothing; the value here exceeds TOO_MANY_PREFIXES
	# from ls-refs.c.
	{
		echo command=ls-refs &&
		echo object-format=$(test_oid algo) &&
		echo 0001 &&
		awk "{
			for (i = 1; i <= 65536; i++)
				print \"ref-prefix refs/heads/\", \$i
		}" &&
		echo 0000
	} |
	test-tool pkt-line pack >in &&

	# and then confirm that we see unmatched prefixes anyway (i.e.,
	# that the prefix was not applied).
	cat >expect <<-EOF &&
	$(git rev-parse HEAD) HEAD
	$(git rev-parse refs/heads/dev) refs/heads/dev
	$(git rev-parse refs/heads/main) refs/heads/main
	$(git rev-parse refs/heads/release) refs/heads/release
	$(git rev-parse refs/tags/annotated-tag) refs/tags/annotated-tag
	$(git rev-parse refs/tags/one) refs/tags/one
	$(git rev-parse refs/tags/two) refs/tags/two
	0000
	EOF

	test-tool serve-v2 --stateless-rpc <in >out &&
	test-tool pkt-line unpack <out >actual &&
	test_cmp expect actual
'

test_expect_success 'peel parameter' '
	test-tool pkt-line pack >in <<-EOF &&
	command=ls-refs
	object-format=$(test_oid algo)
	0001
	peel
	ref-prefix refs/tags/
	0000
	EOF

	cat >expect <<-EOF &&
	$(git rev-parse refs/tags/annotated-tag) refs/tags/annotated-tag peeled:$(git rev-parse refs/tags/annotated-tag^{})
	$(git rev-parse refs/tags/one) refs/tags/one
	$(git rev-parse refs/tags/two) refs/tags/two
	0000
	EOF

	test-tool serve-v2 --stateless-rpc <in >out &&
	test-tool pkt-line unpack <out >actual &&
	test_cmp expect actual
'

test_expect_success 'symrefs parameter' '
	test-tool pkt-line pack >in <<-EOF &&
	command=ls-refs
	object-format=$(test_oid algo)
	0001
	symrefs
	ref-prefix refs/heads/
	0000
	EOF

	cat >expect <<-EOF &&
	$(git rev-parse refs/heads/dev) refs/heads/dev
	$(git rev-parse refs/heads/main) refs/heads/main
	$(git rev-parse refs/heads/release) refs/heads/release symref-target:refs/heads/main
	0000
	EOF

	test-tool serve-v2 --stateless-rpc <in >out &&
	test-tool pkt-line unpack <out >actual &&
	test_cmp expect actual
'

test_expect_success 'sending server-options' '
	test-tool pkt-line pack >in <<-EOF &&
	command=ls-refs
	object-format=$(test_oid algo)
	server-option=hello
	server-option=world
	0001
	ref-prefix HEAD
	0000
	EOF

	cat >expect <<-EOF &&
	$(git rev-parse HEAD) HEAD
	0000
	EOF

	test-tool serve-v2 --stateless-rpc <in >out &&
	test-tool pkt-line unpack <out >actual &&
	test_cmp expect actual
'

test_expect_success 'unexpected lines are not allowed in fetch request' '
	git init server &&

	test-tool pkt-line pack >in <<-EOF &&
	command=fetch
	object-format=$(test_oid algo)
	0001
	this-is-not-a-command
	0000
	EOF

	(
		cd server &&
		test_must_fail test-tool serve-v2 --stateless-rpc
	) <in >/dev/null 2>err &&
	test_grep "unexpected line: .this-is-not-a-command." err
'

# Test the basics of object-info
#
test_expect_success 'basics of object-info' '
	test_config transfer.advertiseObjectInfo true &&

	two_oid=$(git rev-parse two:two.t) &&
	two_size=$(test_file_size two.t) &&

	test-tool pkt-line pack >in <<-EOF &&
	command=object-info
	object-format=$(test_oid algo)
	0001
	size
	oid $two_oid
	oid $two_oid
	0000
	EOF

	cat >expect <<-EOF &&
	size
	$two_oid $two_size
	$two_oid $two_size
	0000
	EOF

	test-tool serve-v2 --stateless-rpc <in >out &&
	test-tool pkt-line unpack <out >actual &&
	test_cmp expect actual
'

test_expect_success 'object-info supports type' '
	test_config transfer.advertiseObjectInfo true &&

	two_oid=$(git rev-parse two:two.t) &&
	two_size=$(test_file_size two.t) &&

	test-tool pkt-line pack >in <<-EOF &&
	command=object-info
	object-format=$(test_oid algo)
	0001
	size
	type
	oid $two_oid
	oid $two_oid
	0000
	EOF

	cat >expect <<-EOF &&
	size
	type
	$two_oid $two_size blob
	$two_oid $two_size blob
	0000
	EOF

	test-tool serve-v2 --stateless-rpc <in >out &&
	test-tool pkt-line unpack <out >actual &&
	test_cmp expect actual
'

test_expect_success 'bare OID request' '
	test_config transfer.advertiseObjectInfo true &&

	test-tool pkt-line pack >in <<-EOF &&
	command=object-info
	object-format=$(test_oid algo)
	0001
	oid $(git rev-parse two:two.t)
	0000
	EOF

	cat >expect <<-EOF &&
	$(git rev-parse two:two.t)
	0000
	EOF

	test-tool serve-v2 --stateless-rpc <in >out &&
	test-tool pkt-line unpack <out >actual &&
	test_cmp expect actual
'

test_expect_success 'object-info with bare unrecognized OID' '
	test_config transfer.advertiseObjectInfo true &&

	test-tool pkt-line pack >in <<-EOF &&
	command=object-info
	object-format=$(test_oid algo)
	0001
	oid $unknown_oid
	0000
	EOF

	printf "%s \n" "$unknown_oid" >expect &&
	printf "0000\n" >>expect &&

	test-tool serve-v2 --stateless-rpc <in >out &&
	test-tool pkt-line unpack <out >actual &&
	test_cmp expect actual
'

test_expect_success 'object-info with size for unrecognized OID' '
	test_config transfer.advertiseObjectInfo true &&

	test-tool pkt-line pack >in <<-EOF &&
	command=object-info
	object-format=$(test_oid algo)
	0001
	size
	oid $unknown_oid
	0000
	EOF

	printf "size\n" >expect &&
	printf "%s \n" "$unknown_oid" >>expect &&
	printf "0000\n" >>expect &&

	test-tool serve-v2 --stateless-rpc <in >out &&
	test-tool pkt-line unpack <out >actual &&
	test_cmp expect actual
'

test_expect_success 'test capability advertisement with uploadpack.advertiseBundleURIs' '
	test_config uploadpack.advertiseBundleURIs true &&

	cat >expect.extra <<-EOF &&
	bundle-uri
	EOF
	cat expect.base \
	    expect.extra \
	    expect.trailer >expect &&

	if test_have_prereq WINDOWS
	then
		GIT_USER_AGENT=FAKE && export GIT_USER_AGENT
	fi &&
	GIT_TEST_SIDEBAND_ALL=0 test-tool serve-v2 \
		--advertise-capabilities >out &&
	test-tool pkt-line unpack <out >actual &&
	test_cmp expect actual
'

test_expect_success 'basics of bundle-uri: dies if not enabled' '
	test-tool pkt-line pack >in <<-EOF &&
	command=bundle-uri
	0000
	EOF

	cat >err.expect <<-\EOF &&
	fatal: invalid command '"'"'bundle-uri'"'"'
	EOF

	cat >expect <<-\EOF &&
	ERR serve: invalid command '"'"'bundle-uri'"'"'
	EOF

	test_must_fail test-tool serve-v2 --stateless-rpc <in >out 2>err.actual &&
	test_cmp err.expect err.actual &&
	test_must_be_empty out
'

test_expect_success 'object-info missing from capabilities when disabled' '
	test_config transfer.advertiseObjectInfo false &&

	GIT_TEST_SIDEBAND_ALL=0 test-tool serve-v2 \
		--advertise-capabilities >out &&
	test-tool pkt-line unpack <out >actual &&

	test_grep ! object.info actual
'

test_expect_success 'object-info commands rejected when disabled' '
	test_config transfer.advertiseObjectInfo false &&

	test-tool pkt-line pack >in <<-EOF &&
	command=object-info
	EOF

	test_must_fail test-tool serve-v2 --stateless-rpc <in 2>err &&
	test_grep invalid.command err
'

test_expect_success 'blob-chunks is advertised by an opted-in filev2 server' '
	test_filev2_server blob-chunks-server &&
	test-tool -C blob-chunks-server serve-v2 --advertise-capabilities >out &&
	test-tool pkt-line unpack <out >actual &&
	test_grep "^blob-chunks=1$" actual &&
	git -C blob-chunks-server config uploadpack.allowBlobChunks false &&
	test-tool -C blob-chunks-server serve-v2 --advertise-capabilities >out &&
	test-tool pkt-line unpack <out >actual &&
	test_grep ! "^blob-chunks=" actual &&
	git -C blob-chunks-server config uploadpack.allowBlobChunks true
'

test_expect_success 'protected pack-objects hook disables blob-chunks' '
	blob=$(git -C blob-chunks-server rev-parse main:large) &&
	test-tool pkt-line pack >hook-request <<-EOF &&
	command=blob-chunks
	object-format=$(test_oid algo)
	0001
	manifest
	oid $blob
	0000
	EOF
	test_config -C blob-chunks-server uploadpack.packObjectsHook false &&
	GIT_PROTOCOL=version=2 git -C blob-chunks-server upload-pack \
		--stateless-rpc . <hook-request >out &&
	test-tool pkt-line unpack <out >actual &&
	test_grep "^manifest $blob " actual &&

	test_config_global uploadpack.packObjectsHook false &&
	GIT_PROTOCOL=version=2 git -C blob-chunks-server upload-pack \
		--stateless-rpc --advertise-refs . >out &&
	test-tool pkt-line unpack <out >actual &&
	test_grep ! "^blob-chunks=" actual &&
	test_must_fail env GIT_PROTOCOL=version=2 git -C blob-chunks-server upload-pack \
		--stateless-rpc . <hook-request >out 2>err &&
	test_grep "invalid command.*blob-chunks" err
'

test_expect_success 'blob-chunks manifests honor hidden refs and reachability' '
	public_blob=$(git -C blob-chunks-server rev-parse main:large) &&
	git -C blob-chunks-server switch -c secret &&
	echo hidden >>blob-chunks-server/large &&
	git -C blob-chunks-server commit -am hidden &&
	hidden_blob=$(git -C blob-chunks-server rev-parse secret:large) &&
	echo unreachable >>blob-chunks-server/large &&
	unreachable_blob=$(git -C blob-chunks-server hash-object -w large) &&
	git -C blob-chunks-server restore large &&
	git -C blob-chunks-server switch main &&
	git -C blob-chunks-server config uploadpack.hideRefs refs/heads/secret &&
	test-tool pkt-line pack >in <<-EOF &&
	command=blob-chunks
	object-format=$(test_oid algo)
	0001
	manifest
	oid $public_blob
	oid $hidden_blob
	oid $unreachable_blob
	0000
	EOF
	test-tool -C blob-chunks-server serve-v2 --stateless-rpc <in >out &&
	test-tool pkt-line unpack <out >actual &&
	test_grep "^manifest $public_blob 10485761 9$" actual &&
	test_grep "^unavailable $hidden_blob$" actual &&
	test_grep "^unavailable $unreachable_blob$" actual
'

test_expect_success 'blob-chunks authorizes only blobs in the active namespace' '
	namespace_tip=$(git -C blob-chunks-server commit-tree "secret^{tree}" -m namespaced) &&
	git -C blob-chunks-server update-ref refs/namespaces/private/refs/heads/main "$namespace_tip" &&
	GIT_NAMESPACE=private test-tool -C blob-chunks-server serve-v2 --stateless-rpc <in >out &&
	test-tool pkt-line unpack <out >actual &&
	test_grep "^unavailable $public_blob$" actual &&
	test_grep "^manifest $hidden_blob " actual
'

test_expect_success 'blob-chunks serves a local blob from a partial clone' '
	test_filev2_server blob-chunks-full &&
	echo omitted >blob-chunks-full/absent &&
	git -C blob-chunks-full add absent &&
	git -C blob-chunks-full commit -m absent &&
	git clone --no-local --no-checkout --filter=blob:none \
		--object-storage-format=filev2 blob-chunks-full blob-chunks-partial &&
	git -C blob-chunks-partial config uploadpack.allowBlobChunks true &&
	blob=$(git -C blob-chunks-partial hash-object -w --stdin <blob-chunks-full/large) &&
	missing=$(git -C blob-chunks-partial rev-parse HEAD:absent) &&
	test_must_fail env GIT_NO_LAZY_FETCH=1 git -C blob-chunks-partial cat-file -e "$missing" &&
	test-tool pkt-line pack >partial-request <<-EOF &&
	command=blob-chunks
	object-format=$(test_oid algo)
	0001
	manifest
	oid $blob
	oid $missing
	0000
	EOF
	GIT_PROTOCOL=version=2 git -C blob-chunks-partial upload-pack \
		--stateless-rpc . <partial-request >out &&
	test-tool pkt-line unpack <out >actual &&
	test_grep "^manifest $blob " actual &&
	test_grep "^unavailable $missing$" actual &&
	test_must_fail env GIT_NO_LAZY_FETCH=1 git -C blob-chunks-partial cat-file -e "$missing"
'

blob_chunks_manifest_id () {
	manifest=blob-chunks-server/.git/objects/$(test_oid_to_path "$1") &&
	size=$(test_file_size "$manifest") &&
	{
		printf "blob-chunks-v1\0" &&
		dd if="$manifest" bs=1 skip=8 count="$((size - 40))" 2>/dev/null
	} >manifest-id-input &&
	test-tool sha256 <manifest-id-input
}

test_expect_success 'blob-chunks data requests recheck reachability and manifest ID' '
	public_id=$(blob_chunks_manifest_id "$public_blob") &&
	hidden_id=$(blob_chunks_manifest_id "$hidden_blob") &&
	test-tool pkt-line pack >in <<-EOF &&
	command=blob-chunks
	object-format=$(test_oid algo)
	0001
	chunks
	oid $public_blob $public_id
	chunk 0
	oid $hidden_blob $hidden_id
	chunk 0
	0000
	EOF
	GIT_TRACE_PACKET="$PWD/chunk-data.pkt" \
		test-tool -C blob-chunks-server serve-v2 --stateless-rpc <in >out &&
	test_grep "> data $public_blob 0 " chunk-data.pkt &&
	test_grep "> unavailable $hidden_blob 0$" chunk-data.pkt &&
	sed "s/$public_id/$hidden_id/" <in >stale &&
	test-tool -C blob-chunks-server serve-v2 --stateless-rpc <stale >out &&
	test-tool pkt-line unpack <out >actual &&
	test_grep "^unavailable $public_blob 0$" actual
'

test_expect_success 'blob-chunks rejects non-increasing ordinals' '
	test-tool pkt-line pack >in <<-EOF &&
	command=blob-chunks
	object-format=$(test_oid algo)
	0001
	chunks
	oid $public_blob $public_id
	chunk 1
	chunk 1
	0000
	EOF
	test_must_fail test-tool -C blob-chunks-server serve-v2 --stateless-rpc <in >out 2>err &&
	test_grep "invalid blob-chunks request" err &&
	test_must_be_empty out
'

test_done
