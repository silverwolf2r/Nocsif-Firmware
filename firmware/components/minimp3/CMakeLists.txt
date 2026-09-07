# NocSif — minimp3, a public-domain (CC0) single-header MP3 decoder, as a header-only ESP-IDF component.
#
# Vendored UNMODIFIED from https://github.com/lieff/minimp3 (minimp3.h, master, fetched 2026-09-07;
# the CC0 dedication is in the header itself). Used by the Phase B audio engine (audio.c) for the Carts
# player: audio.c is the ONE translation unit that defines MINIMP3_IMPLEMENTATION (with MINIMP3_ONLY_MP3
# + MINIMP3_NO_SIMD — the Xtensa has neither SSE nor NEON). Decoder state (mp3dec_t, ~6.7 KB) and the
# input/PCM buffers live in PSRAM; mp3dec_decode_frame keeps ~19 KB of scratch on the CALLER's stack, so
# the audio worker's (PSRAM) stack is sized for it. No SRCS — header only.
idf_component_register(INCLUDE_DIRS ".")
