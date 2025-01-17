//
// Puller.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//  https://github.com/couchbase/couchbase-lite-core/wiki/Replication-Protocol

#include "Puller.hh"
#include "RevFinder.hh"
#include "Inserter.hh"
#include "IncomingRev.hh"
#include "ReplicatorTuning.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "BLIP.hh"
#include "Instrumentation.hh"
#include <algorithm>

using namespace std;
using namespace fleece;

namespace litecore { namespace repl {

    Puller::Puller(Replicator *replicator)
    :Worker(replicator, "Pull")
    ,_inserter(new Inserter(replicator))
    ,_revFinder(new RevFinder(replicator))
    ,_returningRevs(this, &Puller::_revsFinished)
#if __APPLE__
    ,_revMailbox(nullptr, "Puller revisions")
#endif
    {
        registerHandler("changes",          &Puller::handleChanges);
        registerHandler("proposeChanges",   &Puller::handleChanges);
        registerHandler("rev",              &Puller::handleRev);
        registerHandler("norev",            &Puller::handleNoRev);
        _spareIncomingRevs.reserve(tuning::kMaxActiveIncomingRevs);
        _skipDeleted = _options.skipDeleted();
        if (nonPassive() && _options.noIncomingConflicts())
            warn("noIncomingConflicts mode is not compatible with active pull replications!");
    }


    // Starting an active pull.
    void Puller::_start(alloc_slice sinceSequence) {
        _lastSequence = sinceSequence;
        _missingSequences.clear(sinceSequence);
        logInfo("Starting pull from remote seq %.*s", SPLAT(_lastSequence));

        Signpost::begin(Signpost::blipSent);
        MessageBuilder msg("subChanges"_sl);
        if (_lastSequence)
            msg["since"_sl] = _lastSequence;
        if (_options.pull == kC4Continuous)
            msg["continuous"_sl] = "true"_sl;
        msg["batch"_sl] = tuning::kChangesBatchSize;

        if (_skipDeleted)
            msg["activeOnly"_sl] = "true"_sl;

        auto channels = _options.channels();
        if (channels) {
            stringstream value;
            unsigned n = 0;
            for (Array::iterator i(channels); i; ++i) {
                slice name = i.value().asString();
                if (name) {
                    if (n++)
                         value << ",";
                    value << name.asString();
                }
            }
            msg["filter"_sl] = "sync_gateway/bychannel"_sl;
            msg["channels"_sl] = value.str();
        } else {
            slice filter = _options.filter();
            if (filter) {
                msg["filter"_sl] = filter;
                for (Dict::iterator i(_options.filterParams()); i; ++i)
                    msg[i.keyString()] = i.value().asString();
            }
        }

        auto docIDs = _options.docIDs();
        if (docIDs) {
            auto &enc = msg.jsonBody();
            enc.beginDict();
            enc.writeKey("docIDs"_sl);
            enc.writeValue(docIDs);
            enc.endDict();
        }
        
        sendRequest(msg, [=](blip::MessageProgress progress) {
            //... After request is sent:
            if (progress.reply && progress.reply->isError()) {
                gotError(progress.reply);
                _fatalError = true;
            }
            if (progress.state == MessageProgress::kComplete)
                Signpost::end(Signpost::blipSent);
        });
    }


#pragma mark - INCOMING CHANGE LISTS:


    // Receiving an incoming "changes" (or "proposeChanges") message
    void Puller::handleChanges(Retained<MessageIn> req) {
        logVerbose("Received '%.*s' REQ#%" PRIu64 " (%zu queued; %u revs pending, %u active, %u unfinished)",
                   SPLAT(req->property("Profile"_sl)), req->number(),
                   _waitingChangesMessages.size(), _pendingRevMessages,
                   _activeIncomingRevs, _unfinishedIncomingRevs);
        Signpost::begin(Signpost::handlingChanges, (uintptr_t)req->number());
        _waitingChangesMessages.push_back(move(req));
        handleMoreChanges();
    }


    // Process waiting "changes" messages if not throttled:
    void Puller::handleMoreChanges() {
        while (!_waitingChangesMessages.empty()
               && _pendingRevMessages < tuning::kMaxPendingRevs) {
            auto req = _waitingChangesMessages.front();
            _waitingChangesMessages.pop_front();
            handleChangesNow(req);
        }

#ifdef LITECORE_SIGNPOSTS
        bool backPressure = !_waitingRevMessages.empty();
        if (_changesBackPressure != backPressure) {
            _changesBackPressure = backPressure;
            if (backPressure)
                Signpost::begin(Signpost::changesBackPressure);
            else
                Signpost::end(Signpost::changesBackPressure);
        }
#endif
    }


    // Actually handle a "changes" message:
    void Puller::handleChangesNow(Retained<MessageIn> req) {
        slice reqType = req->property("Profile"_sl);
        bool proposed = (reqType == "proposeChanges"_sl);
        logVerbose("Handling '%.*s' REQ#%" PRIu64, SPLAT(reqType), req->number());

        auto changes = req->JSONBody().asArray();
        if (!changes && req->body() != "null"_sl) {
            warn("Invalid body of 'changes' message");
            req->respondWithError({"BLIP"_sl, 400, "Invalid JSON body"_sl});
        } else if (changes.empty()) {
            // Empty array indicates we've caught up.
            logInfo("Caught up with remote changes");
            _caughtUp = true;
            _skipDeleted = false;
            req->respond();
        } else if (req->noReply()) {
            warn("Got pointless noreply 'changes' message");
        } else if (_options.noIncomingConflicts() && !proposed) {
            // In conflict-free mode the protocol requires the pusher send "proposeChanges" instead
            req->respondWithError({"BLIP"_sl, 409});
        } else {
            // Pass the buck to the RevFinder so it can find the missing revs & request them...
            increment(_pendingRevFinderCalls);
            _revFinder->findOrRequestRevs(req, &_incomingDocIDs,
                                          asynchronize([=](vector<bool> which) {
                // ... after the RevFinder returns:
                decrement(_pendingRevFinderCalls);
                for (size_t i = 0; i < which.size(); ++i) {
                    bool requesting = (which[i]);
                    if (nonPassive()) {
                        // Add sequence to _missingSequences:
                        auto change = changes[(unsigned)i].asArray();
                        alloc_slice sequence(change[0].toJSON());
                        uint64_t bodySize = requesting ? max(change[4].asUnsigned(), (uint64_t)1) : 0;
                        if (sequence)
                            _missingSequences.add(sequence, bodySize);
                        else
                            warn("Empty/invalid sequence in 'changes' message");
                        addProgress({0, bodySize});
                        if (!requesting)
                            completedSequence(sequence); // Not requesting, just update checkpoint
                    }
                    if (requesting) {
                        increment(_pendingRevMessages);
                        // now awaiting a handleRev call...
                    }
                }
                if (nonPassive()) {
                    logVerbose("Now waiting for %u 'rev' messages; %zu known sequences pending",
                               _pendingRevMessages, _missingSequences.size());
                }
                Signpost::end(Signpost::handlingChanges, (uintptr_t)req->number());
            }));
            return;
        }

        Signpost::end(Signpost::handlingChanges, (uintptr_t)req->number());
    }


#pragma mark - INCOMING REVS:


    // Received an incoming "rev" message, which contains a revision body to insert
    void Puller::handleRev(Retained<MessageIn> msg) {
        if (_activeIncomingRevs < tuning::kMaxActiveIncomingRevs
                && _unfinishedIncomingRevs < tuning::kMaxUnfinishedIncomingRevs) {
            startIncomingRev(msg);
        } else {
            logDebug("Delaying handling 'rev' message for '%.*s' [%zu waiting]",
                     SPLAT(msg->property("id"_sl)), _waitingRevMessages.size()+1);
            if (_waitingRevMessages.empty())
                Signpost::begin(Signpost::revsBackPressure);
            _waitingRevMessages.push_back(move(msg));
        }
    }


    void Puller::handleNoRev(Retained<MessageIn> msg) {
        _incomingDocIDs.remove(alloc_slice(msg->property("id"_sl)));
        decrement(_pendingRevMessages);
        slice sequence(msg->property("sequence"_sl));
        if (sequence)
            completedSequence(alloc_slice(sequence));
        handleMoreChanges();
        if (!msg->noReply()) {
            MessageBuilder response(msg);
            msg->respond(response);
        }
    }


    // Actually process an incoming "rev" now:
    void Puller::startIncomingRev(MessageIn *msg) {
        decrement(_pendingRevMessages);
        increment(_activeIncomingRevs);
        increment(_unfinishedIncomingRevs);
        Retained<IncomingRev> inc;
        if (_spareIncomingRevs.empty()) {
            inc = new IncomingRev(this);
        } else {
            inc = _spareIncomingRevs.back();
            _spareIncomingRevs.pop_back();
        }
        inc->handleRev(msg);  // ... will call _revWasHandled when it's finished
        handleMoreChanges();
    }


    // Callback from an IncomingRev when it's been written to the db but before the commit
    void Puller::_revWasProvisionallyHandled() {
        decrement(_activeIncomingRevs);
        if (_activeIncomingRevs < tuning::kMaxActiveIncomingRevs
                    && _unfinishedIncomingRevs < tuning::kMaxUnfinishedIncomingRevs
                    && !_waitingRevMessages.empty()) {
            auto msg = _waitingRevMessages.front();
            _waitingRevMessages.pop_front();
            if (_waitingRevMessages.empty())
                Signpost::end(Signpost::revsBackPressure);
            startIncomingRev(msg);
            handleMoreChanges();
        }
    }

    // Callback from an IncomingRev when it's finished (either added to db, or failed)
    void Puller::revWasHandled(IncomingRev *inc) {
        _incomingDocIDs.remove(inc->rev()->docID);       // this is thread-safe
        _returningRevs.push(inc);
    }

    void Puller::_revsFinished(int gen) {
        auto revs = _returningRevs.pop(gen);
        for (IncomingRev *inc : *revs) {
            if (!inc->wasProvisionallyInserted())
                _revWasProvisionallyHandled();
            auto rev = inc->rev();
            if (nonPassive())
                completedSequence(inc->remoteSequence(), rev->errorIsTransient, false);
            finishedDocument(rev);
        }
        decrement(_unfinishedIncomingRevs, (unsigned)revs->size());

        if (nonPassive())
            updateLastSequence();

        ssize_t capacity = tuning::kMaxActiveIncomingRevs - _spareIncomingRevs.size();
        if (capacity > 0)
            _spareIncomingRevs.insert(_spareIncomingRevs.end(),
                                      revs->begin(),
                                      revs->begin() + min(size_t(capacity), revs->size()));
    }


    // Records that a sequence has been successfully pulled.
    void Puller::completedSequence(alloc_slice sequence, bool withTransientError, bool shouldUpdateLastSequence) {
        uint64_t bodySize;
        if (withTransientError) {
            // If there's a transient error, don't mark this sequence as completed,
            // but add the body size to the completed so the progress will reach 1.0
            bodySize = _missingSequences.bodySizeOfSequence(sequence);
        } else {
            bool wasEarliest;
            _missingSequences.remove(sequence, wasEarliest, bodySize);
            if (wasEarliest && shouldUpdateLastSequence)
                updateLastSequence();
        }
        addProgress({bodySize, 0});
    }


    void Puller::updateLastSequence() {
        auto since = _missingSequences.since();
        if (since != _lastSequence) {
            _lastSequence = since;
            logVerbose("Checkpoint now at %.*s", SPLAT(_lastSequence));
            if (replicator())
                replicator()->updatePullCheckpoint(_lastSequence);
        }
    }


    void Puller::insertRevision(RevToInsert *rev) {
        _inserter->insertRevision(rev);
    }


#pragma mark - STATUS / PROGRESS:


    void Puller::_childChangedStatus(Worker *task, Status status) {
        // Combine the IncomingRev's progress into mine:
        addProgress(status.progressDelta);
    }

    
    Worker::ActivityLevel Puller::computeActivityLevel() const {
        ActivityLevel level;
        if (_fatalError || !connection()) {
            level = kC4Stopped;
        } else if (Worker::computeActivityLevel() == kC4Busy
                || (!_caughtUp && nonPassive())
                || _pendingRevMessages > 0
                || _unfinishedIncomingRevs > 0
                || _pendingRevFinderCalls > 0) {
            level = kC4Busy;
        } else if (_options.pull == kC4Continuous || isOpenServer()) {
            _spareIncomingRevs.clear();
            level = kC4Idle;
        } else {
            level = kC4Stopped;
        }
        if (SyncBusyLog.effectiveLevel() <= LogLevel::Info) {
            logInfo("activityLevel=%-s: pendingResponseCount=%d, _caughtUp=%d, _pendingRevMessages=%u, _activeIncomingRevs=%u",
                kC4ReplicatorActivityLevelNames[level],
                pendingResponseCount(), _caughtUp,
                _pendingRevMessages, _activeIncomingRevs);
        }
        return level;
    }


} }
