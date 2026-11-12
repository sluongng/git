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
