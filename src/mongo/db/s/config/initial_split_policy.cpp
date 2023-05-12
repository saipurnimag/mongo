/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/s/config/initial_split_policy.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/curop.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/process_interface/shardsvr_process_interface.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/s/balancer/balancer_policy.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/unordered_map.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

using ChunkDistributionMap = stdx::unordered_map<ShardId, size_t>;
using ZoneShardMap = StringMap<std::vector<ShardId>>;
using boost::intrusive_ptr;

std::vector<ShardId> getAllShardIdsShuffled(OperationContext* opCtx) {
    auto shardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
    std::default_random_engine rng{};
    std::shuffle(shardIds.begin(), shardIds.end(), rng);
    return shardIds;
}

/**
 * Creates a chunk based on the given arguments, appends it to 'chunks' and increments the given
 * chunk version
 */
void appendChunk(const SplitPolicyParams& params,
                 const BSONObj& min,
                 const BSONObj& max,
                 ChunkVersion* version,
                 const ShardId& shardId,
                 std::vector<ChunkType>* chunks) {
    chunks->emplace_back(params.collectionUUID, ChunkRange(min, max), *version, shardId);
    auto& chunk = chunks->back();
    chunk.setOnCurrentShardSince(version->getTimestamp());
    chunk.setHistory({ChunkHistory(*chunk.getOnCurrentShardSince(), shardId)});
    version->incMinor();
}

/**
 * Return the shard with least amount of chunks while respecting the zone settings.
 */
ShardId selectBestShard(const ChunkDistributionMap& chunkMap,
                        const ZoneInfo& zoneInfo,
                        const ZoneShardMap& zoneToShards,
                        const ChunkRange& chunkRange) {
    auto zone = zoneInfo.getZoneForChunk(chunkRange);
    auto iter = zoneToShards.find(zone);

    uassert(4952605,
            str::stream() << "no shards found for zone: " << zone
                          << ", while creating initial chunks for new resharded collection",
            iter != zoneToShards.end());
    const auto& shards = iter->second;

    uassert(4952607,
            str::stream() << "no shards found for zone: " << zone
                          << ", while creating initial chunks for new resharded collection",
            !shards.empty());

    auto bestShardIter = chunkMap.end();

    for (const auto& shard : shards) {
        auto candidateIter = chunkMap.find(shard);
        if (bestShardIter == chunkMap.end() || candidateIter->second < bestShardIter->second) {
            bestShardIter = candidateIter;
        }
    }

    invariant(bestShardIter != chunkMap.end());
    return bestShardIter->first;
}

/*
 * Returns a map mapping each tag name to a vector of shard ids with that tag name
 */
StringMap<std::vector<ShardId>> buildTagsToShardIdsMap(OperationContext* opCtx,
                                                       const std::vector<TagsType>& tags) {
    StringMap<std::vector<ShardId>> tagToShardIds;
    if (tags.empty()) {
        return tagToShardIds;
    }

    // Get all docs in config.shards through a query instead of going through the shard registry
    // because we need the zones as well
    const auto configServer = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    const auto shardDocs = uassertStatusOK(
        configServer->exhaustiveFindOnConfig(opCtx,
                                             ReadPreferenceSetting(ReadPreference::Nearest),
                                             repl::ReadConcernLevel::kMajorityReadConcern,
                                             NamespaceString::kConfigsvrShardsNamespace,
                                             BSONObj(),
                                             BSONObj(),
                                             boost::none));
    uassert(50986, str::stream() << "Could not find any shard documents", !shardDocs.docs.empty());

    for (const auto& tag : tags) {
        tagToShardIds[tag.getTag()] = {};
    }

    for (const auto& shardDoc : shardDocs.docs) {
        auto parsedShard = uassertStatusOK(ShardType::fromBSON(shardDoc));
        for (const auto& tag : parsedShard.getTags()) {
            tagToShardIds[tag].push_back(parsedShard.getName());
        }
    }

    return tagToShardIds;
}

/**
 * Returns a set of split points to ensure that chunk boundaries will align with the zone
 * ranges.
 */
BSONObjSet extractSplitPointsFromZones(const ShardKeyPattern& shardKey,
                                       const boost::optional<std::vector<TagsType>>& zones) {
    auto splitPoints = SimpleBSONObjComparator::kInstance.makeBSONObjSet();

    if (!zones) {
        return splitPoints;
    }

    for (const auto& zone : *zones) {
        splitPoints.insert(zone.getMinKey());
        splitPoints.insert(zone.getMaxKey());
    }

    const auto keyPattern = shardKey.getKeyPattern();
    splitPoints.erase(keyPattern.globalMin());
    splitPoints.erase(keyPattern.globalMax());

    return splitPoints;
}

/*
 * Returns a map mapping shard id to a set of zone tags.
 */
stdx::unordered_map<ShardId, stdx::unordered_set<std::string>> buildShardIdToTagsMap(
    OperationContext* opCtx, const std::vector<ShardKeyRange>& shards) {
    stdx::unordered_map<ShardId, stdx::unordered_set<std::string>> shardIdToTags;
    if (shards.empty()) {
        return shardIdToTags;
    }

    // Get all docs in config.shards through a query instead of going through the shard registry
    // because we need the zones as well
    const auto configServer = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    const auto shardDocs = uassertStatusOK(
        configServer->exhaustiveFindOnConfig(opCtx,
                                             ReadPreferenceSetting(ReadPreference::Nearest),
                                             repl::ReadConcernLevel::kMajorityReadConcern,
                                             NamespaceString::kConfigsvrShardsNamespace,
                                             BSONObj(),
                                             BSONObj(),
                                             boost::none));
    uassert(
        7661502, str::stream() << "Could not find any shard documents", !shardDocs.docs.empty());

    for (const auto& shard : shards) {
        shardIdToTags[shard.getShard()] = {};
    }

    for (const auto& shardDoc : shardDocs.docs) {
        auto parsedShard = uassertStatusOK(ShardType::fromBSON(shardDoc));
        for (const auto& tag : parsedShard.getTags()) {
            shardIdToTags[ShardId(parsedShard.getName())].insert(tag);
        }
    }

    return shardIdToTags;
}
}  // namespace

std::vector<BSONObj> InitialSplitPolicy::calculateHashedSplitPoints(
    const ShardKeyPattern& shardKeyPattern, BSONObj prefix, int numInitialChunks) {
    invariant(shardKeyPattern.isHashedPattern());
    invariant(numInitialChunks > 0);

    std::vector<BSONObj> splitPoints;
    if (numInitialChunks == 1) {
        return splitPoints;
    }

    // Hashes are signed, 64-bit integers. So we divide the range (-MIN long, +MAX long) into
    // intervals of size (2^64/numInitialChunks) and create split points at the boundaries.
    //
    // The logic below ensures that initial chunks are all symmetric around 0.
    const long long intervalSize = (std::numeric_limits<long long>::max() / numInitialChunks) * 2;
    long long current = 0;

    const auto proposedKey(shardKeyPattern.getKeyPattern().toBSON());

    auto buildSplitPoint = [&](long long value) {
        // Forward the iterator until hashed field is reached.
        auto shardKeyPatternItr = BSONObjIterator(shardKeyPattern.getKeyPattern().toBSON());
        while (shardKeyPattern.getHashedField().fieldNameStringData() !=
               (*shardKeyPatternItr++).fieldNameStringData()) {
        }

        // Append the prefix fields to the new splitpoint, if any such fields exist.
        BSONObjBuilder bob(prefix);

        // Append the value of the hashed field for the current splitpoint.
        bob.append(shardKeyPattern.getHashedField().fieldNameStringData(), value);

        // Set all subsequent shard key fields to MinKey.
        while (shardKeyPatternItr.more()) {
            bob.appendMinKey((*shardKeyPatternItr++).fieldNameStringData());
        }
        return bob.obj();
    };

    if (numInitialChunks % 2 == 0) {
        splitPoints.push_back(buildSplitPoint(current));
        current += intervalSize;
    } else {
        current += intervalSize / 2;
    }

    for (int i = 0; i < (numInitialChunks - 1) / 2; i++) {
        splitPoints.push_back(buildSplitPoint(current));
        splitPoints.push_back(buildSplitPoint(-current));
        current += intervalSize;
    }

    sort(splitPoints.begin(), splitPoints.end(), SimpleBSONObjComparator::kInstance.makeLessThan());
    return splitPoints;
}

InitialSplitPolicy::ShardCollectionConfig InitialSplitPolicy::generateShardCollectionInitialChunks(
    const SplitPolicyParams& params,
    const ShardKeyPattern& shardKeyPattern,
    const Timestamp& validAfter,
    const std::vector<BSONObj>& splitPoints,
    const std::vector<ShardId>& allShardIds,
    const int numContiguousChunksPerShard) {
    invariant(!allShardIds.empty());

    std::vector<BSONObj> finalSplitPoints;

    // Make sure points are unique and ordered
    auto orderedPts = SimpleBSONObjComparator::kInstance.makeBSONObjSet();

    for (const auto& splitPoint : splitPoints) {
        orderedPts.insert(splitPoint);
    }

    for (const auto& splitPoint : orderedPts) {
        finalSplitPoints.push_back(splitPoint);
    }

    ChunkVersion version({OID::gen(), validAfter}, {1, 0});
    const auto& keyPattern(shardKeyPattern.getKeyPattern());

    std::vector<ChunkType> chunks;

    for (size_t i = 0; i <= finalSplitPoints.size(); i++) {
        const BSONObj min = (i == 0) ? keyPattern.globalMin() : finalSplitPoints[i - 1];
        const BSONObj max =
            (i < finalSplitPoints.size()) ? finalSplitPoints[i] : keyPattern.globalMax();
        const ShardId shardId = allShardIds[(i / numContiguousChunksPerShard) % allShardIds.size()];

        appendChunk(params, min, max, &version, shardId, &chunks);
    }

    return {std::move(chunks)};
}

std::unique_ptr<InitialSplitPolicy> InitialSplitPolicy::calculateOptimizationStrategy(
    OperationContext* opCtx,
    const ShardKeyPattern& shardKeyPattern,
    const std::int64_t numInitialChunks,
    const bool presplitHashedZones,
    const std::vector<TagsType>& tags,
    size_t numShards,
    bool collectionIsEmpty) {
    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "numInitialChunks is only supported when the collection is empty "
                             "and has a hashed field in the shard key pattern",
            !numInitialChunks || (shardKeyPattern.isHashedPattern() && collectionIsEmpty));
    uassert(ErrorCodes::InvalidOptions,
            str::stream()
                << "When the prefix of the hashed shard key is a range field, "
                   "'numInitialChunks' can only be used when the 'presplitHashedZones' is true",
            !numInitialChunks || shardKeyPattern.hasHashedPrefix() || presplitHashedZones);

    // If 'presplitHashedZones' flag is set, we always use 'PresplitHashedZonesSplitPolicy', to make
    // sure we throw the correct assertion if further validation fails.
    if (presplitHashedZones) {
        return std::make_unique<PresplitHashedZonesSplitPolicy>(
            opCtx, shardKeyPattern, tags, numInitialChunks, collectionIsEmpty);
    }

    // The next preference is to use split points based strategy. This is only possible if the
    // collection is empty with shard key having a hashed prefix.
    if (tags.empty() && shardKeyPattern.hasHashedPrefix() && collectionIsEmpty) {
        return std::make_unique<SplitPointsBasedSplitPolicy>(
            shardKeyPattern, numShards, numInitialChunks);
    }

    if (!tags.empty()) {
        if (collectionIsEmpty) {
            return std::make_unique<SingleChunkPerTagSplitPolicy>(opCtx, tags);
        }
        return std::make_unique<SingleChunkOnPrimarySplitPolicy>();
    }

    if (collectionIsEmpty) {
        return std::make_unique<SingleChunkOnPrimarySplitPolicy>();
    }

    return std::make_unique<SingleChunkOnPrimarySplitPolicy>();
}

InitialSplitPolicy::ShardCollectionConfig SingleChunkOnPrimarySplitPolicy::createFirstChunks(
    OperationContext* opCtx,
    const ShardKeyPattern& shardKeyPattern,
    const SplitPolicyParams& params) {
    const auto currentTime = VectorClock::get(opCtx)->getTime();
    const auto validAfter = currentTime.clusterTime().asTimestamp();

    ChunkVersion version({OID::gen(), validAfter}, {1, 0});
    const auto& keyPattern = shardKeyPattern.getKeyPattern();
    std::vector<ChunkType> chunks;
    appendChunk(params,
                keyPattern.globalMin(),
                keyPattern.globalMax(),
                &version,
                params.primaryShardId,
                &chunks);

    return {std::move(chunks)};
}

InitialSplitPolicy::ShardCollectionConfig SplitPointsBasedSplitPolicy::createFirstChunks(
    OperationContext* opCtx,
    const ShardKeyPattern& shardKeyPattern,
    const SplitPolicyParams& params) {

    // On which shards are the generated chunks allowed to be placed.
    const auto shardIds = getAllShardIdsShuffled(opCtx);

    const auto currentTime = VectorClock::get(opCtx)->getTime();
    const auto validAfter = currentTime.clusterTime().asTimestamp();
    return generateShardCollectionInitialChunks(
        params, shardKeyPattern, validAfter, _splitPoints, shardIds, _numContiguousChunksPerShard);
}

AbstractTagsBasedSplitPolicy::AbstractTagsBasedSplitPolicy(OperationContext* opCtx,
                                                           std::vector<TagsType> tags)
    : _tags(tags) {
    _tagToShardIds = buildTagsToShardIdsMap(opCtx, tags);
}

AbstractTagsBasedSplitPolicy::SplitInfo SingleChunkPerTagSplitPolicy::buildSplitInfoForTag(
    TagsType tag, const ShardKeyPattern& shardKeyPattern) {
    const auto nextShardIndex = _nextShardIndexForZone[tag.getTag()]++;
    const auto& shardIdsForTag = getTagsToShardIds().find(tag.getTag())->second;
    auto shardId = shardIdsForTag[nextShardIndex % shardIdsForTag.size()];

    // Do not generate any split points when using this strategy. We create one chunk on a shard
    // choosen using round-robin.
    return {{}, {std::make_pair(shardId, 1)}};
}

InitialSplitPolicy::ShardCollectionConfig AbstractTagsBasedSplitPolicy::createFirstChunks(
    OperationContext* opCtx,
    const ShardKeyPattern& shardKeyPattern,
    const SplitPolicyParams& params) {
    invariant(!_tags.empty());

    const auto shardIds = getAllShardIdsShuffled(opCtx);
    const auto currentTime = VectorClock::get(opCtx)->getTime();
    const auto validAfter = currentTime.clusterTime().asTimestamp();
    const auto& keyPattern = shardKeyPattern.getKeyPattern();

    auto tagToShards = getTagsToShardIds();

    auto nextShardIdForHole = [&, indx = 0L]() mutable {
        return shardIds[indx++ % shardIds.size()];
    };

    ChunkVersion version({OID::gen(), validAfter}, {1, 0});
    auto lastChunkMax = keyPattern.globalMin();
    std::vector<ChunkType> chunks;
    for (const auto& tag : _tags) {
        // Create a chunk for the hole [lastChunkMax, tag.getMinKey)
        if (tag.getMinKey().woCompare(lastChunkMax) > 0) {
            appendChunk(
                params, lastChunkMax, tag.getMinKey(), &version, nextShardIdForHole(), &chunks);
        }
        // Create chunk for the actual tag - [tag.getMinKey, tag.getMaxKey)
        const auto it = tagToShards.find(tag.getTag());
        invariant(it != tagToShards.end());
        uassert(50973,
                str::stream()
                    << "Cannot shard collection " << tag.getNS().toStringForErrorMsg()
                    << " due to zone " << tag.getTag()
                    << " which is not assigned to a shard. Please assign this zone to a shard.",
                !it->second.empty());

        // The buildSplitInfoForTag() should provide split points which are in sorted order. So we
        // don't need to sort them again while generating chunks.
        auto splitInfo = buildSplitInfoForTag(tag, shardKeyPattern);

        // Ensure that the number of splitPoints is consistent with the computed chunk distribution.
        // The resulting number of chunks will be one more than the number of split points to
        // accommodate boundaries.
        invariant(splitInfo.splitPoints.size() + 1 ==
                  std::accumulate(splitInfo.chunkDistribution.begin(),
                                  splitInfo.chunkDistribution.end(),
                                  static_cast<size_t>(0),  // initial value for 'runningSum'.
                                  [](size_t runningSum, const auto& currentElem) {
                                      return runningSum + currentElem.second;
                                  }));

        // Generate chunks using 'splitPoints' and distribute them among shards based on
        // 'chunkDistributionPerShard'.
        size_t splitPointIdx = 0;
        for (auto&& chunksOnShard : splitInfo.chunkDistribution) {
            const auto [targetShard, numChunksForShard] = chunksOnShard;
            for (size_t i = 0; i < numChunksForShard; ++i, ++splitPointIdx) {
                const BSONObj min = (splitPointIdx == 0) ? tag.getMinKey()
                                                         : splitInfo.splitPoints[splitPointIdx - 1];
                const BSONObj max = (splitPointIdx == splitInfo.splitPoints.size())
                    ? tag.getMaxKey()
                    : splitInfo.splitPoints[splitPointIdx];
                appendChunk(params, min, max, &version, targetShard, &chunks);
            }
        }
        lastChunkMax = tag.getMaxKey();
    }

    // Create a chunk for the hole [lastChunkMax, MaxKey]
    if (lastChunkMax.woCompare(keyPattern.globalMax()) < 0) {
        appendChunk(
            params, lastChunkMax, keyPattern.globalMax(), &version, nextShardIdForHole(), &chunks);
    }

    return {std::move(chunks)};
}

AbstractTagsBasedSplitPolicy::SplitInfo PresplitHashedZonesSplitPolicy::buildSplitInfoForTag(
    TagsType tag, const ShardKeyPattern& shardKeyPattern) {
    // Returns the ceiling number for the decimal value of x/y.
    auto ceilOfXOverY = [](auto x, auto y) {
        return (x / y) + (x % y != 0);
    };

    // This strategy presplits each tag such that at least 1 chunk is placed on every shard to which
    // the tag is assigned. We distribute the chunks such that at least '_numInitialChunks' are
    // created across the cluster, and we make a best-effort attempt to ensure that an equal number
    // of chunks are created on each shard regardless of how the zones are laid out.

    //  We take the ceiling when the number is not divisible so that the final number of chunks
    //  we generate are at least '_numInitialChunks'.
    auto numChunksPerShard = ceilOfXOverY(_numInitialChunks, _numTagsPerShard.size());

    const auto& tagsToShardsMap = getTagsToShardIds();
    invariant(tagsToShardsMap.find(tag.getTag()) != tagsToShardsMap.end());
    const auto& shardsForCurrentTag = tagsToShardsMap.find(tag.getTag())->second;

    // For each shard in the current zone, find the quota of chunks that can be allocated to that
    // zone. We distribute chunks equally to all the zones present on a shard.
    std::vector<std::pair<ShardId, size_t>> chunkDistribution;
    chunkDistribution.reserve((shardsForCurrentTag.size()));
    auto numChunksForCurrentTag = 0;
    for (auto&& shard : shardsForCurrentTag) {
        auto numChunksForCurrentTagOnShard =
            ceilOfXOverY(numChunksPerShard, _numTagsPerShard[shard.toString()]);
        chunkDistribution.push_back({shard, numChunksForCurrentTagOnShard});
        numChunksForCurrentTag += (numChunksForCurrentTagOnShard);
    }

    // Extract the fields preceding the hashed field. We use this object as a base for building
    // split points.
    BSONObjBuilder bob;
    for (auto&& elem : tag.getMinKey()) {
        if (elem.fieldNameStringData() == shardKeyPattern.getHashedField().fieldNameStringData()) {
            break;
        }
        bob.append(elem);
    }
    auto prefixBSON = bob.obj();

    return {calculateHashedSplitPoints(shardKeyPattern, prefixBSON, numChunksForCurrentTag),
            std::move(chunkDistribution)};
}

PresplitHashedZonesSplitPolicy::PresplitHashedZonesSplitPolicy(
    OperationContext* opCtx,
    const ShardKeyPattern& shardKeyPattern,
    std::vector<TagsType> tags,
    size_t numInitialChunks,
    bool isCollectionEmpty)
    : AbstractTagsBasedSplitPolicy(opCtx, tags) {
    // Verify that tags have been set up correctly for this split policy.
    _validate(shardKeyPattern, isCollectionEmpty);

    // Calculate the count of zones on each shard and save it in a map for later.
    const auto& tagsToShards = getTagsToShardIds();
    for (auto&& tag : tags) {
        auto& shardsForCurrentTag = tagsToShards.find(tag.getTag())->second;
        for (auto&& shard : shardsForCurrentTag) {
            _numTagsPerShard[shard.toString()]++;
        }
    }
    // If we are here, we have confirmed that at least one tag is already set up. A tag can only be
    // created if they are associated with a zone and the zone has to be assigned to a shard.
    invariant(!_numTagsPerShard.empty());

    // If 'numInitialChunks' was not specified, use default value.
    _numInitialChunks = numInitialChunks ? numInitialChunks : _numTagsPerShard.size() * 2;
}

/**
 * If 'presplitHashedZones' flag is set with shard key prefix being a non-hashed field then all
 * zones must be set up according to the following rules:
 *  1. All lower-bound prefix fields of the shard key must have a value other than MinKey or
 * MaxKey.
 *  2. All lower-bound fields from the hash field onwards must be MinKey.
 *  3. At least one upper-bound prefix field must be different than the lower bound counterpart.
 *
 * Examples for shard key {country : 1, hashedField: "hashed", suffix : 1}:
 * Zone with range : [{country : "US", hashedField: MinKey, suffix: MinKey}, {country :MaxKey,
 * hashedField: MaxKey, suffix: MaxKey}) is valid.
 * Zone with range : [{country : MinKey, hashedField: MinKey, suffix: MinKey}, {country : "US",
 * hashedField: MinKey, suffix: MinKey}) is invalid since it violates #1 rule.
 * Zone with range: [{country : "US", hashedField: MinKey, suffix: "someVal"}, {country :MaxKey,
 * hashedField: MaxKey, suffix: MaxKey}) is invalid since it violates #2 rule.
 * Zone with range: [{country : "US", hashedField: MinKey, suffix: MinKey}, {country : "US",
 * hashedField: MaxKey, suffix: MaxKey}) is invalid since it violates #3 rule.
 *
 * If the shard key has a hashed prefix, then pre-splitting is only supported if there is a single
 * zone defined from global MinKey to global MaxKey. i.e, if the shard key is {x: "hashed", y: 1}
 * then there should be exactly one zone ranging from {x:MinKey, y:MinKey} to {x:MaxKey, y:MaxKey}.
 */
void PresplitHashedZonesSplitPolicy::_validate(const ShardKeyPattern& shardKeyPattern,
                                               bool isCollectionEmpty) {
    const auto& tags = getTags();
    uassert(
        31387,
        "'presplitHashedZones' is only supported when the collection is empty, zones are set up "
        "and shard key pattern has a hashed field",
        isCollectionEmpty && !tags.empty() && shardKeyPattern.isHashedPattern());

    if (shardKeyPattern.hasHashedPrefix()) {
        uassert(31412,
                "For hashed prefix shard keys, 'presplitHashedZones' is only supported when there "
                "is a single zone defined which covers entire shard key range",
                (tags.size() == 1) &&
                    !shardKeyPattern.getKeyPattern().globalMin().woCompare(tags[0].getMinKey()) &&
                    !shardKeyPattern.getKeyPattern().globalMax().woCompare(tags[0].getMaxKey()));
        return;
    }
    for (auto&& tag : tags) {
        auto startItr = BSONObjIterator(tag.getMinKey());
        auto endItr = BSONObjIterator(tag.getMaxKey());

        // We cannot pre-split if the lower bound fields preceding the hashed field are same as
        // the upper bound. We validate that at least one of the preceding field is different.
        // Additionally we all make sure that none of the lower-bound prefix fields have Minkey
        // or MaxKey.
        bool isPrefixDifferent = false;
        do {
            uassert(31388,
                    str::stream()
                        << "One or more zones are not defined in a manner that supports hashed "
                           "pre-splitting. Cannot have MinKey or MaxKey in the lower bound for "
                           "fields preceding the hashed field but found one, for zone "
                        << tag.getTag(),
                    (*startItr).type() != BSONType::MinKey &&
                        (*startItr).type() != BSONType::MaxKey);
            isPrefixDifferent = isPrefixDifferent || (*startItr).woCompare(*endItr);
            ++endItr;
            // Forward the iterator until hashed field is reached.
        } while ((*++startItr).fieldNameStringData() !=
                 shardKeyPattern.getHashedField().fieldNameStringData());
        uassert(31390,
                str::stream() << "One or more zones are not defined in a manner that supports "
                                 "hashed pre-splitting. The value preceding hashed field of the "
                                 "upper bound should be greater than that of lower bound, for zone "
                              << tag.getTag(),
                isPrefixDifferent);

        uassert(
            31389,
            str::stream() << "One or more zones are not defined in a manner that supports "
                             "hashed pre-splitting. The hashed field value for lower bound must "
                             "be MinKey, for zone "
                          << tag.getTag(),
            (*startItr).type() == BSONType::MinKey);

        // Each field in the lower bound after the hashed field must be set to MinKey.
        while (startItr.more()) {
            uassert(31391,
                    str::stream() << "One or more zones are not defined in a manner that supports "
                                     "hashed pre-splitting. The fields after the hashed field must "
                                     "have MinKey value, for zone "
                                  << tag.getTag(),
                    (*startItr++).type() == BSONType::MinKey);
        }
    }
}

std::vector<BSONObj> SamplingBasedSplitPolicy::createRawPipeline(const ShardKeyPattern& shardKey,
                                                                 int numInitialChunks,
                                                                 int samplesPerChunk) {

    std::vector<BSONObj> res;
    const auto& shardKeyFields = shardKey.getKeyPatternFields();
    BSONObjBuilder sortValBuilder;
    using Doc = Document;
    using Arr = std::vector<Value>;
    using V = Value;
    Arr arrayToObjectBuilder;
    for (auto&& fieldRef : shardKeyFields) {
        // If the shard key includes a hashed field and current fieldRef is the hashed field.
        if (shardKey.isHashedPattern() &&
            fieldRef->dottedField().compare(shardKey.getHashedField().fieldNameStringData()) == 0) {
            arrayToObjectBuilder.emplace_back(
                Doc{{"k", V{fieldRef->dottedField()}},
                    {"v", Doc{{"$toHashedIndexKey", V{"$" + fieldRef->dottedField()}}}}});
        } else {
            arrayToObjectBuilder.emplace_back(Doc{
                {"k", V{fieldRef->dottedField()}},
                {"v", Doc{{"$ifNull", V{Arr{V{"$" + fieldRef->dottedField()}, V{BSONNULL}}}}}}});
        }
        sortValBuilder.append(fieldRef->dottedField().toString(), 1);
    }
    res.push_back(BSON("$sample" << BSON("size" << numInitialChunks * samplesPerChunk)));
    res.push_back(BSON("$sort" << sortValBuilder.obj()));
    res.push_back(
        Doc{{"$replaceWith", Doc{{"$arrayToObject", Arr{V{arrayToObjectBuilder}}}}}}.toBson());
    return res;
}

SamplingBasedSplitPolicy SamplingBasedSplitPolicy::make(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ShardKeyPattern& shardKey,
    int numInitialChunks,
    boost::optional<std::vector<TagsType>> zones,
    int samplesPerChunk) {
    uassert(4952603, "samplesPerChunk should be > 0", samplesPerChunk > 0);
    return SamplingBasedSplitPolicy(
        numInitialChunks,
        zones,
        _makePipelineDocumentSource(opCtx, nss, shardKey, numInitialChunks, samplesPerChunk));
}

SamplingBasedSplitPolicy::SamplingBasedSplitPolicy(int numInitialChunks,
                                                   boost::optional<std::vector<TagsType>> zones,
                                                   std::unique_ptr<SampleDocumentSource> samples)
    : _numInitialChunks(numInitialChunks), _zones(std::move(zones)), _samples(std::move(samples)) {
    uassert(4952602, "numInitialChunks should be > 0", numInitialChunks > 0);
    uassert(4952604, "provided zones should not be empty", !_zones || _zones->size());
}

BSONObjSet SamplingBasedSplitPolicy::createFirstSplitPoints(OperationContext* opCtx,
                                                            const ShardKeyPattern& shardKey,
                                                            const SplitPolicyParams& params) {
    if (_zones) {
        for (auto& zone : *_zones) {
            zone.setMinKey(shardKey.getKeyPattern().extendRangeBound(zone.getMinKey(), false));
            zone.setMaxKey(shardKey.getKeyPattern().extendRangeBound(zone.getMaxKey(), false));
        }
    }

    auto splitPoints = extractSplitPointsFromZones(shardKey, _zones);
    if (splitPoints.size() < static_cast<size_t>(_numInitialChunks - 1)) {
        // The BlockingResultsMerger underlying the $mergeCursors stage records how long was
        // spent waiting for samples from the donor shards. It doing so requires the CurOp
        // to be marked as having started.
        CurOp::get(opCtx)->ensureStarted();

        _appendSplitPointsFromSample(
            &splitPoints, shardKey, _numInitialChunks - splitPoints.size() - 1);
    }

    uassert(4952606,
            str::stream() << "The shard key provided does not have enough cardinality to make the "
                             "required number of chunks of "
                          << _numInitialChunks << ", it can only make " << (splitPoints.size() + 1)
                          << " chunks",
            splitPoints.size() >= static_cast<size_t>(_numInitialChunks - 1));

    return splitPoints;
}

InitialSplitPolicy::ShardCollectionConfig SamplingBasedSplitPolicy::createFirstChunks(
    OperationContext* opCtx, const ShardKeyPattern& shardKey, const SplitPolicyParams& params) {
    auto splitPoints = createFirstSplitPoints(opCtx, shardKey, params);

    ZoneShardMap zoneToShardMap;
    ChunkDistributionMap chunkDistribution;

    ZoneInfo zoneInfo;
    if (_zones) {
        zoneToShardMap = buildTagsToShardIdsMap(opCtx, *_zones);

        for (const auto& zone : *_zones) {
            uassertStatusOK(
                zoneInfo.addRangeToZone({zone.getMinKey(), zone.getMaxKey(), zone.getTag()}));
        }
    }

    {
        auto allShardIds = getAllShardIdsShuffled(opCtx);
        for (const auto& shard : allShardIds) {
            chunkDistribution.emplace(shard, 0);
        }

        zoneToShardMap.emplace("", std::move(allShardIds));
    }

    std::vector<ChunkType> chunks;

    const auto& keyPattern = shardKey.getKeyPattern();
    const auto currentTime = VectorClock::get(opCtx)->getTime();
    const auto validAfter = currentTime.clusterTime().asTimestamp();

    ChunkVersion version({OID::gen(), validAfter}, {1, 0});
    auto lastChunkMax = keyPattern.globalMin();

    auto selectShardAndAppendChunk = [&](const BSONObj& chunkMin, const BSONObj& chunkMax) {
        auto bestShard =
            selectBestShard(chunkDistribution, zoneInfo, zoneToShardMap, {chunkMin, chunkMax});
        appendChunk(params, chunkMin, chunkMax, &version, bestShard, &chunks);
        chunkDistribution[bestShard]++;
        lastChunkMax = chunkMax;
    };

    for (const auto& splitPoint : splitPoints) {
        selectShardAndAppendChunk(lastChunkMax, splitPoint);
    }
    selectShardAndAppendChunk(lastChunkMax, keyPattern.globalMax());

    return {std::move(chunks)};
}

void SamplingBasedSplitPolicy::_appendSplitPointsFromSample(BSONObjSet* splitPoints,
                                                            const ShardKeyPattern& shardKey,
                                                            int nToAppend) {
    int nRemaining = nToAppend;
    auto nextKey = _samples->getNext();

    while (nextKey && nRemaining > 0) {
        // if key is hashed, nextKey values are already hashed
        auto result = splitPoints->insert(nextKey->getOwned());
        if (result.second) {
            nRemaining--;
        }
        nextKey = _samples->getNext();
    }
}

std::unique_ptr<SamplingBasedSplitPolicy::SampleDocumentSource>
SamplingBasedSplitPolicy::makePipelineDocumentSource_forTest(OperationContext* opCtx,
                                                             const NamespaceString& ns,
                                                             const ShardKeyPattern& shardKey,
                                                             int numInitialChunks,
                                                             int samplesPerChunk) {
    MakePipelineOptions opts;
    opts.attachCursorSource = false;
    return _makePipelineDocumentSource(
        opCtx, ns, shardKey, numInitialChunks, samplesPerChunk, std::move(opts));
}

std::unique_ptr<SamplingBasedSplitPolicy::SampleDocumentSource>
SamplingBasedSplitPolicy::_makePipelineDocumentSource(OperationContext* opCtx,
                                                      const NamespaceString& ns,
                                                      const ShardKeyPattern& shardKey,
                                                      int numInitialChunks,
                                                      int samplesPerChunk,
                                                      MakePipelineOptions opts) {
    auto rawPipeline = createRawPipeline(shardKey, numInitialChunks, samplesPerChunk);
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
    resolvedNamespaces[ns.coll()] = {ns, std::vector<BSONObj>{}};

    auto pi = [&]() -> std::shared_ptr<MongoProcessInterface> {
        if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer) ||
            serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
            // For the pipeline to be dispatched to shards, the ShardServerProcessInterface must be
            // used. However, the generic factory would only return a ShardServerProcessInterface
            // if the mongod is a shardsvr and the connection is internal. That is, if the mongod is
            // a configsvr or a shardsvr but connected directly, the factory would return a
            // StandaloneProcessInterface. Given this, we need to manually crate a
            // ShardServerProcessInterface here instead of using the generic factory.
            return std::make_shared<ShardServerProcessInterface>(
                Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor());
        }
        return MongoProcessInterface::create(opCtx);
    }();

    auto expCtx = make_intrusive<ExpressionContext>(opCtx,
                                                    boost::none, /* explain */
                                                    false,       /* fromMongos */
                                                    false,       /* needsMerge */
                                                    true,        /* allowDiskUse */
                                                    true,        /* bypassDocumentValidation */
                                                    false,       /* isMapReduceCommand */
                                                    ns,
                                                    boost::none, /* runtimeConstants */
                                                    nullptr,     /* collator */
                                                    std::move(pi),
                                                    std::move(resolvedNamespaces),
                                                    boost::none); /* collUUID */

    expCtx->tempDir = storageGlobalParams.dbpath + "/_tmp";

    return std::make_unique<PipelineDocumentSource>(
        Pipeline::makePipeline(rawPipeline, expCtx, opts), samplesPerChunk - 1);
}

SamplingBasedSplitPolicy::PipelineDocumentSource::PipelineDocumentSource(
    SampleDocumentPipeline pipeline, int skip)
    : _pipeline(std::move(pipeline)), _skip(skip) {}

boost::optional<BSONObj> SamplingBasedSplitPolicy::PipelineDocumentSource::getNext() {
    auto val = _pipeline->getNext();

    if (!val) {
        return boost::none;
    }

    for (int skippedSamples = 0; skippedSamples < _skip; skippedSamples++) {
        auto newVal = _pipeline->getNext();

        if (!newVal) {
            // If there are not enough samples, just select the last sample.
            break;
        }

        val = newVal;
    }

    return val->toBson();
}

ShardDistributionSplitPolicy ShardDistributionSplitPolicy::make(
    OperationContext* opCtx,
    const ShardKeyPattern& shardKey,
    std::vector<ShardKeyRange> shardDistribution,
    boost::optional<std::vector<TagsType>> zones) {
    uassert(7661501, "ShardDistribution should not be empty", shardDistribution.size() > 0);
    return ShardDistributionSplitPolicy(shardDistribution, zones);
}

ShardDistributionSplitPolicy::ShardDistributionSplitPolicy(
    std::vector<ShardKeyRange>& shardDistribution, boost::optional<std::vector<TagsType>> zones)
    : _shardDistribution(std::move(shardDistribution)), _zones(std::move(zones)) {}

InitialSplitPolicy::ShardCollectionConfig ShardDistributionSplitPolicy::createFirstChunks(
    OperationContext* opCtx,
    const ShardKeyPattern& shardKeyPattern,
    const SplitPolicyParams& params) {
    const auto& keyPattern = shardKeyPattern.getKeyPattern();
    if (_zones) {
        for (auto& zone : *_zones) {
            zone.setMinKey(keyPattern.extendRangeBound(zone.getMinKey(), false));
            zone.setMaxKey(keyPattern.extendRangeBound(zone.getMaxKey(), false));
        }
    }

    auto splitPoints = extractSplitPointsFromZones(shardKeyPattern, _zones);
    std::vector<ChunkType> chunks;
    uassert(ErrorCodes::InvalidOptions,
            "ShardDistribution without min/max is not supported.",
            _shardDistribution[0].getMin());

    unsigned long shardDistributionIdx = 0;
    const auto currentTime = VectorClock::get(opCtx)->getTime();
    const auto validAfter = currentTime.clusterTime().asTimestamp();
    ChunkVersion version({OID::gen(), validAfter}, {1, 0});
    for (const auto& splitPoint : splitPoints) {
        _appendChunks(params, splitPoint, keyPattern, shardDistributionIdx, version, chunks);
    }
    _appendChunks(
        params, keyPattern.globalMax(), keyPattern, shardDistributionIdx, version, chunks);

    if (_zones) {
        _checkShardsMatchZones(opCtx, chunks, *_zones);
    }

    return {std::move(chunks)};
}

void ShardDistributionSplitPolicy::_appendChunks(const SplitPolicyParams& params,
                                                 const BSONObj& splitPoint,
                                                 const KeyPattern& keyPattern,
                                                 unsigned long& shardDistributionIdx,
                                                 ChunkVersion& version,
                                                 std::vector<ChunkType>& chunks) {
    while (shardDistributionIdx < _shardDistribution.size()) {
        auto shardMin =
            keyPattern.extendRangeBound(*_shardDistribution[shardDistributionIdx].getMin(), false);
        auto shardMax =
            keyPattern.extendRangeBound(*_shardDistribution[shardDistributionIdx].getMax(), false);
        auto lastChunkMax =
            chunks.empty() ? keyPattern.globalMin() : chunks.back().getRange().getMax();
        /* When we compare a defined shard range with a splitPoint, there are three cases:
         * 1. The whole shard range is on the left side of the splitPoint -> Add this shard as a
         * whole chunk and move to next shard.
         * 2. The splitPoint is in the middle of the shard range. -> Append (shardMin,
         * splitPoint) as a chunk and move to next split point.
         * 3. The whole shard range is on the right side of the splitPoint -> Move to the next
         * splitPoint.
         * This algorithm relies on the shardDistribution is continuous and complete to be
         * correct, which is validated in the cmd handler.
         */
        if (SimpleBSONObjComparator::kInstance.evaluate(shardMin < splitPoint)) {
            // The whole shard range is on the left side of the splitPoint.
            if (SimpleBSONObjComparator::kInstance.evaluate(shardMax <= splitPoint)) {
                appendChunk(params,
                            lastChunkMax,
                            shardMax,
                            &version,
                            _shardDistribution[shardDistributionIdx].getShard(),
                            &chunks);
                lastChunkMax = shardMax;
                shardDistributionIdx++;
            } else {  // The splitPoint is in the middle of the shard range.
                appendChunk(params,
                            lastChunkMax,
                            splitPoint,
                            &version,
                            _shardDistribution[shardDistributionIdx].getShard(),
                            &chunks);
                lastChunkMax = splitPoint;
                return;
            }
        } else {  // The whole shard range is on the right side of the splitPoint.
            return;
        }
    }
}

void ShardDistributionSplitPolicy::_checkShardsMatchZones(
    OperationContext* opCtx,
    const std::vector<ChunkType>& chunks,
    const std::vector<mongo::TagsType>& zones) {
    ZoneInfo zoneInfo;
    auto shardIdToTags = buildShardIdToTagsMap(opCtx, _shardDistribution);
    for (const auto& zone : zones) {
        uassertStatusOK(
            zoneInfo.addRangeToZone({zone.getMinKey(), zone.getMaxKey(), zone.getTag()}));
    }

    for (const auto& chunk : chunks) {
        auto zoneFromCmdParameter = zoneInfo.getZoneForChunk({chunk.getMin(), chunk.getMax()});
        auto iter = shardIdToTags.find(chunk.getShard());
        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "Specified zones and shardDistribution are conflicting with the "
                                 "existing shard/zone, shard "
                              << chunk.getShard() << "doesn't belong to zone "
                              << zoneFromCmdParameter,
                iter != shardIdToTags.end() &&
                    iter->second.find(zoneFromCmdParameter) != iter->second.end());
    }
}

}  // namespace mongo
