#ifndef AASSET_PATCH_H
#define AASSET_PATCH_H

/* Minimal NDK AAssetManager backed by the local filesystem (g_data_path).
 * Geometry Wars 3 reads its assets through AAssetManager_open/read/... rather
 * than via JNI path strings, so we must provide a working implementation.
 *
 * This is a SKELETON: it serves files relative to the data path. If the engine
 * expects assets inside the packed OBB container, you will need to either unpack
 * the OBB into the data dir or make AAssetManager_open parse the container. */

void aasset_patch_init(void);

#endif /* AASSET_PATCH_H */
