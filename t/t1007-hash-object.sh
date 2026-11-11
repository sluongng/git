#!/bin/sh

test_description="git hash-object"

. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-filev2.sh

echo_without_newline() {
	printf '%s' "$*"
}

test_blob_does_not_exist() {
	test_expect_success 'blob does not exist in database' "
		test_must_fail git cat-file blob $1
	"
}

test_blob_exists() {
	test_expect_success 'blob exists in database' "
		git cat-file blob $1
	"
}

hello_content="Hello World"
example_content="This is an example"

setup_repo() {
	echo_without_newline "$hello_content" > hello
	echo_without_newline "$example_content" > example
}

test_repo=test
push_repo() {
	git init --quiet $test_repo
	cd $test_repo

	setup_repo
}

pop_repo() {
	cd ..
	rm -rf $test_repo
}

test_expect_success 'setup' '
	setup_repo &&
	test_oid_cache <<-EOF
	hello sha1:5e1c309dae7f45e0f39b1bf3ac3cd9db12e7d689
	hello sha256:1e3b6c04d2eeb2b3e45c8a330445404c0b7cc7b257e2b097167d26f5230090c4

	example sha1:ddd3f836d3e3fbb7ae289aa9ae83536f76956399
	example sha256:b44fe1fe65589848253737db859bd490453510719d7424daab03daf0767b85ae

	large5GB sha1:0be2be10a4c8764f32c4bf372a98edc731a4b204
	large5GB sha256:dc18ca621300c8d3cfa505a275641ebab00de189859e022a975056882d313e64
	EOF
'

# Argument checking

test_expect_success "multiple '--stdin's are rejected" '
	echo example | test_must_fail git hash-object --stdin --stdin
'

test_expect_success "Can't use --stdin and --stdin-paths together" '
	echo example | test_must_fail git hash-object --stdin --stdin-paths &&
	echo example | test_must_fail git hash-object --stdin-paths --stdin
'

test_expect_success "Can't pass filenames as arguments with --stdin-paths" '
	echo example | test_must_fail git hash-object --stdin-paths hello
'

test_expect_success "Can't use --path with --stdin-paths" '
	echo example | test_must_fail git hash-object --stdin-paths --path=foo
'

test_expect_success "Can't use --path with --no-filters" '
	test_must_fail git hash-object --no-filters --path=foo
'

# Behavior

push_repo

test_expect_success 'hash a file' '
	test "$(test_oid hello)" = $(git hash-object hello)
'

test_blob_does_not_exist "$(test_oid hello)"

test_expect_success 'hash from stdin' '
	test "$(test_oid example)" = $(git hash-object --stdin < example)
'

test_blob_does_not_exist "$(test_oid example)"

test_expect_success 'hash a file and write to database' '
	test "$(test_oid hello)" = $(git hash-object -w hello)
'

test_blob_exists "$(test_oid hello)"

test_expect_success 'git hash-object --stdin file1 <file0 first operates on file0, then file1' '
	echo foo > file1 &&
	obname0=$(echo bar | git hash-object --stdin) &&
	obname1=$(git hash-object file1) &&
	obname0new=$(echo bar | git hash-object --stdin file1 | sed -n -e 1p) &&
	obname1new=$(echo bar | git hash-object --stdin file1 | sed -n -e 2p) &&
	test "$obname0" = "$obname0new" &&
	test "$obname1" = "$obname1new"
'

test_expect_success 'set up crlf tests' '
	echo fooQ | tr Q "\\015" >file0 &&
	cp file0 file1 &&
	echo "file0 -crlf" >.gitattributes &&
	echo "file1 crlf" >>.gitattributes &&
	git config core.autocrlf true &&
	file0_sha=$(git hash-object file0) &&
	file1_sha=$(git hash-object file1) &&
	test "$file0_sha" != "$file1_sha"
'

test_expect_success 'check that appropriate filter is invoke when --path is used' '
	path1_sha=$(git hash-object --path=file1 file0) &&
	path0_sha=$(git hash-object --path=file0 file1) &&
	test "$file0_sha" = "$path0_sha" &&
	test "$file1_sha" = "$path1_sha" &&
	path1_sha=$(git hash-object --path=file1 --stdin <file0) &&
	path0_sha=$(git hash-object --path=file0 --stdin <file1) &&
	test "$file0_sha" = "$path0_sha" &&
	test "$file1_sha" = "$path1_sha"
'

test_expect_success 'gitattributes also work in a subdirectory' '
	mkdir subdir &&
	(
		cd subdir &&
		subdir_sha0=$(git hash-object ../file0) &&
		subdir_sha1=$(git hash-object ../file1) &&
		test "$file0_sha" = "$subdir_sha0" &&
		test "$file1_sha" = "$subdir_sha1"
	)
'

test_expect_success '--path works in a subdirectory' '
	(
		cd subdir &&
		path1_sha=$(git hash-object --path=../file1 ../file0) &&
		path0_sha=$(git hash-object --path=../file0 ../file1) &&
		test "$file0_sha" = "$path0_sha" &&
		test "$file1_sha" = "$path1_sha"
	)
'

test_expect_success 'check that --no-filters option works' '
	nofilters_file1=$(git hash-object --no-filters file1) &&
	test "$file0_sha" = "$nofilters_file1" &&
	nofilters_file1=$(git hash-object --stdin <file1) &&
	test "$file0_sha" = "$nofilters_file1"
'

test_expect_success 'check that --no-filters option works with --stdin-paths' '
	nofilters_file1=$(echo "file1" | git hash-object --stdin-paths --no-filters) &&
	test "$file0_sha" = "$nofilters_file1"
'

pop_repo

for args in "-w --stdin" "--stdin -w"; do
	push_repo

	test_expect_success "hash from stdin and write to database ($args)" '
		test "$(test_oid example)" = $(git hash-object $args < example)
	'

	test_blob_exists "$(test_oid example)"

	pop_repo
done

filenames="hello
example"

oids="$(test_oid hello)
$(test_oid example)"

test_expect_success "hash two files with names on stdin" '
	test "$oids" = "$(echo_without_newline "$filenames" | git hash-object --stdin-paths)"
'

for args in "-w --stdin-paths" "--stdin-paths -w"; do
	push_repo

	test_expect_success "hash two files with names on stdin and write to database ($args)" '
		test "$oids" = "$(echo_without_newline "$filenames" | git hash-object $args)"
	'

	test_blob_exists "$(test_oid hello)"
	test_blob_exists "$(test_oid example)"

	pop_repo
done

test_expect_success 'too-short tree' '
	echo abc >malformed-tree &&
	test_must_fail git hash-object -t tree malformed-tree 2>err &&
	test_grep "too-short tree object" err
'

test_expect_success PERL_TEST_HELPERS 'malformed mode in tree' '
	hex_oid=$(echo foo | git hash-object --stdin -w) &&
	bin_oid=$(echo $hex_oid | hex2oct) &&
	printf "9100644 \0$bin_oid" >tree-with-malformed-mode &&
	test_must_fail git hash-object -t tree tree-with-malformed-mode 2>err &&
	test_grep "malformed mode in tree entry" err
'

test_expect_success PERL_TEST_HELPERS 'empty filename in tree' '
	hex_oid=$(echo foo | git hash-object --stdin -w) &&
	bin_oid=$(echo $hex_oid | hex2oct) &&
	printf "100644 \0$bin_oid" >tree-with-empty-filename &&
	test_must_fail git hash-object -t tree tree-with-empty-filename 2>err &&
	test_grep "empty filename in tree entry" err
'

test_expect_success PERL_TEST_HELPERS 'duplicate filename in tree' '
	hex_oid=$(echo foo | git hash-object --stdin -w) &&
	bin_oid=$(echo $hex_oid | hex2oct) &&
	{
		printf "100644 file\0$bin_oid" &&
		printf "100644 file\0$bin_oid"
	} >tree-with-duplicate-filename &&
	test_must_fail git hash-object -t tree tree-with-duplicate-filename 2>err &&
	test_grep "duplicateEntries" err
'

test_expect_success 'corrupt commit' '
	test_must_fail git hash-object -t commit --stdin </dev/null
'

test_expect_success 'corrupt tag' '
	test_must_fail git hash-object -t tag --stdin </dev/null
'

test_expect_success 'hash-object complains about bogus type name' '
	test_must_fail git hash-object -t bogus --stdin </dev/null
'

test_expect_success 'hash-object complains about truncated type name' '
	test_must_fail git hash-object -t bl --stdin </dev/null
'

test_expect_success '--literally complains about non-standard types' '
	test_must_fail git hash-object -t bogus --literally --stdin
'

test_expect_success '--stdin outside of repository (uses default hash)' '
	nongit git hash-object --stdin <hello >actual &&
	echo "$(test_oid --hash=builtin hello)" >expect &&
	test_cmp expect actual
'

test_expect_success 'write chunks using the FastCDC 2020 profile' '
	git init chunked-writer &&
	test_filev2_blob >chunked-writer/blob &&
	writer_oid=$(git -C chunked-writer hash-object blob) &&
	test-tool -C chunked-writer write-chunked-blob "$writer_oid" <chunked-writer/blob &&
	writer_objects=chunked-writer/.git/objects &&
	writer_manifest=$writer_objects/$(test_oid_to_path "$writer_oid") &&

	# Compare the recorded boundaries to the reference FastCDC output.
	test-tool hexdump <"$writer_manifest" >writer-manifest.hex &&
	awk "{ for (i = 17; i <= NF - 32; i += 36) print \$i \$(i+1) \$(i+2) \$(i+3) }" \
		<writer-manifest.hex >actual &&
	for size in 1144213 1634300 1227888 1142474 1245688 819030 1193051 1791442 287675
	do
		printf "%08x\n" "$size" || return 1
	done >expect &&
	test_cmp expect actual &&
	find "$writer_objects/chunks" -type f >writer-chunks &&
	test_line_count = 9 writer-chunks &&
	git -C chunked-writer cat-file blob "$writer_oid" >actual &&
	test_cmp_bin chunked-writer/blob actual
'

test_expect_success 'do not publish a manifest when the expected blob ID differs' '
	git init chunked-mismatch &&
	expected_oid=$(git -C chunked-mismatch hash-object --stdin </dev/null) &&
	test_must_fail test-tool -C chunked-mismatch write-chunked-blob "$expected_oid" \
		<chunked-writer/blob 2>err &&
	test_grep "object data changed while chunking" err &&
	test_path_is_missing "chunked-mismatch/.git/objects/$(test_oid_to_path "$expected_oid")" &&
	test_path_is_missing "chunked-mismatch/.git/objects/$(test_oid_to_path "$writer_oid")"
'

test_expect_success 'filev2 keeps blobs at the chunking cutoff loose' '
	git init --object-storage-format=filev2 filev2 &&
	test-tool genrandom cutoff 4m >cutoff &&
	git hash-object cutoff >expect &&
	git -C filev2 hash-object -w --stdin <cutoff >actual &&
	test_cmp expect actual &&
	git -C filev2 cat-file blob "$(cat expect)" >cutoff.actual &&
	test_cmp_bin cutoff cutoff.actual &&
	find filev2/.git/objects -type f -path "*/chunks/*" >chunks &&
	test_must_be_empty chunks
'

test_expect_success 'filev2 streams large blobs under their canonical object ID' '
	test_filev2_blob >filev2/original &&
	git hash-object filev2/original >expect &&
	cat filev2/original | git -C filev2 hash-object -w --stdin >actual &&
	test_cmp expect actual &&
	original_oid=$(cat expect) &&
	git -C filev2 cat-file blob "$original_oid" >original.actual &&
	test_cmp_bin filev2/original original.actual &&
	printf "blob\n10485761\n" >expect &&
	{
		git -C filev2 cat-file -t "$original_oid" &&
		git -C filev2 cat-file -s "$original_oid"
	} >actual &&
	test_cmp expect actual &&

	manifest=filev2/.git/objects/$(test_oid_to_path "$original_oid") &&
	test-tool hexdump <"$manifest" >manifest.hex &&
	find filev2/.git/objects/chunks -type f >original.chunks &&
	test_line_count = 9 original.chunks
'

test_expect_success 'filev2 reuses chunks across two versions of a blob' '
	{
		cat filev2/original &&
		printf "appended data\n"
	} >filev2/modified &&
	git hash-object filev2/modified >expect &&
	GIT_TEST_FSYNC=true GIT_TRACE2_EVENT="$(pwd)/filev2-trace" \
		git -C filev2 -c core.bigFileThreshold=1m \
		-c core.fsync=none -c core.fsyncMethod=fsync add modified &&
	test_grep ! "\"category\":\"fsync\"" filev2-trace &&
	git -C filev2 rev-parse :modified >actual &&
	test_cmp expect actual &&
	modified_oid=$(cat expect) &&
	test "$original_oid" != "$modified_oid" &&
	git -C filev2 cat-file blob "$modified_oid" >modified.actual &&
	test_cmp_bin filev2/modified modified.actual &&

	manifest=filev2/.git/objects/$(test_oid_to_path "$modified_oid") &&
	manifest_size=$(test_file_size "$manifest") &&
	modified_chunks=$(((manifest_size - 48) / 36)) &&
	find filev2/.git/objects/chunks -type f >combined.chunks &&
	test_line_count -gt 9 combined.chunks &&
	test_line_count -lt "$((9 + modified_chunks))" combined.chunks &&
	git -C filev2 fsck --no-dangling
'

filev2_hardware_flushes () {
	sed -n "/\"category\":\"fsync\",\"name\":\"hardware-flush\"/s/.*\"count\":\([0-9][0-9]*\)}$/\1/p" "$1"
}

test_expect_success 'filev2 syncs reused chunks before publishing a new blob' '
	{
		cat filev2/original &&
		printf "chunk fsync fixture\n"
	} >filev2/with-fsync &&
	GIT_TEST_FSYNC=true GIT_TRACE2_EVENT="$(pwd)/reuse-trace" \
		git -C filev2 -c core.fsync=loose-object -c core.fsyncMethod=fsync \
		hash-object -w with-fsync >fsynced-oid &&
	count=$(filev2_hardware_flushes reuse-trace) &&
	test "$count" -gt 2 &&
	git -C filev2 cat-file blob "$(cat fsynced-oid)" >actual &&
	test_cmp_bin filev2/with-fsync actual
'

test_expect_success 'filev2 batches fsync for repeated and shared chunks' '
	test-tool genzeros $((16 * 1024 * 1024)) >filev2/batch-original &&
	git -C filev2 -c core.fsync=none hash-object -w batch-original &&
	{
		cat filev2/batch-original &&
		printf "batch fsync fixture\n"
	} >filev2/batch-modified &&
	GIT_TEST_FSYNC=true GIT_TRACE2_EVENT="$(pwd)/batch-trace" \
		git -C filev2 -c core.fsync=loose-object -c core.fsyncMethod=batch \
		add batch-modified 2>batch.err &&
	if ! grep "core.fsyncMethod = batch is unsupported" batch.err
	then
		count=$(filev2_hardware_flushes batch-trace) &&
		test "$count" -eq 1
	fi &&
	git -C filev2 cat-file blob :batch-modified >actual &&
	test_cmp_bin filev2/batch-modified actual
'

test_expect_success POSIXPERM,SANITY 'filev2 reuses a blob in a read-only object directory' '
	chmod u-w filev2/.git/objects &&
	test_when_finished "chmod u+w filev2/.git/objects" &&
	git -C filev2 hash-object -w original >actual &&
	echo "$original_oid" >expect &&
	test_cmp expect actual
'

test_expect_success POSIXPERM,SANITY 'filev2 reuses chunks in a read-only shard' '
	prefix=$(awk "{ print \$21 }" <manifest.hex) &&
	shard=filev2/.git/objects/chunks/$prefix &&
	chmod u-w "$shard" &&
	test_when_finished "chmod u+w \"$shard\"" &&
	{
		cat filev2/original &&
		printf "readonly shard change\n"
	} >filev2/readonly-shard &&
	git -C filev2 -c core.bigFileThreshold=1m add readonly-shard &&
	git -C filev2 cat-file blob :readonly-shard >actual &&
	test_cmp_bin filev2/readonly-shard actual
'

test_expect_success 'filev2 repairs a corrupt shared chunk in a direct write' '
	digest=$(awk "{ for (i = 21; i <= 52; i++) printf \"%s\", \$i }" <manifest.hex) &&
	chunk=filev2/.git/objects/chunks/$(test_oid_to_path "$digest") &&
	mv "$chunk" good-chunk &&
	test_when_finished "mv -f good-chunk \"$chunk\"" &&
	printf "corrupt chunk" >"$chunk" &&
	test_must_fail git -C filev2 cat-file blob "$original_oid" >out 2>err &&
	test_grep "corrupt blob chunk" err &&
	{
		cat filev2/original &&
		printf "another version\n"
	} >filev2/another &&
	another_oid=$(git hash-object filev2/another) &&
	cat filev2/another | git -C filev2 hash-object -w --stdin >actual-oid 2>err &&
	test_must_be_empty err &&
	echo "$another_oid" >expect &&
	test_cmp expect actual-oid &&
	git -C filev2 cat-file blob "$another_oid" >another.actual &&
	test_cmp_bin filev2/another another.actual &&
	git -C filev2 fsck --no-dangling
'

test_expect_success 'filev2 fsck checks large valid attributes' '
	git init --object-storage-format=filev2 filev2-attrs &&
	awk "BEGIN { for (i = 0; i < 218454; i++) print \"# filev2 fsck fixture 1\" }" >attrs &&
	attrs_oid=$(git -C filev2-attrs hash-object -w --stdin <attrs) &&

	# The tree is in an earlier loose shard than the blob in both hash formats.
	printf "100644 blob %s\t.gitattributes\n" "$attrs_oid" >attrs-tree &&
	git -C filev2-attrs mktree <attrs-tree &&
	git -C filev2-attrs fsck --no-dangling
'

test_expect_success RUST 'filev2 keeps compatibility hashes after a pack flush' '
	git init --object-format=sha256 --object-storage-format=filev2 filev2-compat &&
	git init --object-format=sha1 filev2-sha1 &&
	git -C filev2-compat config extensions.compatObjectFormat sha1 &&
	echo small >filev2-compat/small &&
	test-tool genrandom medium-a 2m >filev2-compat/a-medium &&
	test-tool genrandom medium-b 2m >filev2-compat/b-medium &&
	cp filev2/original filev2-compat/z-large &&
	git -C filev2-compat add small &&

	# Flush a conventional pack before publishing the chunked blob.
	git -C filev2-compat -c core.bigFileThreshold=1m -c pack.packSizeLimit=1m \
		add a-medium b-medium z-large &&
	for path in small z-large
	do
		git -C filev2-sha1 hash-object "../filev2-compat/$path" >expect &&
		git -C filev2-compat rev-parse --output-object-format=sha1 ":$path" >actual &&
		test_cmp expect actual || return 1
	done
'

test_expect_success RUST,POSIXPERM,SANITY 'filev2 waits for compatibility mappings before publishing' '
	git init --object-format=sha256 --object-storage-format=filev2 filev2-map &&
	git -C filev2-map config extensions.compatObjectFormat sha1 &&
	echo small >filev2-map/small &&
	git -C filev2-map add small &&
	cp filev2/original filev2-map/large &&
	oid=$(git -C filev2-map hash-object large) &&
	map=filev2-map/.git/objects/loose-object-idx &&
	manifest=filev2-map/.git/objects/$(test_oid_to_path "$oid") &&
	chmod a-w "$map" &&
	test_when_finished "chmod u+w \"$map\"" &&
	test_must_fail git -C filev2-map -c core.bigFileThreshold=1m add large 2>err &&
	test_grep "failed to write loose object index" err &&
	test_path_is_missing "$manifest" &&
	chmod u+w "$map" &&
	git -C filev2-map -c core.bigFileThreshold=1m add large &&
	git -C filev2-sha1 hash-object ../filev2-map/large >expect &&
	git -C filev2-map rev-parse --output-object-format=sha1 :large >actual &&
	test_cmp expect actual &&
	chmod a-w "$map" &&
	git -C filev2-map -c core.bigFileThreshold=1m -c core.fsync=loose-object \
		hash-object -w large >actual &&
	echo "$oid" >expect &&
	test_cmp expect actual
'

test_expect_success 'filev2 preserves the ref update restriction in quarantine' '
	git init --object-storage-format=filev2 filev2-quarantine &&
	test_commit -C filev2-quarantine initial tracked small &&
	git -C filev2-quarantine rev-parse HEAD >expect &&
	mkdir quarantine-odb &&
	test-tool genrandom filev2-quarantine 5m >filev2-quarantine/tracked &&
	test_must_fail env GIT_OBJECT_DIRECTORY="$(pwd)/quarantine-odb" \
		GIT_QUARANTINE_PATH="$(pwd)/quarantine-odb" \
		GIT_ALTERNATE_OBJECT_DIRECTORIES="$(pwd)/filev2-quarantine/.git/objects" \
		git -C filev2-quarantine -c core.bigFileThreshold=1m -c core.fsync=none \
		commit -am quarantined 2>err &&
	test_grep "ref updates forbidden inside quarantine" err &&
	git -C filev2-quarantine rev-parse HEAD >actual &&
	test_cmp expect actual &&
	git -C filev2-quarantine -c core.bigFileThreshold=1m commit -am ordinary &&
	git -C filev2-quarantine cat-file blob HEAD:tracked >actual &&
	test_cmp_bin filev2-quarantine/tracked actual
'

test_expect_success EXPENSIVE,SIZE_T_IS_64BIT \
		'files over 4GB hash literally' '
	test-tool genzeros $((5*1024*1024*1024)) >big &&
	test_oid large5GB >expect &&
	git hash-object --stdin --literally <big >actual &&
	test_cmp expect actual
'

test_expect_success EXPENSIVE,SIZE_T_IS_64BIT \
		'files over 4GB hash correctly via --stdin' '
	{ test -f big || test-tool genzeros $((5*1024*1024*1024)) >big; } &&
	test_oid large5GB >expect &&
	git hash-object --stdin <big >actual &&
	test_cmp expect actual
'

test_expect_success EXPENSIVE,SIZE_T_IS_64BIT \
		'files over 4GB hash correctly' '
	{ test -f big || test-tool genzeros $((5*1024*1024*1024)) >big; } &&
	test_oid large5GB >expect &&
	git hash-object -- big >actual &&
	test_cmp expect actual
'

# This clean filter does nothing, other than excercising the interface.
# We ensure that cleaning doesn't mangle large files on 64-bit Windows.
test_expect_success EXPENSIVE,SIZE_T_IS_64BIT \
		'hash filtered files over 4GB correctly' '
	{ test -f big || test-tool genzeros $((5*1024*1024*1024)) >big; } &&
	test_oid large5GB >expect &&
	test_config filter.null-filter.clean "cat" &&
	echo "big filter=null-filter" >.gitattributes &&
	git hash-object -- big >actual &&
	test_cmp expect actual
'

test_done
