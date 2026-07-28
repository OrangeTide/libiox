# module.mk -- top level
#
# src/ is the library alone. tests/ holds the test suite and the fuzz
# harness, so that a project vendoring libiox can compile src/*.c without
# sweeping up a main() it does not want.

SUBDIRS = src tests
