//
// ReplicatedRev.hh
//
// Copyright © 2018 Couchbase. All rights reserved.
//

#pragma once
#include "RefCounted.hh"
#include "fleece/slice.hh"
#include "c4Base.h"
#include "c4Document.h"
#include "c4Replicator.h"

namespace litecore { namespace repl {

    enum class Dir {
        kPulling = 0,
        kPushing
    };


    /** Metadata of a document revision. Abstract superclass of RevToSend, RevToInsert (see
        ReplicatorTypes.hh). Used to track revisions during the replication flow, and to notify
        the delegate at the end. */
    class ReplicatedRev : public fleece::RefCounted {
    public:
        using slice = fleece::slice;
        using alloc_slice = fleece::alloc_slice;

        // Note: The following fields must be compatible with the public C4DocumentEnded struct:
        const alloc_slice   docID;
        const alloc_slice   revID;
        C4RevisionFlags     flags {0};
        C4SequenceNumber    sequence;
        C4Error             error {};
        bool                errorIsTransient {false};

        const C4DocumentEnded* asDocumentEnded() const  {
            static_assert(offsetof(ReplicatedRev, errorIsTransient) - offsetof(ReplicatedRev, docID) ==
                          offsetof(C4DocumentEnded, errorIsTransient) - offsetof(C4DocumentEnded, docID),
                          "ReplicatedRev doesn't match C4DocumentEnded");
            return (const C4DocumentEnded*)&docID;
        }

        bool                isWarning {false};

        virtual Dir dir() const =0;
        bool deleted() const                            {return (flags & kRevDeleted) != 0;}

        // Internal use only:
        virtual void trim() =0;

    protected:
        template <class SLICE1, class SLICE2>
        ReplicatedRev(SLICE1 docID_, SLICE2 revID_, C4SequenceNumber sequence_ =0)
        :docID(docID_), revID(revID_), sequence(sequence_)
        { }

        ~ReplicatedRev() =default;
    };

} }
