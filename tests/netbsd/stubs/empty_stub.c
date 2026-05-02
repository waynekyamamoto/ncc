/*
 * Empty translation unit. Used by ncc-elf-wrapper.sh in place of NEON
 * source files we can't compile. All NEON exports are concentrated in
 * neon_stub.c (substituted for chacha_neon.c) so compiling other neon
 * files as empty doesn't cause link-time symbol issues.
 */
