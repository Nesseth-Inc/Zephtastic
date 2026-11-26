# Vaporwave Rev2 source

Palette is copied byte for byte; procedural artwork only changes function
linkage from static C helpers to C++ members. Both come from the frozen
ZephCore Portable Vaporwave archive, SHA256
`0e894779719848911356d3784d2635f4ff82d4061c0470beafa8af6098c92edf`.
The 6x8 MIT font retains its original table; the Zephyr CFB registration was
removed for a raw RGB565 renderer. Base commit
`3c7a7c830ac4cdbd0df052f06995d3c7632b9b44`, with the tested Rev2 patch.

ZephTastic adapts the compact 128x64 virtual geometry to the 160x80 T096,
retains high-byte-first RGB565 writes, and uses the T096 ST7735S board
initialization. It does not import MeshCore services, actions or the T114
ST7789 overlay. Physical T096 colors remain unverified.

UI1 ports the five-file display donor from commit
`88d7339e72bcfe70cd1021012761986aee175369` (M3-based display branch) onto
canonical M4. Only display/model/tests were imported; M4 protocol, queue, radio,
identity and configuration behavior stay authoritative. The shared renderer
adds host-verified 128x64 MONO01 and 240x135 RGB565/strip layouts; only the T096
160x80 target is enabled by the canonical build script. See UI-M4U1.md.
