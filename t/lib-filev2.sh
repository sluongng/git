# Deterministic input whose chunk boundaries are checked against FastCDC.
test_filev2_blob () {
	test-tool genrandom filev2 10485761
}

test_filev2_server () {
	git init --object-storage-format=filev2 "$1" &&
	git -C "$1" config uploadpack.allowFilter true &&
	git -C "$1" config uploadpack.allowBlobChunks true &&
	test_filev2_blob >"$1/large" &&
	git -C "$1" add large &&
	test_commit -C "$1" initial
}

test_filev2_clone () {
	git -c protocol.version=2 clone --no-checkout --object-storage-format=filev2 \
		--filter=blob:limit=4194305 -c remote.origin.blobChunks=true "$@"
}
