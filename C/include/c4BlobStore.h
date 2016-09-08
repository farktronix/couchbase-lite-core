//
//  c4BlobStore.h
//  LiteCore
//
//  Created by Jens Alfke on 9/1/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#pragma once
#include "c4Database.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

    // BLOB KEYS:

    /** A raw SHA-1 digest used as the unique identifier of a blob. */
    typedef struct C4BlobKey {
        uint8_t bytes[20];
    } C4BlobKey;

    
    /** Decodes a string of the form "sha1-"+base64 into a raw key. */
    bool c4blob_keyFromString(C4Slice str, C4BlobKey*);

    /** Encodes a blob key to a string of the form "sha1-"+base64. */
    C4SliceResult c4blob_keyToString(C4BlobKey);


    /** Opaque handle for an object that manages storage of blobs. */
    typedef struct c4BlobStore C4BlobStore;


    // BLOB STORE API:

    /** Opens a BlobStore in a directory. If the flags allow creating, the directory will be
        created if necessary. */
    C4BlobStore* c4blob_openStore(C4Slice dirPath,
                                  C4DatabaseFlags,
                                  const C4EncryptionKey*,
                                  C4Error*);

    /** Closes/frees a BlobStore. (A NULL parameter is allowed.) */
    void c4blob_freeStore(C4BlobStore*);

    /** Deletes the BlobStore's blobs and directory, and (if successful) frees the object. */
    bool c4blob_deleteStore(C4BlobStore*, C4Error*);


    /** Gets the content size of a blob given its key. Returns -1 if it doesn't exist.
        WARNING: If blob is encrypted, the size is approximate and may be off by +/- 16 bytes. */
    int64_t c4blob_getSize(C4BlobStore*, C4BlobKey);

    /** Reads the entire contents of a blob into memory. Caller is responsible for freeing it. */
    C4SliceResult c4blob_getContents(C4BlobStore*, C4BlobKey, C4Error*);


    /** Stores a blob. The associated key will be written to `outKey`. */
    bool c4blob_create(C4BlobStore*, C4Slice contents, C4BlobKey *outKey, C4Error*);


    /** Deletes a blob from the store given its key. */
    bool c4blob_delete(C4BlobStore*, C4BlobKey, C4Error*);


    // STREAMING API:

    /** An open stream for reading data from a blob. */
    typedef struct c4ReadStream C4ReadStream;

    /** Opens a blob for reading, as a random-access byte stream. */
    C4ReadStream* c4blob_openReadStream(C4BlobStore*, C4BlobKey, C4Error*);

    /** Reads from an open stream, returning the actual number of bytes read, or zero on error. */
    size_t c4stream_read(C4ReadStream*, void *buffer, size_t maxBytesToRead, C4Error*);

    /** Returns the exact length in bytes of the stream. */
    int64_t c4stream_getLength(C4ReadStream*, C4Error*);

    /** Moves to a random location in the stream; the next c4stream_read call will read from that
        location. */
    bool c4stream_seek(C4ReadStream*, uint64_t position, C4Error*);

    /** Closes a read-stream. (A NULL parameter is allowed.) */
    void c4stream_close(C4ReadStream*);


    /** An open stream for writing data to a blob. */
    typedef struct c4WriteStream C4WriteStream;

    /** Opens a write stream for creating a new blob. You should then call c4stream_write to
        write the data, ending with c4stream_install to compute the blob's key and add it to
        the store, and then c4stream_closeWriter. */
    C4WriteStream* c4blob_createWithStream(C4BlobStore*, C4Error*);

    /** Writes data to a stream. */
    bool c4stream_write(C4WriteStream*, const void *bytes, size_t length, C4Error*);

    /** Computes the blob-key (digest) of the data written to the stream. This should only be
        called after writing the entire data. No more data can be written after this call. */
    C4BlobKey c4stream_computeBlobKey(C4WriteStream*);

    /** Adds the data written to the stream as a finished blob to the store, and returns its key.
        If you skip this call, the blob will not be added to the store. (You might do this if you
        were unable to receive all of the data from the network, or if you've called
        c4stream_computeBlobKey and found that the data does not match the expected digest/key.) */
    bool c4stream_install(C4WriteStream*, C4Error*);

    /** Closes a blob write-stream. If c4stream_install was not already called, the temporary file
        will be deleted without adding the blob to the store. (A NULL parameter is allowed.) */
    void c4stream_closeWriter(C4WriteStream*);


#ifdef __cplusplus
}
#endif