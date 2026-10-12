# Embedded image model fixtures

These original quad models exercise the shared Portable model loader.
`embedded-data.gltf` contains a pink PNG data URI and base64 geometry.
`embedded-buffer.glb` contains a green PNG in a bufferView of its BIN chunk.
Both use an unlit, double-sided material so framebuffer checks can distinguish
a decoded image from an untextured white model without lighting dependencies.
